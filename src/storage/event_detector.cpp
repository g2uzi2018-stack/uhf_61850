// SPDX-License-Identifier: GPL-3.0-only
#include "storage/event_detector.hpp"

#include <cstdint>
#include <limits>
#include <utility>

namespace uhf::storage {

bool EventBundle::has_reason(EventType type) const noexcept {
    const std::uint8_t bit = type == EventType::strong_discharge ? 1U : 2U;
    return (reason_mask & bit) != 0U;
}

EventDetector::EventDetector(EventOptions options) : options_(std::move(options)) {}

std::uint8_t EventDetector::reason_bit(EventType type) noexcept {
    return type == EventType::strong_discharge ? kStrongReason : kSuddenReason;
}

bool EventDetector::has_generation(
    const std::vector<acquisition::PublishedSnapshot>& frames,
    std::uint64_t generation) noexcept {
    for (const acquisition::PublishedSnapshot& frame : frames) {
        if (frame.generation == generation) {
            return true;
        }
    }
    return false;
}

void EventDetector::update_stats(
    EventReasonStats& stats, std::int32_t peak_dbm, std::int32_t delta_db) {
    const bool first = stats.trigger_count == 0U;
    if (stats.trigger_count < std::numeric_limits<std::uint32_t>::max()) {
        ++stats.trigger_count;
    }
    if (first || peak_dbm > stats.max_peak_dbm) {
        stats.max_peak_dbm = peak_dbm;
    }
    if (delta_db > stats.max_delta_db) {
        stats.max_delta_db = delta_db;
    }
}

bool EventDetector::valid_peak(
    const acquisition::PublishedSnapshot& snapshot, bool fresh) const noexcept {
    return fresh && snapshot.payload.payload_status != domain::PayloadStatus::not_refreshed &&
        snapshot.payload.measurements[2U].valid;
}

int EventDetector::find_active(EventType type) const noexcept {
    const std::uint8_t bit = reason_bit(type);
    for (std::size_t index = 0; index < active_.size(); ++index) {
        if (active_[index] && (active_[index]->bundle.reason_mask & bit) != 0U) {
            return static_cast<int>(index);
        }
    }
    return -1;
}

int EventDetector::create_bundle(
    std::uint8_t reason_mask,
    const acquisition::PublishedSnapshot& snapshot,
    std::chrono::steady_clock::time_point observed_at,
    std::chrono::system_clock::time_point observed_at_utc,
    std::int32_t peak_dbm,
    std::int32_t delta_db) {
    std::size_t slot = active_.size();
    for (std::size_t index = 0; index < active_.size(); ++index) {
        if (!active_[index]) {
            slot = index;
            break;
        }
    }
    if (slot == active_.size()) {
        return -1;
    }

    ActiveBundle active;
    active.bundle.id = next_bundle_id_++;
    active.bundle.first_triggered_at = observed_at;
    active.bundle.first_triggered_at_utc = observed_at_utc;
    active.bundle.reason_mask = reason_mask;
    active.bundle.frames.reserve(kHistoryFrameCount + 1U + kPostFrameCount);
    for (const acquisition::PublishedSnapshot& frame : history_) {
        active.bundle.frames.push_back(frame);
    }
    active.bundle.frames.push_back(snapshot);
    if ((reason_mask & kStrongReason) != 0U) {
        active.bundle.strong = EventReasonStats{1U, peak_dbm, 0};
    }
    if ((reason_mask & kSuddenReason) != 0U) {
        active.bundle.sudden = EventReasonStats{1U, peak_dbm, delta_db};
    }
    active_[slot] = std::move(active);
    return static_cast<int>(slot);
}

void EventDetector::attach_reason(
    ActiveBundle& active,
    EventType type,
    std::int32_t peak_dbm,
    std::int32_t delta_db) {
    const std::uint8_t bit = reason_bit(type);
    active.bundle.reason_mask = static_cast<std::uint8_t>(active.bundle.reason_mask | bit);
    if (type == EventType::strong_discharge) {
        update_stats(active.bundle.strong, peak_dbm, 0);
    } else {
        update_stats(active.bundle.sudden, peak_dbm, delta_db);
    }
}

void EventDetector::append_post_frame(const acquisition::PublishedSnapshot& snapshot) {
    for (std::optional<ActiveBundle>& active : active_) {
        if (!active || active->collection_closed ||
            has_generation(active->bundle.frames, snapshot.generation)) {
            continue;
        }
        active->bundle.frames.push_back(snapshot);
        ++active->post_frames;
        if (active->post_frames >= kPostFrameCount) {
            active->collection_closed = true;
        }
    }
}

void EventDetector::enqueue(EventBundle bundle) {
    const std::size_t limit = options_.max_pending_bundles;
    if (limit == 0U) {
        ++dropped_bundle_count_;
        return;
    }
    if (completed_.size() >= limit) {
        completed_.pop_front();
        ++dropped_bundle_count_;
    }
    completed_.push_back(std::move(bundle));
}

void EventDetector::expire(std::chrono::steady_clock::time_point now) {
    for (std::optional<ActiveBundle>& active : active_) {
        if (!active) {
            continue;
        }
        const auto first = active->bundle.first_triggered_at;
        if (!active->collection_closed &&
            now >= first + options_.post_collection_timeout) {
            active->collection_closed = true;
            active->bundle.partial = active->post_frames < kPostFrameCount;
        }
        if (now >= first + options_.merge_window) {
            enqueue(std::move(active->bundle));
            active.reset();
        }
    }
}

void EventDetector::observe(
    const acquisition::PublishedSnapshot& snapshot,
    std::chrono::steady_clock::time_point observed_at,
    bool fresh) {
    observe(snapshot, observed_at, fresh, std::chrono::system_clock::now());
}

void EventDetector::observe(
    const acquisition::PublishedSnapshot& snapshot,
    std::chrono::steady_clock::time_point observed_at,
    bool fresh,
    std::chrono::system_clock::time_point observed_at_utc) {
    expire(observed_at);
    if (snapshot.generation == 0U ||
        (last_generation_ && snapshot.generation <= *last_generation_)) {
        return;
    }
    append_post_frame(snapshot);

    std::int32_t peak_dbm = 0;
    std::int32_t delta_db = 0;
    std::uint8_t trigger_mask = 0U;
    if (valid_peak(snapshot, fresh)) {
        peak_dbm = snapshot.payload.measurements[2U].value;
        if (!strong_armed_ && peak_dbm < options_.strong_rearm_dbm) {
            strong_armed_ = true;
        }
        if (strong_armed_ && peak_dbm >= options_.strong_trigger_dbm) {
            trigger_mask = static_cast<std::uint8_t>(trigger_mask | kStrongReason);
            strong_armed_ = false;
        }
        if (previous_valid_peak_) {
            const std::int64_t difference =
                static_cast<std::int64_t>(peak_dbm) - static_cast<std::int64_t>(*previous_valid_peak_);
            const std::int64_t absolute_difference = difference < 0 ? -difference : difference;
            if (absolute_difference >= static_cast<std::int64_t>(options_.sudden_delta_db)) {
                trigger_mask = static_cast<std::uint8_t>(trigger_mask | kSuddenReason);
                delta_db = static_cast<std::int32_t>(absolute_difference);
            }
        }
        previous_valid_peak_ = peak_dbm;
    }

    if ((trigger_mask & kStrongReason) != 0U &&
        (trigger_mask & kSuddenReason) != 0U) {
        const int strong_slot = find_active(EventType::strong_discharge);
        const int sudden_slot = find_active(EventType::sudden_change);
        if (strong_slot >= 0 && sudden_slot >= 0 && strong_slot != sudden_slot) {
            attach_reason(*active_[static_cast<std::size_t>(strong_slot)],
                EventType::strong_discharge, peak_dbm, delta_db);
            attach_reason(*active_[static_cast<std::size_t>(sudden_slot)],
                EventType::sudden_change, peak_dbm, delta_db);
        } else if (strong_slot >= 0) {
            attach_reason(*active_[static_cast<std::size_t>(strong_slot)],
                EventType::strong_discharge, peak_dbm, 0);
            attach_reason(*active_[static_cast<std::size_t>(strong_slot)],
                EventType::sudden_change, peak_dbm, delta_db);
        } else if (sudden_slot >= 0) {
            attach_reason(*active_[static_cast<std::size_t>(sudden_slot)],
                EventType::sudden_change, peak_dbm, delta_db);
            attach_reason(*active_[static_cast<std::size_t>(sudden_slot)],
                EventType::strong_discharge, peak_dbm, 0);
        } else {
            create_bundle(
                trigger_mask, snapshot, observed_at, observed_at_utc, peak_dbm, delta_db);
        }
    } else if ((trigger_mask & kStrongReason) != 0U) {
        const int slot = find_active(EventType::strong_discharge);
        if (slot >= 0) {
            attach_reason(*active_[static_cast<std::size_t>(slot)],
                EventType::strong_discharge, peak_dbm, 0);
        } else {
            create_bundle(kStrongReason, snapshot, observed_at, observed_at_utc, peak_dbm, 0);
        }
    } else if ((trigger_mask & kSuddenReason) != 0U) {
        const int slot = find_active(EventType::sudden_change);
        if (slot >= 0) {
            attach_reason(*active_[static_cast<std::size_t>(slot)],
                EventType::sudden_change, peak_dbm, delta_db);
        } else {
            create_bundle(kSuddenReason, snapshot, observed_at, observed_at_utc, peak_dbm, delta_db);
        }
    }

    history_.push_back(snapshot);
    while (history_.size() > kHistoryFrameCount) {
        history_.pop_front();
    }
    last_generation_ = snapshot.generation;
}

std::vector<EventBundle> EventDetector::advance(std::chrono::steady_clock::time_point now) {
    expire(now);
    return take_completed();
}

std::vector<EventBundle> EventDetector::take_completed() {
    std::vector<EventBundle> result;
    result.reserve(completed_.size());
    while (!completed_.empty()) {
        result.push_back(std::move(completed_.front()));
        completed_.pop_front();
    }
    return result;
}

bool EventDetector::strong_armed() const noexcept {
    return strong_armed_;
}

std::optional<std::int32_t> EventDetector::previous_valid_peak() const noexcept {
    return previous_valid_peak_;
}

std::size_t EventDetector::dropped_bundle_count() const noexcept {
    return dropped_bundle_count_;
}

}  // namespace uhf::storage
