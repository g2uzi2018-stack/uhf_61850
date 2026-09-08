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
    std::string modbus_tcp_bind{"192.168.3.230"};
    std::uint8_t modbus_tcp_unit_id{1U};
    std::uint16_t modbus_tcp_port{502U};
    std::uint16_t web_port{8080U};
    bool tls_enabled{true};
    bool iec_enabled{true};
    std::uint16_t iec_port{102U};
    std::string iec_ied_name{"UHFPD1"};
    bool ftp_enabled{false};
    std::uint16_t ftp_port{21U};
    std::string overview_title{"局部放电在线监测系统"};
    std::string overview_device{"1号主变"};
    std::uint16_t phase_start_degree{0U};
    bool time_sync_enabled{true};
    std::string sntp_server{"pool.ntp.org"};
    std::uint32_t storage_period_seconds{300U};
    std::uint32_t storage_retention_days{1U};
    std::uint64_t storage_min_free_bytes{536870912U};
    std::int32_t storage_event_threshold_dbm{-45};
    std::int32_t storage_event_rearm_dbm{-50};
    std::uint32_t storage_event_delta_db{10U};
    std::uint32_t storage_event_merge_seconds{60U};
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
    explicit ConfigStore(
        std::filesystem::path file, std::filesystem::path defaults_file = {});

    ConfigStore(const ConfigStore&) = delete;
    ConfigStore& operator=(const ConfigStore&) = delete;

    Snapshot snapshot() const;
    std::string to_json() const;
    UpdateResult update(std::uint64_t expected_version, std::string_view object_json);
    bool set_iec_ied_name(std::string_view ied_name);
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
