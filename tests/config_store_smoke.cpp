// SPDX-License-Identifier: GPL-3.0-only
#include "config/config_store.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <unistd.h>

namespace {

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "config store smoke failed: " << message << '\n';
        return false;
    }
    return true;
}

const char* valid_object() {
    return R"({
        "acquisition_device":"/dev/ttyS1",
        "acquisition_slave_id":2,
        "acquisition_period_ms":6000,
        "acquisition_response_timeout_ms":150,
        "acquisition_max_retries":2,
        "rtu_device":"/dev/ttyS4",
        "rtu_unit_id":3,
        "modbus_tcp_bind":"127.0.0.1",
        "modbus_tcp_unit_id":4,
        "modbus_tcp_port":15021,
        "web_port":8081,
        "tls_enabled":true,
        "iec_enabled":false,
        "iec_port":15102,
        "iec_ied_name":"UHFPD2",
        "ftp_enabled":true,
        "ftp_port":2121,
        "overview_title":"现场局放监测",
        "overview_device":"2号主变",
        "phase_start_degree":45,
        "time_sync_enabled":false,
        "sntp_server":"time.example.com",
        "storage_period_seconds":600,
        "storage_retention_days":2,
        "storage_min_free_bytes":268435456,
        "storage_event_threshold_dbm":-45,
        "storage_event_rearm_dbm":-50,
        "storage_event_delta_db":10,
        "storage_event_merge_seconds":60,
        "v3_alarm_thresholds":[100.5,null,null,9,null,null,null,null,null,19,null,null],
        "v3_current_encoding":"signed16",
        "v3_current_multiplier":0.25,
        "v3_current_offset":-1.5,
        "v3_temperature_multiplier":0.1,
        "v3_temperature_offset":0,
        "v3_current_serial":"9600/8E1",
        "v3_temperature_serial":"19200/8N2"
    })";
}

}  // namespace

