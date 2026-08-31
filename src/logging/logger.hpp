// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace uhf::logging {

enum class Level {
    debug,
    info,
    warning,
    error,
    critical,
};

enum class Component {
    acquisition,
    modbus_rtu,
    modbus_tcp,
    iec61850,
    web,
    auth,
    config,
    storage,
    system,
};

struct Field {
    std::string key;
    std::string value;
};

struct Entry {
    std::chrono::system_clock::time_point timestamp_utc;
    std::uint64_t sequence{0U};
    Level level{Level::info};
    Component component{Component::system};
    std::string event_code;
    std::string message;
    std::vector<Field> fields;
};

struct Options {
    Level minimum_level{Level::info};
    std::size_t max_recent_entries{500U};
    std::size_t max_recent_bytes{512U * 1024U};
    std::size_t max_throttle_records{64U};
    std::chrono::seconds throttle_window{60};
    bool use_syslog{true};
};

class Logger {
public:
    explicit Logger(Options options = {});
    ~Logger();

    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    bool log(
        Level level,
        Component component,
        std::string_view event_code,
        std::string_view message,
        const std::vector<Field>& fields = {});
    bool log_at(
        Level level,
        Component component,
        std::string_view event_code,
        std::string_view message,
        const std::vector<Field>& fields,
        std::chrono::steady_clock::time_point monotonic_now,
        std::chrono::system_clock::time_point utc_now);
    bool recovered(
        Component component,
        std::string_view event_code,
        std::string_view message,
        const std::vector<Field>& fields = {});
    bool recovered_at(
        Component component,
        std::string_view event_code,
        std::string_view message,
        const std::vector<Field>& fields,
        std::chrono::steady_clock::time_point monotonic_now,
        std::chrono::system_clock::time_point utc_now);

    std::vector<Entry> recent(std::size_t limit = 500U) const;
    std::size_t suppressed_count() const noexcept;
    std::size_t evicted_recent_count() const noexcept;

private:
    struct ThrottleState {
        std::chrono::steady_clock::time_point last_emitted;
        std::chrono::steady_clock::time_point last_seen;
        std::size_t suppressed{0U};
    };

    static int level_rank(Level level) noexcept;
    static int syslog_priority(Level level) noexcept;
    static std::string_view level_name(Level level) noexcept;
    static std::string_view component_name(Component component) noexcept;
    static bool is_throttled(Level level) noexcept;
    static bool sensitive_key(std::string_view key) noexcept;
    static std::string clean_text(std::string_view value, std::size_t limit);
    static std::size_t entry_bytes(const Entry& entry) noexcept;
    static std::string throttle_key(Component component, std::string_view event_code);

    Entry normalize(
        Level level,
        Component component,
        std::string_view event_code,
        std::string_view message,
        const std::vector<Field>& fields,
        std::chrono::system_clock::time_point utc_now) const;
    bool emit_locked(Entry entry);
    void trim_recent_locked();
    void evict_oldest_throttle_locked();
    std::string format_syslog(const Entry& entry) const;

    Options options_;
    mutable std::mutex mutex_;
    std::uint64_t next_sequence_{1U};
    std::size_t recent_bytes_{0U};
    std::size_t suppressed_count_{0U};
    std::size_t evicted_recent_count_{0U};
    std::deque<Entry> recent_entries_;
    std::unordered_map<std::string, ThrottleState> throttle_;
};

}  // namespace uhf::logging
