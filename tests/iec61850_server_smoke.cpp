// SPDX-License-Identifier: GPL-3.0-only
#include "acquisition/acquisition.hpp"
#include "domain/snapshot.hpp"
#include "iec61850_client.h"
#include "iec61850/server.hpp"

#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <thread>

namespace {

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

    uhf::iec61850::Server server(
        snapshots, uhf::iec61850::ServerOptions{"127.0.0.1", 15102U, "TESTIED"});
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
