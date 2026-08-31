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
        "tls_enabled":false,
        "iec_ied_name":"UHFPD2",
        "storage_period_seconds":600,
        "storage_retention_days":2,
        "storage_min_free_bytes":268435456
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
        uhf::config::ConfigStore store(path);
        const uhf::config::Snapshot initial = store.snapshot();
        if (!expect(initial.version == 1U, "initial version") ||
            !expect(initial.values.acquisition_slave_id == 1U, "default slave ID") ||
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
        const uhf::config::UpdateResult updated = store.update(1U, valid_object());
        const uhf::config::Snapshot changed = store.snapshot();
        if (!expect(updated == uhf::config::UpdateResult::updated, "valid update") ||
            !expect(changed.version == 2U, "version increment") ||
            !expect(changed.values.rtu_unit_id == 3U, "updated RTU unit") ||
            !expect(changed.values.modbus_tcp_unit_id == 4U, "updated TCP unit") ||
            !expect(changed.values.tls_enabled == false, "updated TLS flag") ||
            !expect(std::filesystem::file_size(path) < 16U * 1024U, "updated file bound")) {
            return 1;
        }
        std::ifstream input(path);
        const std::string saved{
            std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
        if (!expect(saved.find("\"version\": 2") != std::string::npos, "saved version") ||
            !expect(saved.find("Smoke") == std::string::npos, "no test secret")) {
            return 1;
        }
        uhf::config::ConfigStore reopened(path);
        const uhf::config::Snapshot reopened_snapshot = reopened.snapshot();
        if (!expect(reopened_snapshot.version == 2U, "version survives restart") ||
            !expect(reopened_snapshot.values.rtu_unit_id == 3U, "values survive restart") ||
            !expect(reopened_snapshot.values.modbus_tcp_unit_id == 4U, "TCP unit survives restart")) {
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
        std::filesystem::remove_all(root, cleanup_error);
        std::cout << "config store smoke: OK\n";
        return 0;
    } catch (const std::exception& error) {
        std::filesystem::remove_all(root, cleanup_error);
        std::cerr << "config store smoke failed: " << error.what() << '\n';
        return 1;
    }
}
