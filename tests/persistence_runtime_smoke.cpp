// SPDX-License-Identifier: GPL-3.0-only
#include "acquisition/acquisition.hpp"
#include "domain/snapshot.hpp"
#include "logging/logger.hpp"
#include "storage/event_store.hpp"
#include "storage/persistence_runtime.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>
#include <unistd.h>

namespace {

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "persistence runtime smoke failed: " << message << '\n';
        return false;
    }
    return true;
}

std::uint16_t encode_peak(std::int32_t peak) {
    return static_cast<std::uint16_t>(static_cast<std::int16_t>(peak));
}

void publish(
    uhf::acquisition::SnapshotStore& store,
    std::int32_t peak,
    std::chrono::steady_clock::time_point now) {
    std::array<std::uint16_t, uhf::domain::kPd1000RegisterCount> raw{};
    raw[0] = encode_peak(-60);
    raw[1] = 12U;
    raw[2] = encode_peak(peak);
    raw[3] = 180U;
    raw[4] = 240U;
    store.publish(uhf::domain::parse_pd1000_registers(raw), now, now);
}

bool wait_for_file_count(
    const std::filesystem::path& directory,
    std::size_t expected,
    std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        std::size_t count = 0U;
        std::error_code error;
        for (const auto& entry : std::filesystem::directory_iterator(directory, error)) {
            if (!error && entry.path().extension() == ".bin") {
                ++count;
            }
        }
        if (!error && count >= expected) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return false;
}

bool run_stale_event_case(uhf::logging::Logger& logger) {
    const std::filesystem::path root = std::filesystem::temp_directory_path() /
        ("uhf-persistence-stale-" + std::to_string(static_cast<long long>(::getpid())));
    std::error_code cleanup_error;
    std::filesystem::remove_all(root, cleanup_error);
    uhf::acquisition::SnapshotStore snapshot_store;
    const auto now = std::chrono::steady_clock::now();
    publish(snapshot_store, -40, now);
    snapshot_store.record_failure(now, "serial timeout");

    uhf::storage::PersistenceOptions options;
    options.data_root = root;
    options.periodic_period = std::chrono::hours(1);
    options.cleanup_period = std::chrono::hours(1);
    options.cleaner_options.min_free_bytes = 0U;
    options.cleaner_options.low_watermark_percent = 0U;
    options.cleaner_options.recovery_percent = 0U;
    options.cleaner_options.recovery_extra_bytes = 0U;
    options.event_options.post_collection_timeout = std::chrono::seconds(1);
    options.event_options.merge_window = std::chrono::seconds(1);

    uhf::storage::PersistenceWorker worker(snapshot_store, logger, options);
    worker.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(2200));
    worker.stop();
    const uhf::storage::PersistenceStats stats = worker.stats();
    std::filesystem::remove_all(root, cleanup_error);
    return expect(stats.saved_event_count == 0U, "stale snapshot triggered an event");
}

}  // namespace

int main() {
    const std::filesystem::path root = std::filesystem::temp_directory_path() /
        ("uhf-persistence-runtime-" + std::to_string(static_cast<long long>(::getpid())));
    std::error_code cleanup_error;
    std::filesystem::remove_all(root, cleanup_error);
    try {
        uhf::logging::Options logger_options;
        logger_options.use_syslog = false;
        uhf::logging::Logger logger(logger_options);
        if (!run_stale_event_case(logger)) {
            return 1;
        }
        uhf::acquisition::SnapshotStore snapshot_store;
        uhf::storage::PersistenceOptions options;
        options.data_root = root;
        options.periodic_period = std::chrono::seconds(1);
        options.cleanup_period = std::chrono::hours(1);
        options.cleaner_options.min_free_bytes = 0U;
        options.cleaner_options.low_watermark_percent = 0U;
        options.cleaner_options.recovery_percent = 0U;
        options.cleaner_options.recovery_extra_bytes = 0U;
        options.event_options.post_collection_timeout = std::chrono::seconds(1);
        options.event_options.merge_window = std::chrono::seconds(1);
        uhf::storage::PersistenceWorker worker(snapshot_store, logger, options);
        worker.start();

        for (const std::int32_t peak : {-60, -40, -41, -42, -43}) {
            const auto now = std::chrono::steady_clock::now();
            publish(snapshot_store, peak, now);
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
        }

        if (!expect(
                wait_for_file_count(root / "frames", 1U, std::chrono::seconds(2)),
                "periodic frame saved") ||
            !expect(
                wait_for_file_count(root / "events", 1U, std::chrono::seconds(3)),
                "completed event saved")) {
            worker.stop();
            std::filesystem::remove_all(root, cleanup_error);
            return 1;
        }

        worker.stop();
        const uhf::storage::PersistenceStats stats = worker.stats();
        if (!expect(stats.saved_frame_count >= 1U, "frame save count") ||
            !expect(stats.saved_event_count == 1U, "event save count") ||
            !expect(stats.dropped_frame_count == 0U, "no dropped frames") ||
            !expect(stats.dropped_event_count == 0U, "no dropped events")) {
            std::filesystem::remove_all(root, cleanup_error);
            return 1;
        }

        std::size_t event_count = 0U;
        for (const auto& entry : std::filesystem::directory_iterator(root / "events")) {
            if (entry.path().extension() != ".bin") {
                continue;
            }
            uhf::storage::EventBundleStore store(root / "events");
            const auto bundle = store.read(entry.path());
            if (!expect(bundle.has_value(), "event round trip") || !bundle) {
                std::filesystem::remove_all(root, cleanup_error);
                return 1;
            }
            if (!expect(bundle->reason_mask == 3U, "combined event reasons") ||
                !expect(bundle->frames.size() == 5U, "event frame bound") ||
                !expect(!bundle->partial, "complete event collection")) {
                std::filesystem::remove_all(root, cleanup_error);
                return 1;
            }
            ++event_count;
        }
        if (!expect(event_count == 1U, "one event file")) {
            std::filesystem::remove_all(root, cleanup_error);
            return 1;
        }

        std::filesystem::remove_all(root, cleanup_error);
        std::cout << "persistence runtime smoke: OK\n";
        return 0;
    } catch (const std::exception& error) {
        std::filesystem::remove_all(root, cleanup_error);
        std::cerr << "persistence runtime smoke failed: " << error.what() << '\n';
        return 1;
    }
}
