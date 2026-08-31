// SPDX-License-Identifier: GPL-3.0-only
#include "storage/persistence_runtime.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <utility>
#include <vector>

namespace {

constexpr auto kWorkerInterval = std::chrono::milliseconds(100);

}  // namespace

namespace uhf::storage {

PersistenceWorker::PersistenceWorker(
    acquisition::SnapshotStore& snapshot_store,
    logging::Logger& logger,
    PersistenceOptions options)
    : snapshot_store_(snapshot_store),
      logger_(logger),
      options_(std::move(options)),
      frame_store_(options_.data_root / "frames"),
      event_store_(options_.data_root / "events"),
      cleaner_(options_.data_root, options_.cleaner_options),
      event_detector_(options_.event_options) {}

PersistenceWorker::~PersistenceWorker() {
    stop();
}

void PersistenceWorker::start() {
    if (worker_.joinable()) {
        return;
    }
    stop_requested_.store(false);
    worker_ = std::thread(&PersistenceWorker::run, this);
}

void PersistenceWorker::stop() noexcept {
    stop_requested_.store(true);
    if (worker_.joinable()) {
        worker_.join();
    }
}

PersistenceStats PersistenceWorker::stats() const {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    return stats_;
}

bool PersistenceWorker::alarm_active() const noexcept {
    return alarm_active_.load();
}

void PersistenceWorker::update_cleanup_state(const CleanupResult& result) {
    {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        stats_.low_watermark_active = result.low_watermark_active;
        stats_.writes_paused = result.writes_paused;
        stats_.cleanup_failed = result.deletion_failed;
    }
    if (result.deletion_failed) {
        logger_.log(
            logging::Level::critical,
            logging::Component::storage,
            "cleanup.failed",
            "storage cleanup failed; new writes may be paused");
    }
}

void PersistenceWorker::increment_dropped_frame() {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    ++stats_.dropped_frame_count;
}

void PersistenceWorker::save_completed_events(std::vector<EventBundle> bundles) {
    for (EventBundle& bundle : bundles) {
        if (!cleaner_.accepting_writes()) {
            {
                std::lock_guard<std::mutex> lock(stats_mutex_);
                ++stats_.dropped_event_count;
            }
            logger_.log(
                logging::Level::critical,
                logging::Component::storage,
                "event.dropped",
                "completed event dropped while storage writes are paused");
            continue;
        }
        if (!event_store_.save(bundle)) {
            {
                std::lock_guard<std::mutex> lock(stats_mutex_);
                ++stats_.dropped_event_count;
            }
            logger_.log(
                logging::Level::error,
                logging::Component::storage,
                "event.save_failed",
                "unable to persist completed event bundle");
            continue;
        }
        std::lock_guard<std::mutex> lock(stats_mutex_);
        ++stats_.saved_event_count;
    }
}

void PersistenceWorker::run() {
    std::uint64_t last_generation = 0U;
    std::optional<std::chrono::steady_clock::time_point> last_periodic_save;
    std::uint64_t last_periodic_generation = 0U;
    std::uint64_t dropped_generation = 0U;
    const auto cleanup_interval = std::max(options_.cleanup_period, std::chrono::seconds::zero());
    auto next_cleanup = std::chrono::steady_clock::now();

    while (!stop_requested_.load()) {
        const auto now = std::chrono::steady_clock::now();
        const auto now_utc = std::chrono::system_clock::now();
        if (now >= next_cleanup) {
            update_cleanup_state(cleaner_.run(now_utc));
            next_cleanup = now + cleanup_interval;
            if (cleanup_interval == std::chrono::seconds::zero()) {
                next_cleanup = now + std::chrono::seconds(1);
            }
        }

        const acquisition::ServingView serving_view = snapshot_store_.serving_view();
        if (serving_view.snapshot && serving_view.snapshot->generation != 0U &&
            serving_view.snapshot->generation > last_generation) {
            last_generation = serving_view.snapshot->generation;
            event_detector_.observe(
                *serving_view.snapshot,
                now,
                serving_view.status.availability == acquisition::Availability::fresh,
                now_utc);
            alarm_active_.store(!event_detector_.strong_armed());

            const bool period_elapsed = !last_periodic_save ||
                options_.periodic_period <= std::chrono::seconds::zero() ||
                now >= *last_periodic_save + options_.periodic_period;
            if (period_elapsed && serving_view.snapshot->generation != last_periodic_generation) {
                if (!cleaner_.accepting_writes()) {
                    if (dropped_generation != serving_view.snapshot->generation) {
                        dropped_generation = serving_view.snapshot->generation;
                        increment_dropped_frame();
                    }
                    logger_.log(
                        logging::Level::critical,
                        logging::Component::storage,
                        "frame.dropped",
                        "periodic frame dropped while storage writes are paused");
                } else if (frame_store_.save(*serving_view.snapshot, now_utc)) {
                    last_periodic_save = now;
                    last_periodic_generation = serving_view.snapshot->generation;
                    dropped_generation = 0U;
                    std::lock_guard<std::mutex> lock(stats_mutex_);
                    ++stats_.saved_frame_count;
                } else {
                    if (dropped_generation != serving_view.snapshot->generation) {
                        dropped_generation = serving_view.snapshot->generation;
                        increment_dropped_frame();
                    }
                    logger_.log(
                        logging::Level::error,
                        logging::Component::storage,
                        "frame.save_failed",
                        "unable to persist periodic measurement frame");
                }
            }
        }

        save_completed_events(event_detector_.advance(now));

        const auto sleep_until = std::chrono::steady_clock::now() + kWorkerInterval;
        while (!stop_requested_.load()) {
            const auto remaining = sleep_until - std::chrono::steady_clock::now();
            if (remaining <= std::chrono::steady_clock::duration::zero()) {
                break;
            }
            std::this_thread::sleep_for(std::min(
                remaining,
                std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                    std::chrono::milliseconds(25))));
        }
    }
}

}  // namespace uhf::storage
