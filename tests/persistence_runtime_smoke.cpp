// SPDX-License-Identifier: GPL-3.0-only
#include "acquisition/acquisition.hpp"
#include "domain/snapshot.hpp"
#include "logging/logger.hpp"
#include "storage/event_store.hpp"
#include "storage/persistence_runtime.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
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

bool wait_for(
    const std::function<bool()>& condition,
    std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (condition()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    return condition();
}

bool replace_once(std::string& value, const std::string& from, const std::string& to) {
    const std::size_t position = value.find(from);
    if (position == std::string::npos) {
        return false;
    }
    value.replace(position, from.size(), to);
    return true;
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

bool run_event_configuration_reload_case(uhf::logging::Logger& logger) {
    const std::filesystem::path root = std::filesystem::temp_directory_path() /
        ("uhf-persistence-config-reload-" + std::to_string(static_cast<long long>(::getpid())));
    std::error_code cleanup_error;
    std::filesystem::remove_all(root, cleanup_error);
    uhf::config::ConfigStore config(root / "config.json");
    std::string initial = config.to_json();
    if (!replace_once(initial, "\"storage_event_threshold_dbm\": -45",
            "\"storage_event_threshold_dbm\": -40") ||
        !replace_once(initial, "\"storage_event_delta_db\": 10",
            "\"storage_event_delta_db\": 85") ||
        !replace_once(initial, "\"storage_event_merge_seconds\": 60",
            "\"storage_event_merge_seconds\": 1") ||
        !expect(
            config.update(1U, initial) == uhf::config::UpdateResult::updated,
            "initial event configuration update")) {
        std::filesystem::remove_all(root, cleanup_error);
        return false;
    }

    uhf::acquisition::SnapshotStore snapshot_store;
    uhf::storage::PersistenceOptions options;
    options.config_store = &config;
    options.data_root = root / "data";
    options.cleanup_period = std::chrono::hours(1);
    options.cleaner_options.min_free_bytes = 0U;
    options.cleaner_options.low_watermark_percent = 0U;
    options.cleaner_options.recovery_percent = 0U;
    options.cleaner_options.recovery_extra_bytes = 0U;
    options.event_options.post_collection_timeout = std::chrono::seconds(1);
    uhf::storage::PersistenceWorker worker(snapshot_store, logger, options);
    worker.start();
    publish(snapshot_store, -60, std::chrono::steady_clock::now());
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    publish(snapshot_store, -42, std::chrono::steady_clock::now());
    std::this_thread::sleep_for(std::chrono::milliseconds(250));

    std::string reloaded = config.to_json();
    if (!replace_once(reloaded, "\"storage_event_threshold_dbm\": -40",
            "\"storage_event_threshold_dbm\": -45") ||
        !expect(
            config.update(2U, reloaded) == uhf::config::UpdateResult::updated,
            "runtime event configuration update")) {
        worker.stop();
        std::filesystem::remove_all(root, cleanup_error);
        return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    publish(snapshot_store, -42, std::chrono::steady_clock::now());
    const bool saved = wait_for_file_count(root / "data" / "events", 1U,
        std::chrono::seconds(3));
    worker.stop();
    const uhf::storage::PersistenceStats stats = worker.stats();
    std::filesystem::remove_all(root, cleanup_error);
    return expect(saved, "event threshold hot reload produced an event") &&
        expect(stats.saved_event_count == 1U, "one reloaded event saved");
}

bool run_v3_history_case(uhf::logging::Logger& logger) {
    const std::filesystem::path root = std::filesystem::temp_directory_path() /
        ("uhf-persistence-v3-" + std::to_string(static_cast<long long>(::getpid())));
    std::error_code cleanup_error;
    std::filesystem::remove_all(root, cleanup_error);
    uhf::acquisition::SnapshotStore legacy_store;
    uhf::v3::SnapshotStore v3_store;
    uhf::storage::PersistenceOptions options;
    options.data_root = root;
    options.v3_snapshot_store = &v3_store;
    options.periodic_period = std::chrono::seconds::zero();
    options.cleanup_period = std::chrono::hours(1);
    options.cleaner_options.min_free_bytes = 0U;
    options.cleaner_options.low_watermark_percent = 0U;
    options.cleaner_options.recovery_percent = 0U;
    options.cleaner_options.recovery_extra_bytes = 0U;
    uhf::storage::PersistenceWorker worker(legacy_store, logger, options);
    worker.start();
    uhf::v3::TemperatureValues temperature{};
    temperature[0] = uhf::v3::valid_value(20.0F);
    v3_store.publish_temperature(temperature, std::chrono::steady_clock::now());
    const bool saved = wait_for_file_count(root / "v3", 1U, std::chrono::seconds(2));
    worker.stop();
    const auto paths = uhf::storage::V3HistoryStore(root / "v3").list(1U);
    const bool readable = saved && !paths.empty() &&
        uhf::storage::V3HistoryStore(root / "v3").read(paths.front()).has_value();
    std::filesystem::remove_all(root, cleanup_error);
    return expect(readable, "v3 history record saved and readable");
}

bool run_v3_write_failure_recovery_case(uhf::logging::Logger& logger) {
    const std::filesystem::path root = std::filesystem::temp_directory_path() /
        ("uhf-persistence-v3-failure-" +
         std::to_string(static_cast<long long>(::getpid())));
    std::error_code error;
    std::filesystem::remove_all(root, error);
    uhf::acquisition::SnapshotStore legacy_store;
    uhf::v3::SnapshotStore v3_store;
    uhf::storage::PersistenceOptions options;
    options.data_root = root;
    options.v3_snapshot_store = &v3_store;
    options.periodic_period = std::chrono::seconds::zero();
    options.cleanup_period = std::chrono::hours(1);
    options.cleaner_options.min_free_bytes = 0U;
    options.cleaner_options.low_watermark_percent = 0U;
    options.cleaner_options.recovery_percent = 0U;
    options.cleaner_options.recovery_extra_bytes = 0U;
    uhf::storage::PersistenceWorker worker(legacy_store, logger, options);

    const std::filesystem::path history = root / "v3";
    const std::filesystem::path backup = root / "v3-backup";
    std::filesystem::rename(history, backup, error);
    if (!expect(!error, "prepare deterministic v3 write failure")) {
        std::filesystem::remove_all(root, error);
        return false;
    }
    std::ofstream blocker(history, std::ios::binary);
    blocker << "not a directory";
    blocker.close();

    uhf::v3::TemperatureValues temperature{};
    temperature[0] = uhf::v3::valid_value(20.0F);
    worker.start();
    v3_store.publish_temperature(temperature, std::chrono::steady_clock::now());
    const bool failure_observed = wait_for(
        [&worker] {
            const uhf::storage::PersistenceStats stats = worker.stats();
            return stats.write_failed && stats.dropped_frame_count == 1U;
        },
        std::chrono::seconds(2));
    worker.stop();
    const auto failure_stats = worker.stats();
    const auto recent_logs = logger.recent(100U);
    const bool failure_logged = std::any_of(
        recent_logs.begin(), recent_logs.end(), [](const uhf::logging::Entry& entry) {
            return entry.event_code == "v3_history.save_failed";
        });

    std::filesystem::remove(history, error);
    error.clear();
    std::filesystem::rename(backup, history, error);
    if (!expect(failure_observed, "v3 write failure updates runtime stats") ||
        !expect(failure_stats.saved_frame_count == 0U, "failed v3 record is not counted saved") ||
        !expect(failure_logged, "v3 write failure is logged") ||
        !expect(!error, "restore v3 history directory")) {
        std::filesystem::remove_all(root, error);
        return false;
    }

    temperature[0] = uhf::v3::valid_value(21.0F);
    v3_store.publish_temperature(temperature, std::chrono::steady_clock::now());
    worker.start();
    const bool recovered = wait_for(
        [&worker] {
            const uhf::storage::PersistenceStats stats = worker.stats();
            return !stats.write_failed && stats.saved_frame_count == 1U;
        },
        std::chrono::seconds(2));
    worker.stop();
    const auto paths = uhf::storage::V3HistoryStore(history).list(10U);
    std::filesystem::remove_all(root, error);
    return expect(recovered, "successful v3 write clears failure state") &&
        expect(paths.size() == 1U, "recovery saves exactly one intact v3 record");
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
        if (!run_event_configuration_reload_case(logger)) {
            return 1;
        }
        if (!run_v3_history_case(logger)) {
            return 1;
        }
        if (!run_v3_write_failure_recovery_case(logger)) {
            return 1;
        }
        uhf::acquisition::SnapshotStore snapshot_store;
        uhf::storage::PersistenceOptions options;
        options.data_root = root;
        options.periodic_period = std::chrono::seconds::zero();
        options.cleanup_period = std::chrono::hours(1);
        options.cleaner_options.min_free_bytes = 0U;
        options.cleaner_options.low_watermark_percent = 0U;
        options.cleaner_options.recovery_percent = 0U;
        options.cleaner_options.recovery_extra_bytes = 0U;
        options.event_options.post_collection_timeout = std::chrono::seconds(1);
        options.event_options.merge_window = std::chrono::seconds(1);
        uhf::storage::PersistenceWorker worker(snapshot_store, logger, options);
        worker.start();

        std::size_t published_count = 0U;
        for (const std::int32_t peak : {-60, -40, -41, -42, -43}) {
            const auto now = std::chrono::steady_clock::now();
            publish(snapshot_store, peak, now);
            ++published_count;
            if (!expect(
                    wait_for(
                        [&worker, published_count] {
                            return worker.stats().saved_frame_count >= published_count;
                        },
                        std::chrono::seconds(2)),
                    "worker observed published frame")) {
                worker.stop();
                std::filesystem::remove_all(root, cleanup_error);
                return 1;
            }
        }

        if (!expect(
                wait_for_file_count(root / "frames", 5U, std::chrono::seconds(2)),
                "published frames saved") ||
            !expect(
                wait_for_file_count(root / "events", 1U, std::chrono::seconds(3)),
                "completed event saved")) {
            worker.stop();
            std::filesystem::remove_all(root, cleanup_error);
            return 1;
        }

        worker.stop();
        const uhf::storage::PersistenceStats stats = worker.stats();
        if (!expect(stats.saved_frame_count == 5U, "frame save count") ||
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
