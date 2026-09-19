// SPDX-License-Identifier: GPL-3.0-only
#include "acquisition/acquisition.hpp"
#include "domain/snapshot.hpp"
#include "iec61850_client.h"
#include "iec61850/scl_model.hpp"
#include "iec61850/server.hpp"
#include "linked_list.h"
#include "v3/acquisition.hpp"
#include "v3/protocol.hpp"

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
    std::atomic<std::uint32_t> data_set_size{0U};
    std::atomic<bool> first_measurement_changed{false};
    std::atomic<float> first_measurement_value{0.0F};
};

struct StateReportState {
    std::atomic<unsigned int> count{0U};
    std::atomic<std::uint32_t> data_set_size{0U};
    std::atomic<bool> current_communication_alarm_changed{false};
    std::atomic<bool> current_communication_alarm{false};
};

void report_callback(void* parameter, ClientReport report) {
    auto* state = static_cast<ReportState*>(parameter);
    MmsValue* values = ClientReport_getDataSetValues(report);
    if (values != nullptr) {
        const std::uint32_t size = MmsValue_getArraySize(values);
        state->data_set_size.store(size);
        constexpr int kFirstV3MeasurementIndex = 3;
        if (size > static_cast<std::uint32_t>(kFirstV3MeasurementIndex) &&
            ClientReport_getReasonForInclusion(report, kFirstV3MeasurementIndex) ==
                IEC61850_REASON_DATA_CHANGE) {
            MmsValue* value = MmsValue_getElement(values, kFirstV3MeasurementIndex);
            if (value != nullptr && MmsValue_getType(value) == MMS_FLOAT) {
                state->first_measurement_value.store(MmsValue_toFloat(value));
                state->first_measurement_changed.store(true);
            }
        }
        state->count.fetch_add(1U);
    }
}

