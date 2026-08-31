// SPDX-License-Identifier: GPL-3.0-only
#include "acquisition/acquisition.hpp"
#include "domain/snapshot.hpp"
#include "iec61850_client.h"
#include "iec61850/server.hpp"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <thread>
#include <utility>

namespace {

struct ReportState {
    std::atomic<unsigned int> count{0U};
};

void report_callback(void* parameter, ClientReport report) {
    auto* state = static_cast<ReportState*>(parameter);
    if (ClientReport_getDataSetValues(report) != nullptr) {
        state->count.fetch_add(1U);
    }
}

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
    }
    return condition;
}

}  // namespace

int main() {
    uhf::acquisition::SnapshotStore snapshots;
    uhf::domain::ParsedSnapshot payload;
    payload.payload_status = uhf::domain::PayloadStatus::good;
    payload.measurements[0] = uhf::domain::Measurement{0U, -55, true};
    payload.measurements[1] = uhf::domain::Measurement{0U, 12, true};
    payload.measurements[2] = uhf::domain::Measurement{0U, -50, true};
    payload.measurements[3] = uhf::domain::Measurement{0U, 180, true};
    payload.measurements[4] = uhf::domain::Measurement{0U, 240, true};
    const auto now = std::chrono::steady_clock::now();
    snapshots.publish(payload, now, now);

    uhf::iec61850::ServerOptions server_options;
    server_options.bind_address = "127.0.0.1";
    server_options.port = 15102U;
    server_options.ied_name = "TESTIED";
    server_options.alarm_provider = [] { return true; };
    uhf::iec61850::Server server(snapshots, std::move(server_options));
    server.start();
    bool ok = expect(server.running(), "MMS server is running");

    IedConnection connection = IedConnection_create();
    IedClientError error = IED_ERROR_OK;
    IedConnection_setConnectTimeout(connection, 1000U);
    IedConnection_connect(connection, &error, "127.0.0.1", 15102);
    ok = expect(error == IED_ERROR_OK, "MMS client connects") && ok;

    if (error == IED_ERROR_OK) {
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        MmsValue* value = IedConnection_readObject(
            connection,
            &error,
            "TESTIEDPDMON/GGIO1.AnIn1.mag.f",
            IEC61850_FC_MX);
        ok = expect(error == IED_ERROR_OK && value != nullptr, "read first measurement") && ok;
        if (value != nullptr) {
            ok = expect(
                MmsValue_getType(value) == MMS_FLOAT &&
                    std::fabs(static_cast<double>(MmsValue_toFloat(value)) + 55.0) < 0.01,
                "first measurement value") && ok;
            MmsValue_delete(value);
        }

        const Quality fresh_quality = IedConnection_readQualityValue(
            connection,
            &error,
            "TESTIEDPDMON/GGIO1.AnIn1.q",
            IEC61850_FC_MX);
        ok = expect(
            error == IED_ERROR_OK && fresh_quality == static_cast<Quality>(QUALITY_VALIDITY_GOOD),
            "fresh measurement quality") && ok;

        snapshots.record_failure(std::chrono::steady_clock::now(), "serial timeout");
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        error = IED_ERROR_OK;
        const Quality stale_quality = IedConnection_readQualityValue(
            connection,
            &error,
            "TESTIEDPDMON/GGIO1.AnIn1.q",
            IEC61850_FC_MX);
        ok = expect(
            error == IED_ERROR_OK &&
                stale_quality == static_cast<Quality>(
                    QUALITY_VALIDITY_QUESTIONABLE | QUALITY_DETAIL_OLD_DATA),
            "stale measurement quality") && ok;
        MmsValue* stale_value = IedConnection_readObject(
            connection,
            &error,
            "TESTIEDPDMON/GGIO1.AnIn1.mag.f",
            IEC61850_FC_MX);
        ok = expect(
            error == IED_ERROR_OK && stale_value != nullptr &&
                std::fabs(static_cast<double>(MmsValue_toFloat(stale_value)) + 55.0) < 0.01,
            "stale measurement retains last-good value") && ok;
        if (stale_value != nullptr) {
            MmsValue_delete(stale_value);
        }
        const auto recovered_at = std::chrono::steady_clock::now();
        snapshots.publish(payload, recovered_at, recovered_at);
        std::this_thread::sleep_for(std::chrono::milliseconds(150));

        MmsValue* frequency = IedConnection_readObject(
            connection,
            &error,
            "TESTIEDPDMON/GGIO1.IntIn1.stVal",
            IEC61850_FC_ST);
        ok = expect(error == IED_ERROR_OK && frequency != nullptr, "read frequency") && ok;
        if (frequency != nullptr) {
            ok = expect(
                MmsValue_getType(frequency) == MMS_INTEGER && MmsValue_toInt32(frequency) == 12,
                "frequency value") && ok;
            MmsValue_delete(frequency);
        }

        MmsValue* peak = IedConnection_readObject(
            connection,
            &error,
            "TESTIEDPDMON/SPDC1.UhfPaDsch.mag.f",
            IEC61850_FC_MX);
        ok = expect(error == IED_ERROR_OK && peak != nullptr, "read standard peak") && ok;
        if (peak != nullptr) {
            ok = expect(
                std::fabs(static_cast<double>(MmsValue_toFloat(peak)) + 50.0) < 0.01,
                "standard peak value") && ok;
            MmsValue_delete(peak);
        }

        MmsValue* alarm = IedConnection_readObject(
            connection,
            &error,
            "TESTIEDPDMON/SPDC1.PaDschAlm.stVal",
            IEC61850_FC_ST);
        ok = expect(error == IED_ERROR_OK && alarm != nullptr, "read event alarm") && ok;
        if (alarm != nullptr) {
            ok = expect(MmsValue_getBoolean(alarm), "shared event alarm state") && ok;
            MmsValue_delete(alarm);
        }

        ClientDataSet data_set = IedConnection_readDataSetValues(
            connection, &error, "TESTIEDPDMON/LLN0.DSMeasurements", nullptr);
        ok = expect(error == IED_ERROR_OK && data_set != nullptr, "read static data set") && ok;
        if (data_set != nullptr) {
            MmsValue* values = ClientDataSet_getValues(data_set);
            ok = expect(
                ClientDataSet_getDataSetSize(data_set) == 6 && values != nullptr &&
                    MmsValue_getArraySize(values) == 6U,
                "static data set has five measurements and alarm") && ok;
            ClientDataSet_destroy(data_set);
        }

        ClientReportControlBlock rcb = IedConnection_getRCBValues(
            connection, &error, "TESTIEDPDMON/LLN0.RP.RPMeasurements", nullptr);
        ok = expect(error == IED_ERROR_OK && rcb != nullptr, "read URCB") && ok;
        if (rcb != nullptr) {
            ok = expect(!ClientReportControlBlock_isBuffered(rcb), "report is unbuffered") && ok;
            ReportState report_state;
            IedConnection_installReportHandler(
                connection,
                "TESTIEDPDMON/LLN0.RP.RPMeasurements",
                ClientReportControlBlock_getRptId(rcb),
                report_callback,
                &report_state);
            ClientReportControlBlock_setTrgOps(
                rcb, TRG_OPT_DATA_CHANGED | TRG_OPT_INTEGRITY);
            ClientReportControlBlock_setRptEna(rcb, true);
            IedConnection_setRCBValues(
                connection,
                &error,
                rcb,
                RCB_ELEMENT_RPT_ENA | RCB_ELEMENT_TRG_OPS,
                true);
            ok = expect(error == IED_ERROR_OK, "enable URCB") && ok;

            uhf::domain::ParsedSnapshot changed = payload;
            changed.measurements[2] = uhf::domain::Measurement{0U, -40, true};
            const auto changed_at = std::chrono::steady_clock::now();
            snapshots.publish(changed, changed_at, changed_at);
            const auto report_deadline = std::chrono::steady_clock::now() +
                std::chrono::seconds(2);
            while (report_state.count.load() == 0U &&
                   std::chrono::steady_clock::now() < report_deadline) {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
            ok = expect(report_state.count.load() > 0U, "receive data-change report") && ok;

            ClientReportControlBlock_setRptEna(rcb, false);
            IedConnection_setRCBValues(
                connection, &error, rcb, RCB_ELEMENT_RPT_ENA, true);
            ok = expect(error == IED_ERROR_OK, "disable URCB") && ok;
            ClientReportControlBlock_destroy(rcb);
        }
        IedConnection_close(connection);
    }

    IedConnection_destroy(connection);
    server.stop();
    ok = expect(!server.running(), "MMS server stops") && ok;
    if (ok) {
        std::cout << "IEC 61850 MMS server smoke: OK\n";
    }
    return ok ? 0 : 1;
}
