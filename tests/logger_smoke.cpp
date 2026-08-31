// SPDX-License-Identifier: GPL-3.0-only
#include "logging/logger.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "logger smoke failed: " << message << '\n';
        return false;
    }
    return true;
}

Clock::time_point at_seconds(std::int64_t seconds) {
    return Clock::time_point(std::chrono::seconds(seconds));
}

std::chrono::system_clock::time_point utc_at(std::int64_t seconds) {
    return std::chrono::system_clock::time_point(std::chrono::seconds(seconds));
}

}  // namespace

int main() {
    uhf::logging::Options options;
    options.use_syslog = false;
    options.max_recent_entries = 8U;
    options.max_recent_bytes = 4096U;
    options.throttle_window = std::chrono::seconds(60);
    uhf::logging::Logger logger(options);

    const std::vector<uhf::logging::Field> fields = {
        {"unit", "485-1"},
        {"password", "must-not-appear"},
        {"session_cookie", "must-not-appear"},
        {"spectrum", "must-not-appear"},
    };
    if (!expect(logger.log_at(
                    uhf::logging::Level::info,
                    uhf::logging::Component::system,
                    "startup",
                    "gateway started",
                    fields,
                    at_seconds(0),
                    utc_at(1)),
            "info accepted") ||
        !expect(!logger.log_at(
                    uhf::logging::Level::debug,
                    uhf::logging::Component::system,
                    "debug-only",
                    "filtered",
                    {},
                    at_seconds(0),
                    utc_at(1)),
            "debug filtered")) {
        return 1;
    }
    const std::vector<uhf::logging::Entry> initial = logger.recent();
    if (!expect(initial.size() == 1U, "initial entry count") ||
        !expect(initial.front().fields.size() == 1U, "sensitive fields omitted") ||
        !expect(initial.front().fields.front().key == "unit", "safe field retained")) {
        return 1;
    }

    if (!expect(logger.log_at(
                    uhf::logging::Level::error,
                    uhf::logging::Component::acquisition,
                    "poll.timeout",
                    "PD1000 response timeout",
                    {},
                    at_seconds(10),
                    utc_at(10)),
            "first error emitted") ||
        !expect(!logger.log_at(
                    uhf::logging::Level::error,
                    uhf::logging::Component::acquisition,
                    "poll.timeout",
                    "PD1000 response timeout",
                    {},
                    at_seconds(11),
                    utc_at(11)),
            "repeated error throttled") ||
        !expect(logger.suppressed_count() == 1U, "suppressed counter") ||
        !expect(logger.recovered_at(
                    uhf::logging::Component::acquisition,
                    "poll.timeout",
                    "PD1000 polling recovered",
                    {},
                    at_seconds(12),
                    utc_at(12)),
            "recovery emitted") ||
        !expect(!logger.recovered_at(
                    uhf::logging::Component::acquisition,
                    "poll.timeout",
                    "duplicate recovery",
                    {},
                    at_seconds(13),
                    utc_at(13)),
            "duplicate recovery suppressed")) {
        return 1;
    }
    const std::vector<uhf::logging::Entry> entries = logger.recent();
    if (!expect(entries.size() == 3U, "recovery entry count") ||
        !expect(entries.back().event_code == "poll.timeout.recovered", "recovery event code") ||
        !expect(entries.back().fields.size() == 1U &&
                entries.back().fields.front().key == "suppressed_count" &&
                entries.back().fields.front().value == "1",
            "recovery suppressed count")) {
        return 1;
    }

    uhf::logging::Options bounded_options;
    bounded_options.use_syslog = false;
    bounded_options.max_recent_entries = 2U;
    bounded_options.max_recent_bytes = 4096U;
    uhf::logging::Logger bounded(bounded_options);
    bounded.log(
        uhf::logging::Level::info, uhf::logging::Component::system, "one", "one");
    bounded.log(
        uhf::logging::Level::info, uhf::logging::Component::system, "two", "two");
    bounded.log(
        uhf::logging::Level::info, uhf::logging::Component::system, "three", "three");
    const std::vector<uhf::logging::Entry> bounded_entries = bounded.recent();
    if (!expect(bounded_entries.size() == 2U, "recent entry bound") ||
        !expect(bounded_entries.front().event_code == "two", "oldest entry evicted") ||
        !expect(bounded.evicted_recent_count() == 1U, "eviction counter")) {
        return 1;
    }

    std::cout << "logger smoke: OK\n";
    return 0;
}
