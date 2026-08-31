#!/usr/bin/env python3
"""Check that the published generic IEC model description is complete."""

from pathlib import Path
import sys
import xml.etree.ElementTree as ET


def fail(message: str) -> None:
    raise SystemExit(f"IEC 61850 ICD smoke failed: {message}")


def main() -> int:
    if len(sys.argv) != 2:
        fail("expected ICD path")
    path = Path(sys.argv[1])
    try:
        root = ET.parse(path).getroot()
    except (ET.ParseError, OSError) as error:
        fail(f"unable to parse ICD: {error}")

    namespace = "{http://www.iec.ch/61850/2003/SCL}"
    if root.tag != namespace + "SCL":
        fail("unexpected SCL namespace")
    ied = root.find(namespace + "IED")
    if ied is None or ied.get("name") != "UHFPD1":
        fail("IED name is not UHFPD1")
    ldevice = root.find(".//" + namespace + "LDevice")
    if ldevice is None or ldevice.get("inst") != "PDMON":
        fail("PDMON logical device is missing")

    expected_nodes = {"LLN0", "LPHD", "SPDC", "GGIO"}
    actual_nodes = {node.get("lnClass") for node in ldevice if node.tag in (namespace + "LN", namespace + "LN0")}
    if actual_nodes != expected_nodes:
        fail(f"logical node set mismatch: {actual_nodes!r}")

    dataset = ldevice.find(".//" + namespace + "DataSet[@name='DSMeasurements']")
    if dataset is None or len(dataset.findall(namespace + "FCDA")) != 6:
        fail("static six-member dataset is missing")
    report = ldevice.find(".//" + namespace + "ReportControl[@name='RPMeasurements']")
    if report is None or report.get("buffered") != "false" or report.get("intgPd") != "60000":
        fail("unbuffered 60-second report is missing")

    text = path.read_text(encoding="utf-8")
    for reference in (
        'doName="AnIn1"', 'doName="IntIn1"', 'doName="AnIn2"',
        'doName="AnIn3"', 'doName="AnIn4"', 'doName="PaDschAlm"',
        'name="UhfPaDsch"',
    ):
        if reference not in text:
            fail(f"model member missing: {reference}")
    print("IEC 61850 ICD smoke: OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
