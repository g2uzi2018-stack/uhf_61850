// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

namespace uhf::config {

struct Values {
    std::string acquisition_device{"/dev/ttyS1"};
    std::uint8_t acquisition_slave_id{1U};
    std::uint32_t acquisition_period_ms{6000U};
    std::uint32_t acquisition_response_timeout_ms{150U};
    std::uint8_t acquisition_max_retries{3U};
    std::string rtu_device{"/dev/ttyS4"};
    std::uint8_t rtu_unit_id{1U};
    std::string modbus_tcp_bind{"127.0.0.1"};
    std::uint8_t modbus_tcp_unit_id{1U};
    std::uint16_t modbus_tcp_port{502U};
    std::uint16_t web_port{8080U};
    bool tls_enabled{true};
    std::string iec_ied_name{"UHFPD1"};
    std::uint32_t storage_period_seconds{300U};
    std::uint32_t storage_retention_days{1U};
    std::uint64_t storage_min_free_bytes{536870912U};
};

struct Snapshot {
    std::uint64_t version{1U};
    Values values{};
};

enum class UpdateResult {
    updated,
    invalid,
    conflict,
    storage_error,
};

class ConfigStore {
public:
    explicit ConfigStore(std::filesystem::path file);

    ConfigStore(const ConfigStore&) = delete;
    ConfigStore& operator=(const ConfigStore&) = delete;

    Snapshot snapshot() const;
    std::string to_json() const;
    UpdateResult update(std::uint64_t expected_version, std::string_view object_json);
    const std::filesystem::path& path() const noexcept;

private:
    static bool parse_values(std::string_view json, Values& values);
    static bool validate(const Values& values) noexcept;
    static std::string serialize(const Snapshot& snapshot);
    static bool write_atomic(
        const std::filesystem::path& path, std::string_view contents) noexcept;
    static std::optional<std::string> read_file(const std::filesystem::path& path);

    std::filesystem::path file_;
    mutable std::mutex mutex_;
    Snapshot snapshot_;
};

}  // namespace uhf::config
