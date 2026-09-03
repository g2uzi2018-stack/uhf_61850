// SPDX-License-Identifier: GPL-3.0-only
#include "acquisition/acquisition.hpp"
#include "domain/snapshot.hpp"
#include "iec61850_client.h"
#include "iec61850/scl_model.hpp"
#include "iec61850/server.hpp"
#include "linked_list.h"

extern "C" {
#include "hal_thread.h"
#include "mms_mapping.h"
#include "mms_mapping_internal.h"
#include "mms_server_internal.h"
#include "mms_server_libinternal.h"
#include "ied_server_private.h"
#include "reporting.h"
}

#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <fstream>
#include <iterator>
#include <string>
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

bool report_buffer_storm_is_bounded() {
    uhf::iec61850::Model model("REPORTIED");
    IedServerConfig config = IedServerConfig_create();
    if (config == nullptr) {
        return false;
    }
    IedServerConfig_setReportBufferSize(config, 65536);
    IedServerConfig_setReportBufferSizeForURCBs(config, 65536);
    IedServer raw_server = IedServer_createWithConfig(model.raw(), nullptr, config);
    IedServerConfig_destroy(config);
    if (raw_server == nullptr) {
        return false;
    }

    auto* private_server = reinterpret_cast<sIedServer*>(raw_server);
    IedServer_start(raw_server, 15104);
    bool bounded = IedServer_isRunning(raw_server);
    IedConnection client = nullptr;
    ClientReportControlBlock rcb = nullptr;
    if (bounded) {
        client = IedConnection_create();
        bounded = client != nullptr;
        if (bounded) {
            IedConnection_setConnectTimeout(client, 1000U);
            IedClientError error = IED_ERROR_OK;
            IedConnection_connect(client, &error, "127.0.0.1", 15104);
            bounded = error == IED_ERROR_OK;
            if (bounded) {
                rcb = IedConnection_getRCBValues(
                    client,
                    &error,
                    "REPORTIEDPDMON/LLN0.RP.RPMeasurements",
                    nullptr);
                bounded = error == IED_ERROR_OK && rcb != nullptr;
            }
            if (bounded) {
                ClientReportControlBlock_setTrgOps(
                    rcb, TRG_OPT_DATA_CHANGED | TRG_OPT_QUALITY_CHANGED);
                ClientReportControlBlock_setRptEna(rcb, true);
                IedConnection_setRCBValues(
                    client,
                    &error,
                    rcb,
                    RCB_ELEMENT_RPT_ENA | RCB_ELEMENT_TRG_OPS,
                    true);
                bounded = error == IED_ERROR_OK;
            }
        }
    }

    MmsMapping* mapping = private_server->mmsMapping;
    LinkedList element = mapping == nullptr ? nullptr : LinkedList_getNext(mapping->reportControls);
    auto* report = element == nullptr
        ? nullptr
        : reinterpret_cast<ReportControl*>(LinkedList_getData(element));
    bounded = bounded &&
        report != nullptr &&
        report->dataSet != nullptr &&
        report->inclusionFlags != nullptr &&
        report->reportBuffer != nullptr &&
        report->reportBuffer->memoryBlockSize == 65536;
    if (bounded) {
        // Keep the enabled report queued without letting the connection worker drain it.
        report->clientConnection = nullptr;
        report->enabled = true;
        constexpr std::size_t kUpdateCount = 12000U;
        for (std::size_t index = 0U; index < kUpdateCount; ++index) {
            ReportControl_valueUpdated(
                report, 0, REPORT_CONTROL_VALUE_CHANGED, false);
        }
        const ReportBuffer* buffer = report->reportBuffer;
        const std::size_t maximum_entries =
            static_cast<std::size_t>(buffer->memoryBlockSize) / sizeof(ReportBufferEntry);
        bounded = buffer->reportsCount > 0 &&
            static_cast<std::size_t>(buffer->reportsCount) <= maximum_entries &&
            MmsServer_getReportBufferOverflowCount(private_server->mmsServer) > 0U;
    }

    if (rcb != nullptr) {
        ClientReportControlBlock_destroy(rcb);
    }
    if (client != nullptr) {
        IedConnection_close(client);
        IedConnection_destroy(client);
    }
    if (IedServer_isRunning(raw_server)) {
        IedServer_stop(raw_server);
    }
    IedServer_destroy(raw_server);
    return bounded;
}

