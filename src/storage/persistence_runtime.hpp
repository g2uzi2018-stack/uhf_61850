// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "acquisition/acquisition.hpp"
#include "logging/logger.hpp"
#include "storage/event_detector.hpp"
#include "storage/event_store.hpp"
#include "storage/frame_store.hpp"
#include "storage/storage_cleaner.hpp"

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
    std::filesystem::path data_root{"/var/lib/uhf-gateway/data"};
    std::chrono::seconds periodic_period{std::chrono::seconds(300)};
    std::chrono::seconds cleanup_period{std::chrono::hours(1)};
    CleanerOptions cleaner_options{};
    EventOptions event_options{};
};

struct PersistenceStats {
    bool low_watermark_active{false};
    bool writes_paused{false};
    bool cleanup_failed{false};
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

private:
    void run();
    void update_cleanup_state(const CleanupResult& result);
    void save_completed_events(std::vector<EventBundle> bundles);
    void increment_dropped_frame();

    acquisition::SnapshotStore& snapshot_store_;
    logging::Logger& logger_;
    PersistenceOptions options_;
    FrameStore frame_store_;
    EventBundleStore event_store_;
    StorageCleaner cleaner_;
    EventDetector event_detector_;
    std::atomic<bool> stop_requested_{false};
    std::thread worker_;
    mutable std::mutex stats_mutex_;
    PersistenceStats stats_;
};

}  // namespace uhf::storage