void state_report_callback(void* parameter, ClientReport report) {
    auto* state = static_cast<StateReportState*>(parameter);
    MmsValue* values = ClientReport_getDataSetValues(report);
    if (values != nullptr) {
        const std::uint32_t size = MmsValue_getArraySize(values);
        state->data_set_size.store(size);
        constexpr int kCurrentCommunicationAlarmIndex = 1;
        if (size > static_cast<std::uint32_t>(kCurrentCommunicationAlarmIndex) &&
            ClientReport_getReasonForInclusion(report, kCurrentCommunicationAlarmIndex) ==
                IEC61850_REASON_DATA_CHANGE) {
            MmsValue* value = MmsValue_getElement(values, kCurrentCommunicationAlarmIndex);
            if (value != nullptr && MmsValue_getType(value) == MMS_BOOLEAN) {
                state->current_communication_alarm.store(MmsValue_getBoolean(value));
                state->current_communication_alarm_changed.store(true);
            }
        }
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

bool probe_v3_running_model(
    const char* port_text, const char* prefix, const char* expected_text) {
    std::uint16_t port = 0U;
    const auto parsed_port = std::from_chars(
        port_text, port_text + std::char_traits<char>::length(port_text), port);
    if (parsed_port.ec != std::errc{} ||
        parsed_port.ptr != port_text + std::char_traits<char>::length(port_text)) {
        return false;
    }
    std::size_t parsed_characters = 0U;
    float expected = 0.0F;
    try {
        expected = std::stof(expected_text, &parsed_characters);
    } catch (...) {
        return false;
    }
    if (parsed_characters != std::char_traits<char>::length(expected_text) ||
        !std::isfinite(expected)) {
        return false;
    }

    IedConnection connection = IedConnection_create();
    if (connection == nullptr) {
        return false;
    }
    IedClientError error = IED_ERROR_OK;
    IedConnection_setConnectTimeout(connection, 2000U);
    IedConnection_connect(connection, &error, "127.0.0.1", port);
    bool ok = expect(error == IED_ERROR_OK, "connect to running v3 gateway MMS server");

    ClientDataSet data_set = nullptr;
    if (ok) {
        data_set = IedConnection_readDataSetValues(
            connection,
            &error,
            (std::string(prefix) + "/LLN0.DSV3Measurements").c_str(),
            nullptr);
        ok = expect(
            error == IED_ERROR_OK && data_set != nullptr &&
                ClientDataSet_getDataSetSize(data_set) == 41,
            "running v3 gateway exposes the 41-point measurement data set") && ok;
    }
    if (data_set != nullptr) {
        ClientDataSet_destroy(data_set);
    }

    const std::string rcb_reference =
        std::string(prefix) + "/LLN0.RP.RPV3Measurements";
    ClientReportControlBlock rcb = nullptr;
    ReportState report_state;
    if (ok) {
        rcb = IedConnection_getRCBValues(
            connection, &error, rcb_reference.c_str(), nullptr);
        ok = expect(
            error == IED_ERROR_OK && rcb != nullptr &&
                !ClientReportControlBlock_isBuffered(rcb),
            "running v3 gateway exposes the measurement URCB") && ok;
    }
    if (ok) {
        IedConnection_installReportHandler(
            connection,
            rcb_reference.c_str(),
            ClientReportControlBlock_getRptId(rcb),
            report_callback,
            &report_state);
        ClientReportControlBlock_setTrgOps(rcb, TRG_OPT_DATA_CHANGED);
        ClientReportControlBlock_setRptEna(rcb, true);
        IedConnection_setRCBValues(
            connection,
            &error,
            rcb,
            RCB_ELEMENT_RPT_ENA | RCB_ELEMENT_TRG_OPS,
            true);
        ok = expect(error == IED_ERROR_OK, "enable running v3 gateway measurement URCB") && ok;
    }

    if (ok) {
        std::cout << "IEC_V3_PROBE_READY\n" << std::flush;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
        while ((!report_state.first_measurement_changed.load() ||
                std::fabs(static_cast<double>(
                    report_state.first_measurement_value.load()) - expected) >= 0.01) &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        ok = expect(
            report_state.count.load() > 0U && report_state.data_set_size.load() == 41 &&
                report_state.first_measurement_changed.load() &&
                std::fabs(static_cast<double>(
                    report_state.first_measurement_value.load()) - expected) < 0.01,
            "running v3 gateway emits the expected Ia data-change report") && ok;
    }

    if (ok) {
        const std::string value_reference = std::string(prefix) + "/MMXU1.AnIn1.mag.f";
        MmsValue* value = IedConnection_readObject(
            connection, &error, value_reference.c_str(), IEC61850_FC_MX);
        const bool value_read = error == IED_ERROR_OK;
        const Quality quality = IedConnection_readQualityValue(
            connection,
            &error,
            (std::string(prefix) + "/MMXU1.AnIn1.q").c_str(),
            IEC61850_FC_MX);
        ok = expect(
            value_read && error == IED_ERROR_OK && value != nullptr &&
                std::fabs(static_cast<double>(MmsValue_toFloat(value)) - expected) < 0.01 &&
                quality == static_cast<Quality>(QUALITY_VALIDITY_GOOD),
            "running v3 gateway serves the reported Ia value with good quality") && ok;
        if (value != nullptr) {
            MmsValue_delete(value);
        }
    }

    if (rcb != nullptr) {
        ClientReportControlBlock_setRptEna(rcb, false);
        IedConnection_setRCBValues(
            connection, &error, rcb, RCB_ELEMENT_RPT_ENA, true);
        ok = expect(error == IED_ERROR_OK, "disable running v3 gateway measurement URCB") && ok;
        ClientReportControlBlock_destroy(rcb);
    }
    IedConnection_close(connection);
    IedConnection_destroy(connection);
    return ok;
}

bool v3_model_is_readable() {
    uhf::v3::FreshnessLimits freshness;
    freshness.current = std::chrono::milliseconds(250);
    uhf::v3::SnapshotStore v3_snapshots(3U, {}, freshness);
    const auto now = std::chrono::steady_clock::now();
    const auto current_utc = std::chrono::system_clock::time_point(
        std::chrono::milliseconds(1700000001000LL));
    const auto temperature_utc = std::chrono::system_clock::time_point(
        std::chrono::milliseconds(1700000002000LL));
    const auto pd_utc = std::chrono::system_clock::time_point(
        std::chrono::milliseconds(1700000003000LL));
    uhf::v3::CurrentValues current{};
    for (std::size_t index = 0U; index < current.size(); ++index) {
        current[index] = uhf::v3::valid_value(static_cast<float>(index + 1U));
    }
    v3_snapshots.publish_current(current, now, current_utc);
    v3_snapshots.publish_temperature(
        {uhf::v3::valid_value(20.0F), uhf::v3::valid_value(21.0F),
         uhf::v3::valid_value(22.0F)}, now, temperature_utc);
    uhf::v3::ChannelRegisters words{};
    words[2] = 123U;
    uhf::v3::RegisterValidity received;
    received.set();
    const uhf::v3::PdChannel channel = uhf::v3::decode_pd_channel(words, received);
    for (std::size_t index = 0U; index < uhf::v3::kChannelCount; ++index) {
        v3_snapshots.publish_pd_channel(index, channel, now, pd_utc);
    }

    uhf::acquisition::SnapshotStore legacy_snapshots;
    uhf::iec61850::ServerOptions options;
    options.bind_address = "127.0.0.1";
    options.port = 15106U;
    options.ied_name = "TESTV3";
    options.model_definition = uhf::iec61850::default_v3_model_definition("TESTV3");
    options.v3_snapshot_store = &v3_snapshots;
    uhf::iec61850::Server server(legacy_snapshots, std::move(options));
    server.start();
    IedConnection connection = IedConnection_create();
    IedClientError error = IED_ERROR_OK;
    IedConnection_setConnectTimeout(connection, 1000U);
    IedConnection_connect(connection, &error, "127.0.0.1", 15106U);
    bool ok = error == IED_ERROR_OK;
    if (ok) {
        v3_snapshots.publish_current(
            current, std::chrono::steady_clock::now(), current_utc);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        MmsValue* peak = IedConnection_readObject(
            connection, &error, "TESTV3PDMON/SPDC2.UhfPaDsch.mag.f", IEC61850_FC_MX);
        ok = error == IED_ERROR_OK && peak != nullptr &&
            std::fabs(static_cast<double>(MmsValue_toFloat(peak)) - 123.0) < 0.01;
        if (peak != nullptr) {
            MmsValue_delete(peak);
        }
        MmsValue* calculated = IedConnection_readObject(
            connection, &error, "TESTV3PDMON/MMXU1.AnIn1.mag.f", IEC61850_FC_MX);
        ok = ok && error == IED_ERROR_OK && calculated != nullptr;
        if (calculated != nullptr) {
            MmsValue_delete(calculated);
        }
        Timestamp* peak_time = IedConnection_readTimestampValue(
            connection,
            &error,
            "TESTV3PDMON/SPDC2.UhfPaDsch.t",
            IEC61850_FC_MX,
            nullptr);
        ok = ok && error == IED_ERROR_OK && peak_time != nullptr &&
            Timestamp_getTimeInMs(peak_time) == 1700000003000ULL;
        if (peak_time != nullptr) Timestamp_destroy(peak_time);
        Timestamp* current_time = IedConnection_readTimestampValue(
            connection,
            &error,
            "TESTV3PDMON/MMXU1.AnIn1.t",
            IEC61850_FC_MX,
            nullptr);
        ok = ok && error == IED_ERROR_OK && current_time != nullptr &&
            Timestamp_getTimeInMs(current_time) == 1700000001000ULL;
        if (current_time != nullptr) Timestamp_destroy(current_time);
        const Quality fresh_quality = IedConnection_readQualityValue(
            connection, &error, "TESTV3PDMON/MMXU1.AnIn1.q", IEC61850_FC_MX);
        ok = ok && error == IED_ERROR_OK &&
            fresh_quality == static_cast<Quality>(QUALITY_VALIDITY_GOOD);

        ClientDataSet data_set = IedConnection_readDataSetValues(
            connection, &error, "TESTV3PDMON/LLN0.DSV3Measurements", nullptr);
        ok = expect(
            error == IED_ERROR_OK && data_set != nullptr,
            "read v3 static measurement data set") && ok;
        if (data_set != nullptr) {
            MmsValue* values = ClientDataSet_getValues(data_set);
            MmsValue* first_measurement = values == nullptr
                ? nullptr : MmsValue_getElement(values, 3);
            ok = expect(
                ClientDataSet_getDataSetSize(data_set) == 41 && values != nullptr &&
                    MmsValue_getArraySize(values) == 41 && first_measurement != nullptr &&
                    MmsValue_getType(first_measurement) == MMS_FLOAT &&
                    std::fabs(static_cast<double>(MmsValue_toFloat(first_measurement)) - 1.0) <
                        0.01,
                "v3 static data set contains all 41 mapped measurements") && ok;
            ClientDataSet_destroy(data_set);
        }

        ClientReportControlBlock rcb = IedConnection_getRCBValues(
            connection,
            &error,
            "TESTV3PDMON/LLN0.RP.RPV3Measurements",
            nullptr);
        ok = expect(error == IED_ERROR_OK && rcb != nullptr, "read v3 measurement URCB") && ok;
        if (rcb != nullptr) {
            ok = expect(!ClientReportControlBlock_isBuffered(rcb),
                        "v3 measurement report is unbuffered") && ok;
            ReportState report_state;
            IedConnection_installReportHandler(
                connection,
                "TESTV3PDMON/LLN0.RP.RPV3Measurements",
                ClientReportControlBlock_getRptId(rcb),
                report_callback,
                &report_state);
            ClientReportControlBlock_setTrgOps(rcb, TRG_OPT_DATA_CHANGED);
            ClientReportControlBlock_setRptEna(rcb, true);
            IedConnection_setRCBValues(
                connection,
                &error,
                rcb,
                RCB_ELEMENT_RPT_ENA | RCB_ELEMENT_TRG_OPS,
                true);
            ok = expect(error == IED_ERROR_OK, "enable v3 measurement URCB") && ok;

            current[0] = uhf::v3::valid_value(9.0F);
            v3_snapshots.publish_current(
                current, std::chrono::steady_clock::now(), current_utc);
            const auto report_deadline = std::chrono::steady_clock::now() +
                std::chrono::seconds(2);
            while ((!report_state.first_measurement_changed.load() ||
                    std::fabs(static_cast<double>(
                        report_state.first_measurement_value.load()) - 9.0) >= 0.01) &&
                   std::chrono::steady_clock::now() < report_deadline) {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
            ok = expect(
                report_state.count.load() > 0U &&
                    report_state.data_set_size.load() == 41 &&
                    report_state.first_measurement_changed.load() &&
                    std::fabs(static_cast<double>(
                        report_state.first_measurement_value.load()) - 9.0) < 0.01,
                "receive v3 Ia data-change report from the unified snapshot") && ok;

            ClientReportControlBlock_setRptEna(rcb, false);
            IedConnection_setRCBValues(
                connection, &error, rcb, RCB_ELEMENT_RPT_ENA, true);
            ok = expect(error == IED_ERROR_OK, "disable v3 measurement URCB") && ok;
            ClientReportControlBlock_destroy(rcb);
        }

        ClientDataSet state_data_set = IedConnection_readDataSetValues(
            connection, &error, "TESTV3PDMON/LLN0.DSV3State", nullptr);
        ok = expect(
            error == IED_ERROR_OK && state_data_set != nullptr,
            "read v3 static state data set") && ok;
        if (state_data_set != nullptr) {
            MmsValue* values = ClientDataSet_getValues(state_data_set);
            MmsValue* current_communication_alarm = values == nullptr
                ? nullptr : MmsValue_getElement(values, 1);
            ok = expect(
                ClientDataSet_getDataSetSize(state_data_set) == 15 && values != nullptr &&
                    MmsValue_getArraySize(values) == 15 &&
                    current_communication_alarm != nullptr &&
                    MmsValue_getType(current_communication_alarm) == MMS_BOOLEAN &&
                    !MmsValue_getBoolean(current_communication_alarm),
                "v3 state data set contains 15 points and no current communication alarm") && ok;
            ClientDataSet_destroy(state_data_set);
        }

        ClientReportControlBlock state_rcb = IedConnection_getRCBValues(
            connection,
            &error,
            "TESTV3PDMON/LLN0.BR.RPV3State",
            nullptr);
        ok = expect(
            error == IED_ERROR_OK && state_rcb != nullptr,
            "read v3 state BRCB") && ok;
        if (state_rcb != nullptr) {
            ok = expect(ClientReportControlBlock_isBuffered(state_rcb),
                        "v3 state report is buffered") && ok;
            StateReportState report_state;
            IedConnection_installReportHandler(
                connection,
                "TESTV3PDMON/LLN0.BR.RPV3State",
                ClientReportControlBlock_getRptId(state_rcb),
                state_report_callback,
                &report_state);
            ClientReportControlBlock_setTrgOps(state_rcb, TRG_OPT_DATA_CHANGED);
            ClientReportControlBlock_setRptEna(state_rcb, true);
            IedConnection_setRCBValues(
                connection,
                &error,
                state_rcb,
                RCB_ELEMENT_RPT_ENA | RCB_ELEMENT_TRG_OPS,
                true);
            ok = expect(error == IED_ERROR_OK, "enable v3 state BRCB") && ok;

            for (std::size_t failure = 0U; failure < 3U; ++failure) {
                v3_snapshots.record_failure(
                    uhf::v3::Source::current,
                    std::chrono::steady_clock::now(),
                    "simulated timeout",
                    current_utc + std::chrono::milliseconds(
                        static_cast<std::int64_t>(failure + 1U)));
            }
            const auto report_deadline = std::chrono::steady_clock::now() +
                std::chrono::seconds(6);
            while ((!report_state.current_communication_alarm_changed.load() ||
                    !report_state.current_communication_alarm.load()) &&
                   std::chrono::steady_clock::now() < report_deadline) {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
            ok = expect(
                report_state.count.load() > 0U &&
                    report_state.data_set_size.load() == 15 &&
                    report_state.current_communication_alarm_changed.load() &&
                    report_state.current_communication_alarm.load(),
                "receive v3 current communication-alarm report after three failures") && ok;

            ClientReportControlBlock_setRptEna(state_rcb, false);
            IedConnection_setRCBValues(
                connection, &error, state_rcb, RCB_ELEMENT_RPT_ENA, true);
            ok = expect(error == IED_ERROR_OK, "disable v3 state BRCB") && ok;
            ClientReportControlBlock_destroy(state_rcb);
        }

        v3_snapshots.publish_current(
            current, std::chrono::steady_clock::now(), current_utc);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        MmsValue* recovered_communication_alarm = IedConnection_readObject(
            connection,
            &error,
            "TESTV3PDMON/GGIO1.Ind2.stVal",
            IEC61850_FC_ST);
        const bool recovered_alarm_read = error == IED_ERROR_OK;
        const Quality recovered_alarm_quality = IedConnection_readQualityValue(
            connection,
            &error,
            "TESTV3PDMON/GGIO1.Ind2.q",
            IEC61850_FC_ST);
        ok = expect(
            recovered_alarm_read && error == IED_ERROR_OK &&
                recovered_communication_alarm != nullptr &&
                !MmsValue_getBoolean(recovered_communication_alarm) &&
                recovered_alarm_quality == static_cast<Quality>(QUALITY_VALIDITY_GOOD),
            "successful current sample clears the served communication alarm") && ok;
        if (recovered_communication_alarm != nullptr) {
            MmsValue_delete(recovered_communication_alarm);
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(350));
        const Quality stale_quality = IedConnection_readQualityValue(
            connection, &error, "TESTV3PDMON/MMXU1.AnIn1.q", IEC61850_FC_MX);
        const bool stale_quality_read = error == IED_ERROR_OK;
        MmsValue* stale_value = IedConnection_readObject(
            connection, &error, "TESTV3PDMON/MMXU1.AnIn1.mag.f", IEC61850_FC_MX);
        ok = ok && stale_quality_read && error == IED_ERROR_OK && stale_value != nullptr &&
            stale_quality == static_cast<Quality>(
                QUALITY_VALIDITY_QUESTIONABLE | QUALITY_DETAIL_OLD_DATA) &&
            std::fabs(static_cast<double>(MmsValue_toFloat(stale_value)) - 9.0) < 0.01;
        if (stale_value != nullptr) MmsValue_delete(stale_value);
    }
    IedConnection_close(connection);
    IedConnection_destroy(connection);
    server.stop();
    return ok;
}

}  // namespace

int main(int argc, char* argv[]) {
    if (argc == 2 && std::string(argv[1]) == "--v3") {
        const bool ok = v3_model_is_readable();
        if (ok) {
            std::cout << "IEC 61850 v3 model smoke: OK\n";
        }
        return ok ? 0 : 1;
    }
    if (argc == 4 && std::string(argv[1]) == "--probe") {
        const bool ok = probe_running_model(argv[2], argv[3]);
        if (ok) {
            std::cout << "IEC 61850 running model probe: OK\n";
        }
        return ok ? 0 : 1;
    }
    if (argc == 5 && std::string(argv[1]) == "--v3-probe") {
        const bool ok = probe_v3_running_model(argv[2], argv[3], argv[4]);
        if (ok) {
            std::cout << "IEC 61850 running v3 gateway probe: OK\n";
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
        const auto close_deadline = std::chrono::steady_clock::now() +
            std::chrono::milliseconds(500);
        while (server.stats().active_connections != 1U &&
               std::chrono::steady_clock::now() < close_deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
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

        const auto communication_failure_at = std::chrono::steady_clock::now();
        snapshots.record_failure(
            communication_failure_at,
            "no response 1",
            uhf::acquisition::FailureReason::no_response);
        snapshots.record_failure(
            communication_failure_at,
            "no response 2",
            uhf::acquisition::FailureReason::no_response);
        snapshots.record_failure(
            communication_failure_at,
            "no response 3",
            uhf::acquisition::FailureReason::no_response);
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        MmsValue* communication_alarm = IedConnection_readObject(
            connection,
            &error,
            "TESTIEDPDMON/GGIO1.Ind1.stVal",
            IEC61850_FC_ST);
        ok = expect(
            error == IED_ERROR_OK && communication_alarm != nullptr,
            "read communication alarm") && ok;
        if (communication_alarm != nullptr) {
            ok = expect(
                MmsValue_getBoolean(communication_alarm),
                "third no-response result raises IEC communication alarm") && ok;
            MmsValue_delete(communication_alarm);
        }

        ClientDataSet state_data_set = IedConnection_readDataSetValues(
            connection, &error, "TESTIEDPDMON/LLN0.DSState", nullptr);
        ok = expect(
            error == IED_ERROR_OK && state_data_set != nullptr,
            "read IEC state data set") && ok;
        if (state_data_set != nullptr) {
            ok = expect(
                ClientDataSet_getDataSetSize(state_data_set) == 2,
                "state data set carries event and communication alarms") && ok;
            ClientDataSet_destroy(state_data_set);
        }

        const auto communication_recovered_at = std::chrono::steady_clock::now();
        snapshots.publish(payload, communication_recovered_at, communication_recovered_at);
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        communication_alarm = IedConnection_readObject(
            connection,
            &error,
            "TESTIEDPDMON/GGIO1.Ind1.stVal",
            IEC61850_FC_ST);
        ok = expect(
            error == IED_ERROR_OK && communication_alarm != nullptr &&
                !MmsValue_getBoolean(communication_alarm),
            "successful snapshot clears IEC communication alarm") && ok;
        if (communication_alarm != nullptr) {
            MmsValue_delete(communication_alarm);
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
