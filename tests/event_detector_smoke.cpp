// SPDX-License-Identifier: GPL-3.0-only
#include "storage/event_detector.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>

namespace {

using Clock = std::chrono::steady_clock;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "event detector smoke failed: " << message << '\n';
        return false;
    }
    return true;
}

Clock::time_point at_seconds(std::int64_t seconds) {
    return Clock::time_point(std::chrono::seconds(seconds));
}

uhf::acquisition::PublishedSnapshot snapshot(
    std::uint64_t generation,
    std::int32_t peak_dbm,
    bool valid = true,
    uhf::domain::PayloadStatus status = uhf::domain::PayloadStatus::good) {
    uhf::acquisition::PublishedSnapshot result;
    result.generation = generation;
    result.payload.payload_status = status;
    result.payload.measurements[2U] = uhf::domain::Measurement{0U, peak_dbm, valid};
    return result;
}

void observe(
    uhf::storage::EventDetector& detector,
    std::uint64_t generation,
    std::int32_t peak_dbm,
    std::int64_t seconds,
    bool fresh = true,
    bool valid = true,
    uhf::domain::PayloadStatus status = uhf::domain::PayloadStatus::good) {
    detector.observe(snapshot(generation, peak_dbm, valid, status), at_seconds(seconds), fresh);
}

bool check_generations(
    const uhf::storage::EventBundle& bundle,
    const std::uint64_t* expected,
    std::size_t count) {
    if (!expect(bundle.frames.size() == count, "event frame count")) {
        return false;
    }
    for (std::size_t index = 0; index < count; ++index) {
        if (!expect(bundle.frames[index].generation == expected[index], "event frame generation")) {
            return false;
        }
    }
    return true;
}

bool test_combined_and_merge() {
    uhf::storage::EventDetector detector;
    observe(detector, 1U, -60, 0);
    observe(detector, 2U, -55, 1);
    observe(detector, 3U, -40, 2);
    observe(detector, 4U, -40, 3);
    observe(detector, 5U, -40, 4);
    observe(detector, 6U, -40, 5);
    observe(detector, 7U, -50, 10);

    if (!expect(detector.advance(at_seconds(61)).empty(), "merge window remains open")) {
        return false;
    }
    const std::vector<uhf::storage::EventBundle> bundles = detector.advance(at_seconds(62));
    if (!expect(bundles.size() == 1U, "one merged bundle") || bundles.empty()) {
        return false;
    }
    const uhf::storage::EventBundle& bundle = bundles.front();
    const std::uint64_t expected[] = {1U, 2U, 3U, 4U, 5U, 6U};
    return expect(bundle.has_reason(uhf::storage::EventType::strong_discharge),
               "strong reason present") &&
        expect(bundle.has_reason(uhf::storage::EventType::sudden_change),
            "sudden reason present") &&
        expect(bundle.strong.trigger_count == 1U, "strong trigger count") &&
        expect(bundle.sudden.trigger_count == 2U, "sudden merge count") &&
        expect(bundle.sudden.max_peak_dbm == -40, "sudden maximum peak") &&
        expect(bundle.sudden.max_delta_db == 15, "sudden maximum delta") &&
        expect(!bundle.partial, "complete post collection") &&
        check_generations(bundle, expected, 6U);
}

bool test_strong_rearm() {
    uhf::storage::EventDetector detector;
    observe(detector, 1U, -55, 0);
    observe(detector, 2U, -50, 1);
    observe(detector, 3U, -45, 2);
    if (!expect(!detector.strong_armed(), "strong event disarms")) {
        return false;
    }
    observe(detector, 4U, -50, 3);
    if (!expect(!detector.strong_armed(), "rearm uses strictly below threshold")) {
        return false;
    }
    observe(detector, 5U, -51, 4);
    if (!expect(detector.strong_armed(), "strong event rearms below threshold")) {
        return false;
    }
    observe(detector, 6U, -45, 5);
    if (!expect(!detector.strong_armed(), "second strong event disarms")) {
        return false;
    }
    const std::vector<uhf::storage::EventBundle> bundles = detector.advance(at_seconds(62));
    if (!expect(bundles.size() == 1U, "rearmed events merge") || bundles.empty()) {
        return false;
    }
    return expect(bundles.front().strong.trigger_count == 2U, "rearmed trigger count");
}

bool test_invalid_and_stale_do_not_update_baseline() {
    uhf::storage::EventDetector detector;
    observe(detector, 1U, -70, 0);
    if (!expect(detector.previous_valid_peak() == std::optional<std::int32_t>{-70},
            "initial valid baseline")) {
        return false;
    }
    observe(detector, 2U, -50, 1, false);
    observe(detector, 3U, -62, 2, true, true, uhf::domain::PayloadStatus::not_refreshed);
    observe(detector, 4U, -62, 3, true, false);
    if (!expect(detector.previous_valid_peak() == std::optional<std::int32_t>{-70},
            "invalid frames preserve baseline") ||
        !expect(detector.advance(at_seconds(64)).empty(), "invalid frames do not trigger")) {
        return false;
    }
    observe(detector, 5U, -60, 5);
    observe(detector, 6U, -50, 6);
    const std::vector<uhf::storage::EventBundle> bundles = detector.advance(at_seconds(66 + 60));
    return expect(bundles.size() == 1U, "valid change triggers after skipped frames") &&
        expect(bundles.front().sudden.max_delta_db == 10, "valid baseline delta");
}

bool test_partial_post_collection() {
    uhf::storage::EventDetector detector;
    observe(detector, 1U, -55, 0);
    observe(detector, 2U, -50, 1);
    observe(detector, 3U, -45, 2);
    observe(detector, 4U, -44, 3);
    if (!expect(detector.advance(at_seconds(32)).empty(), "partial waits for merge window")) {
        return false;
    }
    const std::vector<uhf::storage::EventBundle> bundles = detector.advance(at_seconds(62));
    if (!expect(bundles.size() == 1U, "partial event closes") || bundles.empty()) {
        return false;
    }
    const std::uint64_t expected[] = {1U, 2U, 3U, 4U};
    return expect(bundles.front().partial, "partial marker") &&
        check_generations(bundles.front(), expected, 4U);
}

bool test_update_options() {
    uhf::storage::EventOptions options;
    options.strong_trigger_dbm = -40;
    options.strong_rearm_dbm = -50;
    options.sudden_delta_db = 85;
    options.merge_window = std::chrono::seconds(1);
    uhf::storage::EventDetector detector(options);
    observe(detector, 1U, -60, 0);
    observe(detector, 2U, -42, 1);
    if (!expect(detector.strong_armed(), "initial event threshold was applied")) {
        return false;
    }

    options.strong_trigger_dbm = -45;
    detector.update_options(options);
    observe(detector, 3U, -42, 2);
    const std::vector<uhf::storage::EventBundle> bundles = detector.advance(at_seconds(4));
    return expect(bundles.size() == 1U, "updated event threshold triggered") &&
        expect(bundles.front().has_reason(uhf::storage::EventType::strong_discharge),
            "updated strong event reason");
}

}  // namespace

int main() {
    if (!test_combined_and_merge() || !test_strong_rearm() ||
        !test_invalid_and_stale_do_not_update_baseline() || !test_partial_post_collection() ||
        !test_update_options()) {
        return 1;
    }
    std::cout << "event detector smoke: OK\n";
    return 0;
}
