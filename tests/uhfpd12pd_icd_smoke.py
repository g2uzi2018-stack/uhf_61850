#!/usr/bin/env python3
"""Check the completed telemetry/state model in UHFPD12PD.icd."""

from pathlib import Path
import sys
import xml.etree.ElementTree as ET


SCL = "{http://www.iec.ch/61850/2003/SCL}"


def fail(message: str) -> None:
    raise SystemExit(f"UHFPD12PD ICD smoke failed: {message}")


def main() -> int:
    if len(sys.argv) != 2:
        fail("expected ICD path")
    path = Path(sys.argv[1])
    try:
        root = ET.parse(path).getroot()
    except (ET.ParseError, OSError) as error:
        fail(f"unable to parse ICD: {error}")

    if root.tag != SCL + "SCL":
        fail("unexpected SCL namespace")

    ied = root.find(SCL + "IED")
    if ied is None or ied.get("name") != "UHFPD12PD":
        fail("UHFPD12PD IED is missing")

    ldevice = ied.find(".//" + SCL + "LDevice")
    if ldevice is None or ldevice.get("inst") != "MON":
        fail("MON logical device is missing")
    ln0 = ldevice.find(SCL + "LN0")
    if ln0 is None:
        fail("LLN0 is missing")

    datasets = {dataset.get("name"): dataset for dataset in ln0.findall(SCL + "DataSet")}
    for name in ("DSMeasurements", "DSState"):
        if name not in datasets:
            fail(f"{name} data set is missing")
    if datasets["DSMeasurements"].get("desc") != "局部放电遥测数据集":
        fail("telemetry data set description is missing")
    if datasets["DSState"].get("desc") != "局部放电遥信数据集":
        fail("state data set description is missing")

    state_members = {
        (
            fcda.get("ldInst"),
            fcda.get("lnClass"),
            fcda.get("lnInst"),
            fcda.get("doName"),
            fcda.get("daName"),
            fcda.get("fc"),
        )
        for fcda in datasets["DSState"].findall(SCL + "FCDA")
    }
    if state_members != {("MON", "SPDC", "1", "PaDschAlm", "stVal", "ST")}:
        fail(f"unexpected state data set members: {state_members!r}")

    reports = {report.get("name"): report for report in ln0.findall(SCL + "ReportControl")}
    expected_report_datasets = {
        "RPMeasurements": "DSMeasurements",
        "RPState": "DSState",
    }
    if set(expected_report_datasets) - set(reports):
        fail("telemetry/state report control is missing")
    for report_name, dataset_name in expected_report_datasets.items():
        report = reports[report_name]
        if report.get("datSet") != dataset_name:
            fail(f"{report_name} is not linked to {dataset_name}")
        if report.get("confRev") != "1" or report.get("intgPd") != "60000":
            fail(f"{report_name} has unexpected revision or integrity period")
        trigger_ops = report.find(SCL + "TrgOps")
        if trigger_ops is None or trigger_ops.get("dchg") != "true" or trigger_ops.get("qchg") != "true":
            fail(f"{report_name} lacks data/quality change triggers")
        if trigger_ops.get("period") != "true":
            fail(f"{report_name} lacks periodic trigger")
        options = report.find(SCL + "OptFields")
        if options is None or any(options.get(name) != "true" for name in (
            "seqNum", "timeStamp", "dataSet", "reasonCode", "dataRef")):
            fail(f"{report_name} lacks required report option fields")

    services = ied.find(SCL + "Services")
    report_service = None if services is None else services.find(SCL + "ConfReportControl")
    try:
        max_reports = int(report_service.get("max", "0")) if report_service is not None else 0
    except ValueError:
        max_reports = 0
    if max_reports < 2:
        fail("ConfReportControl capacity is below the two configured reports")

    print("UHFPD12PD ICD smoke: OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
