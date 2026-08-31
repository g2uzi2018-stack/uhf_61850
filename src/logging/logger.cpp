// SPDX-License-Identifier: GPL-3.0-only
#include "logging/logger.hpp"

#include <algorithm>
#include <cctype>
#include <iterator>
#include <syslog.h>
#include <utility>

namespace {

constexpr std::size_t kMaxEventCodeBytes = 64U;
constexpr std::size_t kMaxMessageBytes = 256U;
constexpr std::size_t kMaxFieldCount = 8U;
constexpr std::size_t kMaxFieldKeyBytes = 32U;
constexpr std::size_t kMaxFieldValueBytes = 128U;
constexpr std::size_t kMaxEntryBytes = 2048U;

bool contains_ascii_case_insensitive(std::string_view value, std::string_view needle) {
    if (needle.empty() || needle.size() > value.size()) {
        return false;
    }
    for (std::size_t offset = 0; offset + needle.size() <= value.size(); ++offset) {
        bool matches = true;
        for (std::size_t index = 0; index < needle.size(); ++index) {
            const unsigned char actual = static_cast<unsigned char>(value[offset + index]);
            const unsigned char expected = static_cast<unsigned char>(needle[index]);
            if (std::tolower(actual) != std::tolower(expected)) {
                matches = false;
                break;
            }
        }
        if (matches) {
            return true;
        }
    }
    return false;
}

}  // namespace