int main() {
    const std::filesystem::path root = std::filesystem::temp_directory_path() /
        ("uhf-config-store-" + std::to_string(static_cast<long long>(::getpid())));
    std::error_code cleanup_error;
    std::filesystem::remove_all(root, cleanup_error);
    try {
        const std::filesystem::path path = root / "config.json";
        const std::filesystem::path defaults_path = root / "defaults.json";
        std::filesystem::create_directories(root);
        std::string defaults = valid_object();
        defaults.replace(defaults.find('{'), 1U, "{\"version\":1,");
        std::ofstream defaults_output(defaults_path);
        defaults_output << defaults;
        defaults_output.close();
        uhf::config::ConfigStore store(path, defaults_path);
        const uhf::config::Snapshot initial = store.snapshot();
        if (!expect(initial.version == 1U, "initial version") ||
            !expect(initial.values.acquisition_slave_id == 2U, "file default slave ID") ||
            !expect(initial.values.overview_title == "现场局放监测", "overview title") ||
            !expect(initial.values.phase_start_degree == 45U, "phase start") ||
            !expect(!initial.values.time_sync_enabled, "manual time mode") ||
            !expect(initial.values.v3_alarm_thresholds[0] == 100.5F,
                    "v3 PD threshold") ||
            !expect(!initial.values.v3_alarm_thresholds[1],
                    "v3 unconfigured threshold") ||
            !expect(initial.values.v3_current_encoding == "signed16",
                    "v3 current encoding") ||
            !expect(initial.values.v3_current_multiplier == 0.25F,
                    "v3 current multiplier") ||
            !expect(initial.values.v3_temperature_multiplier == 0.1F,
                    "v3 temperature multiplier") ||
            !expect(initial.values.v3_current_serial == "9600/8E1",
                    "v3 current serial profile") ||
            !expect(initial.values.v3_temperature_serial == "19200/8N2",
                    "v3 temperature serial profile") ||
            !expect(std::filesystem::is_regular_file(path), "initial file") ||
            !expect(std::filesystem::file_size(path) < 16U * 1024U, "bounded file")) {
            return 1;
        }
        const uhf::config::UpdateResult conflict = store.update(2U, valid_object());
        if (!expect(conflict == uhf::config::UpdateResult::conflict, "version conflict")) {
            return 1;
        }
        const uhf::config::UpdateResult invalid = store.update(1U, "{\"web_port\":80}");
        if (!expect(invalid == uhf::config::UpdateResult::invalid, "invalid value rejected")) {
            return 1;
        }
        const uhf::config::UpdateResult incomplete = store.update(
            1U,
            "{\"acquisition_device\":\"/dev/ttyS1\",\"acquisition_slave_id\":2,"
            "\"acquisition_period_ms\":6000,\"acquisition_response_timeout_ms\":150,"
            "\"acquisition_max_retries\":2,\"rtu_device\":\"/dev/ttyS4\","
            "\"rtu_unit_id\":3,\"modbus_tcp_bind\":\"127.0.0.1\","
            "\"modbus_tcp_unit_id\":4,\"modbus_tcp_port\":15021,\"web_port\":8081,"
            "\"tls_enabled\":true,\"iec_enabled\":false,\"iec_port\":15102,"
            "\"iec_ied_name\":\"UHFPD2\",\"storage_period_seconds\":600,"
            "\"storage_retention_days\":2}");
        if (!expect(incomplete == uhf::config::UpdateResult::invalid, "incomplete config rejected")) {
            return 1;
        }
        const std::string insecure_object = [] {
            std::string value = valid_object();
            const std::string enabled = "\"tls_enabled\":true";
            const std::size_t position = value.find(enabled);
            if (position != std::string::npos) {
                value.replace(position, enabled.size(), "\"tls_enabled\":false");
            }
            return value;
        }();
        const uhf::config::UpdateResult insecure = store.update(1U, insecure_object);
        if (!expect(insecure == uhf::config::UpdateResult::invalid, "HTTP-only config rejected")) {
            return 1;
        }
        const std::string conflicting_ports = [] {
            std::string value = valid_object();
            const std::string port = "\"modbus_tcp_port\":15021";
            const std::size_t position = value.find(port);
            if (position != std::string::npos) {
                value.replace(position, port.size(), "\"modbus_tcp_port\":8081");
            }
            return value;
        }();
    const uhf::config::UpdateResult conflict_ports =
            store.update(1U, conflicting_ports);
        if (!expect(
                conflict_ports == uhf::config::UpdateResult::invalid,
                "conflicting service ports accepted")) {
            return 1;
        }
        const std::string invalid_event_window = [] {
            std::string value = valid_object();
            const std::string rearm = "\"storage_event_rearm_dbm\":-50";
            const std::size_t position = value.find(rearm);
            if (position != std::string::npos) {
                value.replace(
                    position, rearm.size(), "\"storage_event_rearm_dbm\":-45");
            }
            return value;
        }();
        const uhf::config::UpdateResult invalid_event =
            store.update(1U, invalid_event_window);
        if (!expect(
                invalid_event == uhf::config::UpdateResult::invalid,
                "event rearm threshold must be below trigger threshold")) {
            return 1;
        }
        const std::string conflicting_ftp_port = [] {
            std::string value = valid_object();
            const std::string port = "\"ftp_port\":2121";
            const std::size_t position = value.find(port);
            if (position != std::string::npos) {
                value.replace(position, port.size(), "\"ftp_port\":8081");
            }
            return value;
        }();
        if (!expect(
                store.update(1U, conflicting_ftp_port) == uhf::config::UpdateResult::invalid,
                "conflicting FTP port accepted")) {
            return 1;
        }
        const std::string invalid_alarm_count = [] {
            std::string value = valid_object();
            const std::string thresholds =
                "\"v3_alarm_thresholds\":[100.5,null,null,9,null,null,null,null,null,19,null,null]";
            const std::size_t position = value.find(thresholds);
            if (position != std::string::npos) {
                value.replace(
                    position, thresholds.size(),
                    "\"v3_alarm_thresholds\":[100.5,null]");
            }
            return value;
        }();
        if (!expect(
                store.update(1U, invalid_alarm_count) == uhf::config::UpdateResult::invalid,
                "short v3 alarm array accepted")) {
            return 1;
        }
        const std::string partial_conversion = [] {
            std::string value = valid_object();
            const std::string offset = "\"v3_current_offset\":-1.5,";
            const std::size_t position = value.find(offset);
            if (position != std::string::npos) {
                value.erase(position, offset.size());
            }
            return value;
        }();
        if (!expect(
                store.update(1U, partial_conversion) == uhf::config::UpdateResult::invalid,
                "partial v3 current conversion accepted")) {
            return 1;
        }
        const std::string negative_multiplier = [] {
            std::string value = valid_object();
            const std::string multiplier = "\"v3_temperature_multiplier\":0.1";
            const std::size_t position = value.find(multiplier);
            if (position != std::string::npos) {
                value.replace(
                    position, multiplier.size(),
                    "\"v3_temperature_multiplier\":-0.1");
            }
            return value;
        }();
        if (!expect(
                store.update(1U, negative_multiplier) ==
                    uhf::config::UpdateResult::invalid,
                "negative v3 conversion multiplier accepted")) {
            return 1;
        }
        const std::string unsupported_serial = [] {
            std::string value = valid_object();
            const std::string profile = "\"v3_current_serial\":\"9600/8E1\"";
            const std::size_t position = value.find(profile);
            if (position != std::string::npos) {
                value.replace(
                    position, profile.size(),
                    "\"v3_current_serial\":\"12000/8N1\"");
            }
            return value;
        }();
        if (!expect(
                store.update(1U, unsupported_serial) ==
                    uhf::config::UpdateResult::invalid,
                "unsupported v3 serial profile accepted")) {
            return 1;
        }
        const std::filesystem::path blocked_temporary = path.string() + ".tmp";
        std::filesystem::create_directory(blocked_temporary);
        const uhf::config::UpdateResult storage_failure = store.update(1U, valid_object());
        if (!expect(storage_failure == uhf::config::UpdateResult::storage_error,
                    "atomic config write failure was not surfaced") ||
            !expect(store.snapshot().version == 1U,
                    "failed config write changed the active version") ||
            !expect(store.snapshot().values.acquisition_slave_id == 2U,
                    "failed config write changed active values")) {
            return 1;
        }
        std::filesystem::remove(blocked_temporary, cleanup_error);
        const uhf::config::UpdateResult updated = store.update(1U, valid_object());
        const uhf::config::Snapshot changed = store.snapshot();
        if (!expect(updated == uhf::config::UpdateResult::updated, "valid update") ||
            !expect(changed.version == 2U, "version increment") ||
            !expect(changed.values.rtu_unit_id == 3U, "updated RTU unit") ||
            !expect(changed.values.modbus_tcp_unit_id == 4U, "updated TCP unit") ||
            !expect(changed.values.tls_enabled, "TLS remains enabled") ||
            !expect(changed.values.iec_enabled == false, "updated IEC flag") ||
            !expect(changed.values.iec_port == 15102U, "updated IEC port") ||
            !expect(changed.values.ftp_enabled, "updated FTP flag") ||
            !expect(changed.values.ftp_port == 2121U, "updated FTP port") ||
            !expect(changed.values.overview_device == "2号主变", "overview device") ||
            !expect(changed.values.sntp_server == "time.example.com", "SNTP server") ||
            !expect(changed.values.storage_event_threshold_dbm == -45, "event threshold") ||
            !expect(changed.values.storage_event_rearm_dbm == -50, "event rearm") ||
            !expect(changed.values.storage_event_delta_db == 10U, "event delta") ||
            !expect(changed.values.storage_event_merge_seconds == 60U, "event merge window") ||
            !expect(changed.values.v3_alarm_thresholds[3] == 9.0F,
                    "current alarm threshold") ||
            !expect(changed.values.v3_alarm_thresholds[9] == 19.0F,
                    "temperature alarm threshold") ||
            !expect(changed.values.v3_current_offset == -1.5F,
                    "current conversion offset") ||
            !expect(changed.values.v3_temperature_offset == 0.0F,
                    "temperature conversion offset") ||
            !expect(changed.values.v3_current_serial == "9600/8E1",
                    "current serial profile") ||
            !expect(std::filesystem::file_size(path) < 16U * 1024U, "updated file bound")) {
            return 1;
        }
        std::ifstream input(path);
        const std::string saved{
            std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
        if (!expect(saved.find("\"version\": 2") != std::string::npos, "saved version") ||
            !expect(saved.find("\"iec_port\": 15102") != std::string::npos, "saved IEC port") ||
            !expect(saved.find("\"phase_start_degree\": 45") != std::string::npos, "saved phase start") ||
            !expect(saved.find("\"v3_alarm_thresholds\": [100.5,null,null,9") !=
                    std::string::npos, "saved v3 alarm thresholds") ||
            !expect(saved.find("\"v3_current_encoding\": \"signed16\"") !=
                    std::string::npos, "saved v3 current encoding") ||
            !expect(saved.find("\"v3_temperature_multiplier\": 0.1") !=
                    std::string::npos, "saved v3 temperature multiplier") ||
            !expect(saved.find("\"v3_temperature_serial\": \"19200/8N2\"") !=
                    std::string::npos, "saved v3 temperature serial profile") ||
            !expect(saved.find("Smoke") == std::string::npos, "no test secret")) {
            return 1;
        }
        if (!expect(!store.set_iec_ied_name("bad-name"), "invalid direct IED name") ||
            !expect(store.snapshot().version == 2U, "invalid IED name changed version") ||
            !expect(store.set_iec_ied_name("RENAMED1"), "direct IED name update") ||
            !expect(store.snapshot().version == 3U, "IED name version increment") ||
            !expect(store.snapshot().values.iec_ied_name == "RENAMED1", "updated IED name")) {
            return 1;
        }
        uhf::config::ConfigStore reopened(path);
        const uhf::config::Snapshot reopened_snapshot = reopened.snapshot();
        if (!expect(reopened_snapshot.version == 3U, "version survives restart") ||
            !expect(reopened_snapshot.values.rtu_unit_id == 3U, "values survive restart") ||
            !expect(reopened_snapshot.values.modbus_tcp_unit_id == 4U, "TCP unit survives restart") ||
            !expect(reopened_snapshot.values.iec_port == 15102U, "IEC port survives restart") ||
            !expect(reopened_snapshot.values.iec_ied_name == "RENAMED1", "IED name survives restart") ||
            !expect(reopened_snapshot.values.overview_title == "现场局放监测", "overview survives restart") ||
            !expect(reopened_snapshot.values.v3_alarm_thresholds[0] == 100.5F,
                    "v3 alarm threshold survives restart") ||
            !expect(!reopened_snapshot.values.v3_alarm_thresholds[2],
                    "v3 disabled alarm survives restart") ||
            !expect(reopened_snapshot.values.v3_current_encoding == "signed16",
                    "v3 encoding survives restart") ||
            !expect(reopened_snapshot.values.v3_current_multiplier == 0.25F,
                    "v3 multiplier survives restart") ||
            !expect(reopened_snapshot.values.v3_current_serial == "9600/8E1",
                    "v3 serial profile survives restart")) {
            return 1;
        }

        std::ofstream corrupt(path, std::ios::trunc);
        corrupt << "{\"web_port\":80}";
        corrupt.close();
        bool rejected = false;
        try {
            uhf::config::ConfigStore broken(path);
        } catch (const std::exception&) {
            rejected = true;
        }
        if (!expect(rejected, "corrupt config rejected at startup")) {
            return 1;
        }
        const std::filesystem::path bad_defaults_path = root / "bad-defaults.json";
        std::ofstream bad_defaults(bad_defaults_path);
        bad_defaults << "{\"web_port\":80}";
        bad_defaults.close();
        bool bad_defaults_rejected = false;
        try {
            uhf::config::ConfigStore invalid_defaults(root / "fresh.json", bad_defaults_path);
        } catch (const std::exception&) {
            bad_defaults_rejected = true;
        }
        if (!expect(bad_defaults_rejected, "invalid defaults rejected at startup")) {
            return 1;
        }
        const std::filesystem::path empty_defaults_path = root / "empty-defaults.json";
        std::ofstream empty_defaults(empty_defaults_path);
        empty_defaults.close();
        bool empty_defaults_rejected = false;
        try {
            uhf::config::ConfigStore empty_defaults_store(
                root / "empty.json", empty_defaults_path);
        } catch (const std::exception&) {
            empty_defaults_rejected = true;
        }
        if (!expect(empty_defaults_rejected, "empty defaults rejected at startup")) {
            return 1;
        }
        std::filesystem::remove_all(root, cleanup_error);
        std::cout << "config store smoke: OK\n";
        return 0;
    } catch (const std::exception& error) {
        std::filesystem::remove_all(root, cleanup_error);
        std::cerr << "config store smoke failed: " << error.what() << '\n';
        return 1;
    }
}
