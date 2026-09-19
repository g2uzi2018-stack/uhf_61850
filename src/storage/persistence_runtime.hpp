// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "acquisition/acquisition.hpp"
#include "config/config_store.hpp"
#include "logging/logger.hpp"
#include "storage/event_detector.hpp"
#include "storage/event_store.hpp"
#include "storage/frame_store.hpp"
#include "storage/storage_cleaner.hpp"
#include "storage/v3_history_store.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <thread>

namespace uhf::storage {

struct PersistenceOptions {
    config::ConfigStore* config_store{nullptr};
    std::filesystem::path data_root{"/var/lib/uhf-gateway/data"};
    std::chrono::seconds periodic_period{std::chrono::seconds(300)};
    std::chrono::seconds cleanup_period{std::chrono::hours(1)};
    CleanerOptions cleaner_options{};
    EventOptions event_options{};
    const v3::SnapshotStore* v3_snapshot_store{nullptr};
};

struct PersistenceStats {
    bool low_watermark_active{false};
    bool writes_paused{false};
    bool cleanup_failed{false};
    bool write_failed{false};
    std::size_t saved_frame_count{0U};
    std::size_t saved_event_count{0U};
    std::size_t dropped_frame_count{0U};
    std::size_t dropped_event_count{0U};
};

class PersistenceWorker {
public:
    PersistenceWorker(
        acquisition::SnapshotStore& snapshot_store,
        logging::Logger& logger,
        PersistenceOptions options = {});
    ~PersistenceWorker();

    PersistenceWorker(const PersistenceWorker&) = delete;
    PersistenceWorker& operator=(const PersistenceWorker&) = delete;

    void start();
    void stop() noexcept;
    PersistenceStats stats() const;
    bool alarm_active() const noexcept;

private:
    void apply_runtime_configuration(std::uint64_t& applied_version);
    void run();
    void update_cleanup_state(const CleanupResult& result);
    void save_completed_events(std::vector<EventBundle> bundles);
    void increment_dropped_frame();

    acquisition::SnapshotStore& snapshot_store_;
    logging::Logger& logger_;
    PersistenceOptions options_;
    const v3::SnapshotStore* v3_snapshot_store_{nullptr};
    FrameStore frame_store_;
    std::unique_ptr<V3HistoryStore> v3_history_store_;
    EventBundleStore event_store_;
    StorageCleaner cleaner_;
    EventDetector event_detector_;
    std::atomic<bool> stop_requested_{false};
    std::thread worker_;
    mutable std::mutex stats_mutex_;
    PersistenceStats stats_;
    std::atomic<bool> alarm_active_{false};
};

}  // namespace uhf::storage