namespace uhf::logging {

Logger::Logger(Options options) : options_(std::move(options)) {
    if (options_.use_syslog) {
        ::openlog("uhf-gatewayd", LOG_PID | LOG_NDELAY, LOG_LOCAL0);
    }
}

Logger::~Logger() {
    if (options_.use_syslog) {
        ::closelog();
    }
}

int Logger::level_rank(Level level) noexcept {
    switch (level) {
    case Level::debug:
        return 0;
    case Level::info:
        return 1;
    case Level::warning:
        return 2;
    case Level::error:
        return 3;
    case Level::critical:
        return 4;
    }
    return 4;
}

int Logger::syslog_priority(Level level) noexcept {
    switch (level) {
    case Level::debug:
        return LOG_DEBUG;
    case Level::info:
        return LOG_INFO;
    case Level::warning:
        return LOG_WARNING;
    case Level::error:
        return LOG_ERR;
    case Level::critical:
        return LOG_CRIT;
    }
    return LOG_CRIT;
}

std::string_view Logger::level_name(Level level) noexcept {
    switch (level) {
    case Level::debug:
        return "DEBUG";
    case Level::info:
        return "INFO";
    case Level::warning:
        return "WARN";
    case Level::error:
        return "ERROR";
    case Level::critical:
        return "CRITICAL";
    }
    return "CRITICAL";
}

std::string_view Logger::component_name(Component component) noexcept {
    switch (component) {
    case Component::acquisition:
        return "acquisition";
    case Component::modbus_rtu:
        return "modbus-rtu";
    case Component::modbus_tcp:
        return "modbus-tcp";
    case Component::iec61850:
        return "iec61850";
    case Component::web:
        return "web";
    case Component::auth:
        return "auth";
    case Component::config:
        return "config";
    case Component::storage:
        return "storage";
    case Component::system:
        return "system";
    }
    return "system";
}

bool Logger::is_throttled(Level level) noexcept {
    return level_rank(level) >= level_rank(Level::warning);
}

bool Logger::sensitive_key(std::string_view key) noexcept {
    constexpr std::string_view sensitive[] = {
        "password", "passwd", "cookie", "token", "secret", "private", "credential",
        "authorization", "auth", "key", "spectrum", "payload", "config",
    };
    for (const std::string_view word : sensitive) {
        if (contains_ascii_case_insensitive(key, word)) {
            return true;
        }
    }
    return false;
}

std::string Logger::clean_text(std::string_view value, std::size_t limit) {
    std::string result;
    result.reserve(std::min(value.size(), limit));
    for (const char character : value) {
        if (result.size() >= limit) {
            break;
        }
        const unsigned char unsigned_character = static_cast<unsigned char>(character);
        result.push_back(unsigned_character < 0x20U || unsigned_character == 0x7FU ? '?' : character);
    }
    return result;
}

std::size_t Logger::entry_bytes(const Entry& entry) noexcept {
    std::size_t result = entry.event_code.size() + entry.message.size() + 32U;
    for (const Field& field : entry.fields) {
        result += field.key.size() + field.value.size() + 2U;
    }
    return result;
}

std::string Logger::throttle_key(Component component, std::string_view event_code) {
    return std::string(component_name(component)) + ":" +
        clean_text(event_code, kMaxEventCodeBytes);
}

Entry Logger::normalize(
    Level level,
    Component component,
    std::string_view event_code,
    std::string_view message,
    const std::vector<Field>& fields,
    std::chrono::system_clock::time_point utc_now) const {
    Entry result;
    result.timestamp_utc = utc_now;
    result.level = level;
    result.component = component;
    result.event_code = clean_text(event_code, kMaxEventCodeBytes);
    result.message = clean_text(message, kMaxMessageBytes);
    if (result.event_code.empty()) {
        result.event_code = "unspecified";
    }
    result.fields.reserve(std::min(fields.size(), kMaxFieldCount));
    for (const Field& field : fields) {
        if (result.fields.size() >= kMaxFieldCount || sensitive_key(field.key)) {
            continue;
        }
        std::string key = clean_text(field.key, kMaxFieldKeyBytes);
        if (key.empty()) {
            continue;
        }
        for (char& character : key) {
            const unsigned char unsigned_character = static_cast<unsigned char>(character);
            if (!(std::isalnum(unsigned_character) != 0 || character == '_' || character == '-' ||
                character == '.')) {
                character = '_';
            }
        }
        result.fields.push_back(Field{std::move(key), clean_text(field.value, kMaxFieldValueBytes)});
    }
    while (entry_bytes(result) > kMaxEntryBytes && !result.fields.empty()) {
        result.fields.pop_back();
    }
    return result;
}

void Logger::trim_recent_locked() {
    while ((!options_.max_recent_entries || recent_entries_.size() > options_.max_recent_entries) ||
           (recent_bytes_ > options_.max_recent_bytes && !recent_entries_.empty())) {
        recent_bytes_ -= entry_bytes(recent_entries_.front());
        recent_entries_.pop_front();
        ++evicted_recent_count_;
    }
}

void Logger::evict_oldest_throttle_locked() {
    if (throttle_.empty()) {
        return;
    }
    auto oldest = throttle_.begin();
    for (auto iterator = std::next(throttle_.begin()); iterator != throttle_.end(); ++iterator) {
        if (iterator->second.last_seen < oldest->second.last_seen) {
            oldest = iterator;
        }
    }
    throttle_.erase(oldest);
}

std::string Logger::format_syslog(const Entry& entry) const {
    std::string result = "seq=" + std::to_string(entry.sequence) +
        " level=" + std::string(level_name(entry.level)) +
        " component=" + std::string(component_name(entry.component)) +
        " code=" + entry.event_code + " message=\"";
    for (const char character : entry.message) {
        if (character == '\\' || character == '"') {
            result.push_back('\\');
        }
        result.push_back(character);
    }
    result.push_back('"');
    for (const Field& field : entry.fields) {
        result.append(" ");
        result.append(field.key);
        result.append("=\"");
        for (const char character : field.value) {
            if (character == '\\' || character == '"') {
                result.push_back('\\');
            }
            result.push_back(character);
        }
        result.append("\"");
    }
    return result;
}

bool Logger::emit_locked(Entry entry) {
    entry.sequence = next_sequence_++;
    if (options_.use_syslog) {
        const std::string formatted = format_syslog(entry);
        ::syslog(syslog_priority(entry.level), "%s", formatted.c_str());
    }

    const std::size_t bytes = entry_bytes(entry);
    if (options_.max_recent_entries > 0U && options_.max_recent_bytes >= bytes) {
        recent_bytes_ += bytes;
        recent_entries_.push_back(std::move(entry));
        trim_recent_locked();
    }
    return true;
}

bool Logger::log(
    Level level,
    Component component,
    std::string_view event_code,
    std::string_view message,
    const std::vector<Field>& fields) {
    return log_at(
        level, component, event_code, message, fields,
        std::chrono::steady_clock::now(), std::chrono::system_clock::now());
}

bool Logger::log_at(
    Level level,
    Component component,
    std::string_view event_code,
    std::string_view message,
    const std::vector<Field>& fields,
    std::chrono::steady_clock::time_point monotonic_now,
    std::chrono::system_clock::time_point utc_now) {
    if (level_rank(level) < level_rank(options_.minimum_level)) {
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    const std::string key = throttle_key(component, event_code);
    std::size_t suppressed = 0U;
    if (is_throttled(level) && options_.throttle_window.count() > 0) {
        auto iterator = throttle_.find(key);
        if (iterator != throttle_.end() &&
            monotonic_now < iterator->second.last_emitted + options_.throttle_window) {
            ++iterator->second.suppressed;
            iterator->second.last_seen = monotonic_now;
            ++suppressed_count_;
            return false;
        }
        if (iterator == throttle_.end()) {
            if (options_.max_throttle_records == 0U) {
                return emit_locked(normalize(level, component, event_code, message, fields, utc_now));
            }
            if (throttle_.size() >= options_.max_throttle_records) {
                evict_oldest_throttle_locked();
            }
            iterator = throttle_.emplace(key, ThrottleState{monotonic_now, monotonic_now, 0U}).first;
        }
        suppressed = iterator->second.suppressed;
        iterator->second.last_emitted = monotonic_now;
        iterator->second.last_seen = monotonic_now;
        iterator->second.suppressed = 0U;
    }

    std::vector<Field> effective_fields = fields;
    if (suppressed > 0U) {
        effective_fields.push_back(Field{"suppressed_count", std::to_string(suppressed)});
    }
    return emit_locked(normalize(level, component, event_code, message, effective_fields, utc_now));
}

bool Logger::recovered(
    Component component,
    std::string_view event_code,
    std::string_view message,
    const std::vector<Field>& fields) {
    return recovered_at(
        component, event_code, message, fields,
        std::chrono::steady_clock::now(), std::chrono::system_clock::now());
}

bool Logger::recovered_at(
    Component component,
    std::string_view event_code,
    std::string_view message,
    const std::vector<Field>& fields,
    std::chrono::steady_clock::time_point monotonic_now,
    std::chrono::system_clock::time_point utc_now) {
    if (level_rank(Level::info) < level_rank(options_.minimum_level)) {
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    const std::string key = throttle_key(component, event_code);
    const auto iterator = throttle_.find(key);
    if (iterator == throttle_.end()) {
        return false;
    }
    const std::size_t suppressed = iterator->second.suppressed;
    throttle_.erase(iterator);

    std::vector<Field> effective_fields = fields;
    if (suppressed > 0U) {
        effective_fields.push_back(Field{"suppressed_count", std::to_string(suppressed)});
    }
    (void)monotonic_now;
    return emit_locked(normalize(
        Level::info, component, std::string(event_code) + ".recovered", message,
        effective_fields, utc_now));
}

std::vector<Entry> Logger::recent(std::size_t limit) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<Entry> result;
    if (limit == 0U) {
        return result;
    }
    const std::size_t begin = recent_entries_.size() > limit ? recent_entries_.size() - limit : 0U;
    result.reserve(recent_entries_.size() - begin);
    for (std::size_t index = begin; index < recent_entries_.size(); ++index) {
        result.push_back(recent_entries_[index]);
    }
    return result;
}

std::size_t Logger::suppressed_count() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return suppressed_count_;
}

std::size_t Logger::evicted_recent_count() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return evicted_recent_count_;
}

}  // namespace uhf::logging