bool uploaded_model_is_browsable(const char* path) {
    std::ifstream input(path, std::ios::binary);
    const std::string contents(
        (std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    uhf::iec61850::SclModelDefinition definition;
    std::string parse_error;
    if (!uhf::iec61850::parse_scl_model(contents, definition, parse_error)) {
        std::cerr << "parser error: " << parse_error << '\n';
        return false;
    }
    uhf::acquisition::SnapshotStore snapshots;
    uhf::iec61850::ServerOptions options;
    options.bind_address = "127.0.0.1";
    options.port = 15105U;
    options.ied_name = definition.ied_name;
    options.model_definition = definition;
    uhf::iec61850::Server server(snapshots, std::move(options));
    server.start();
    IedConnection connection = IedConnection_create();
    IedClientError error = IED_ERROR_OK;
    IedConnection_setConnectTimeout(connection, 1000U);
    IedConnection_connect(connection, &error, "127.0.0.1", 15105U);
    const std::string prefix = definition.ied_name + definition.logical_device;
    bool state_report_found = false;
    if (error == IED_ERROR_OK) {
        for (const ACSIClass report_class : {ACSI_CLASS_URCB, ACSI_CLASS_BRCB}) {
            IedClientError directory_error = IED_ERROR_OK;
            LinkedList reports = IedConnection_getLogicalNodeDirectory(
                connection,
                &directory_error,
                (prefix + "/LLN0").c_str(),
                report_class);
            if (directory_error == IED_ERROR_OK && reports != nullptr) {
                for (LinkedList item = reports; item != nullptr; item = LinkedList_getNext(item)) {
                    const char* name = static_cast<const char*>(LinkedList_getData(item));
                    if (name != nullptr && std::string(name) == "RPState") {
                        state_report_found = true;
                    }
                }
                LinkedList_destroy(reports);
            }
        }
    }
    const bool ok = error == IED_ERROR_OK && state_report_found;
    IedConnection_close(connection);
    IedConnection_destroy(connection);
    server.stop();
    return ok;
}

bool probe_running_model(const char* port_text, const char* prefix) {
    std::uint16_t port = 0U;
    const auto parsed = std::from_chars(
        port_text, port_text + std::char_traits<char>::length(port_text), port);
    if (parsed.ec != std::errc{} || parsed.ptr != port_text + std::char_traits<char>::length(port_text)) {
        return false;
    }
    IedConnection connection = IedConnection_create();
    IedClientError error = IED_ERROR_OK;
    IedConnection_setConnectTimeout(connection, 1000U);
    IedConnection_connect(connection, &error, "127.0.0.1", port);
    if (error != IED_ERROR_OK) {
        IedConnection_destroy(connection);
        return false;
    }
    MmsValue* value = IedConnection_readObject(
        connection, &error, (std::string(prefix) + "/SPDC1.PaDschAlm.stVal").c_str(), IEC61850_FC_ST);
    const bool point_ok = error == IED_ERROR_OK && value != nullptr;
    if (value != nullptr) {
        MmsValue_delete(value);
    }
    IedClientError directory_error = IED_ERROR_OK;
    LinkedList reports = IedConnection_getLogicalNodeDirectory(
        connection, &directory_error, (std::string(prefix) + "/LLN0").c_str(), ACSI_CLASS_BRCB);
    bool report_ok = false;
    if (directory_error == IED_ERROR_OK && reports != nullptr) {
        for (LinkedList item = reports; item != nullptr; item = LinkedList_getNext(item)) {
            const char* name = static_cast<const char*>(LinkedList_getData(item));
            if (name != nullptr && std::string(name) == "RPState") {
                report_ok = true;
            }
        }
        LinkedList_destroy(reports);
    }
    IedConnection_close(connection);
    IedConnection_destroy(connection);
    return point_ok && report_ok;
}

}  // namespace

int main(int argc, char* argv[]) {
    if (argc == 4 && std::string(argv[1]) == "--probe") {
        const bool ok = probe_running_model(argv[2], argv[3]);
        if (ok) {
            std::cout << "IEC 61850 running model probe: OK\n";
        }
        return ok ? 0 : 1;
    }
    if (argc == 2) {
        const bool ok = uploaded_model_is_browsable(argv[1]);
        if (ok) {
            std::cout << "IEC 61850 uploaded model server smoke: OK\n";
        }
        return ok ? 0 : 1;
    }
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
        std::array<IedConnection, 3> capacity_connections{};
        for (IedConnection& capacity_connection : capacity_connections) {
            capacity_connection = IedConnection_create();
            IedConnection_setConnectTimeout(capacity_connection, 1000U);
            IedClientError capacity_error = IED_ERROR_OK;
            IedConnection_connect(capacity_connection, &capacity_error, "127.0.0.1", 15102);
            ok = expect(capacity_error == IED_ERROR_OK, "MMS connection within cap succeeds") && ok;
        }

        IedConnection overflow_connection = IedConnection_create();
        IedConnection_setConnectTimeout(overflow_connection, 1000U);
        IedClientError overflow_error = IED_ERROR_OK;
        IedConnection_connect(overflow_connection, &overflow_error, "127.0.0.1", 15102);
        ok = expect(overflow_error != IED_ERROR_OK, "fifth MMS connection is rejected") && ok;
        IedConnection_close(overflow_connection);
        IedConnection_destroy(overflow_connection);
        const uhf::iec61850::RuntimeStats stats = server.stats();
        ok = expect(
            stats.active_connections == 4U,
            "active MMS connections reach the configured cap") && ok;
        ok = expect(
            stats.connection_rejections == 1U,
            "connection-limit rejection is counted") && ok;
        for (IedConnection capacity_connection : capacity_connections) {
            IedConnection_close(capacity_connection);
            IedConnection_destroy(capacity_connection);
        }
        ok = expect(
            server.stats().active_connections == 1U,
            "active MMS connections drop after clients close") && ok;

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

        const Quality standard_peak_quality = IedConnection_readQualityValue(
            connection,
            &error,
            "TESTIEDPDMON/SPDC1.UhfPaDsch.q",
            IEC61850_FC_MX);
        ok = expect(
            error == IED_ERROR_OK &&
                standard_peak_quality == static_cast<Quality>(QUALITY_VALIDITY_GOOD),
            "standard peak quality") && ok;
        Timestamp* standard_peak_time = IedConnection_readTimestampValue(
            connection,
            &error,
            "TESTIEDPDMON/SPDC1.UhfPaDsch.t",
            IEC61850_FC_MX,
            nullptr);
        ok = expect(
            error == IED_ERROR_OK && standard_peak_time != nullptr &&
                Timestamp_getTimeInMs(standard_peak_time) > 0U,
            "standard peak timestamp") && ok;
        if (standard_peak_time != nullptr) {
            Timestamp_destroy(standard_peak_time);
        }

        char data_set_element[] = "TESTIEDPDMON/GGIO1.AnIn1.mag.f";
        LinkedList data_set_elements = LinkedList_create();
        LinkedList_add(data_set_elements, data_set_element);
        error = IED_ERROR_OK;
        IedConnection_createDataSet(connection, &error, "@blocked", data_set_elements);
        ok = expect(error != IED_ERROR_OK, "dynamic data set service is disabled") && ok;
        LinkedList_destroyStatic(data_set_elements);

        error = IED_ERROR_OK;
        LinkedList file_directory = IedConnection_getFileDirectory(connection, &error, nullptr);
        ok = expect(error != IED_ERROR_OK && file_directory == nullptr, "file service is disabled") && ok;
        if (file_directory != nullptr) {
            LinkedList_destroyDeep(
                file_directory,
                reinterpret_cast<LinkedListValueDeleteFunction>(FileDirectoryEntry_destroy));
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
    ok = expect(
        server.update_endpoint("127.0.0.1", 15103U),
        "MMS endpoint reload") && ok;
    IedConnection reloaded_connection = IedConnection_create();
    IedConnection_setConnectTimeout(reloaded_connection, 1000U);
    error = IED_ERROR_OK;
    IedConnection_connect(reloaded_connection, &error, "127.0.0.1", 15103);
    ok = expect(error == IED_ERROR_OK, "MMS client connects after endpoint reload") && ok;
    IedConnection_close(reloaded_connection);
    IedConnection_destroy(reloaded_connection);
    server.stop();
    ok = expect(!server.running(), "MMS server stops") && ok;
    ok = expect(
        report_buffer_storm_is_bounded(),
        "report storm stays within 64 KiB and records overflow") && ok;
    if (ok) {
        std::cout << "IEC 61850 MMS server smoke: OK\n";
    }
    return ok ? 0 : 1;
}
