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
        "storage_event_merge_seconds":60
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
        const uhf::config::UpdateResult updated = store.update(1U, valid_object());
        const uhf::config::Snapshot changed = store.snapshot();
        if (!expect(updated == uhf::config::UpdateResult::updated, "valid update") ||
            !expect(changed.version == 2U, "version increment") ||
            !expect(changed.values.rtu_unit_id == 3U, "updated RTU unit") ||
            !expect(changed.values.modbus_tcp_unit_id == 4U, "updated TCP unit") ||
            !expect(changed.values.tls_enabled, "TLS remains enabled") ||
            !expect(changed.values.iec_enabled == false, "updated IEC flag") ||
            !expect(changed.values.iec_port == 15102U, "updated IEC port") ||
            !expect(changed.values.overview_device == "2号主变", "overview device") ||
            !expect(changed.values.sntp_server == "time.example.com", "SNTP server") ||
            !expect(changed.values.storage_event_threshold_dbm == -45, "event threshold") ||
            !expect(changed.values.storage_event_rearm_dbm == -50, "event rearm") ||
            !expect(changed.values.storage_event_delta_db == 10U, "event delta") ||
            !expect(changed.values.storage_event_merge_seconds == 60U, "event merge window") ||
            !expect(std::filesystem::file_size(path) < 16U * 1024U, "updated file bound")) {
            return 1;
        }
        std::ifstream input(path);
        const std::string saved{
            std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
        if (!expect(saved.find("\"version\": 2") != std::string::npos, "saved version") ||
            !expect(saved.find("\"iec_port\": 15102") != std::string::npos, "saved IEC port") ||
            !expect(saved.find("\"phase_start_degree\": 45") != std::string::npos, "saved phase start") ||
            !expect(saved.find("Smoke") == std::string::npos, "no test secret")) {
            return 1;
        }
        uhf::config::ConfigStore reopened(path);
        const uhf::config::Snapshot reopened_snapshot = reopened.snapshot();
        if (!expect(reopened_snapshot.version == 2U, "version survives restart") ||
            !expect(reopened_snapshot.values.rtu_unit_id == 3U, "values survive restart") ||
            !expect(reopened_snapshot.values.modbus_tcp_unit_id == 4U, "TCP unit survives restart") ||
            !expect(reopened_snapshot.values.iec_port == 15102U, "IEC port survives restart") ||
            !expect(reopened_snapshot.values.overview_title == "现场局放监测", "overview survives restart")) {
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
