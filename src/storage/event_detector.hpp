// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "acquisition/acquisition.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <vector>

namespace uhf::storage {

enum class EventType {
    strong_discharge,
    sudden_change,
};

struct EventReasonStats {
    std::uint32_t trigger_count{0};
    std::int32_t max_peak_dbm{0};
    std::int32_t max_delta_db{0};
};

struct EventBundle {
    std::uint64_t id{0};
    std::chrono::steady_clock::time_point first_triggered_at;
    std::chrono::system_clock::time_point first_triggered_at_utc;
    std::uint8_t reason_mask{0U};
    bool partial{false};
    EventReasonStats strong;
    EventReasonStats sudden;
    std::vector<acquisition::PublishedSnapshot> frames;

    bool has_reason(EventType type) const noexcept;
};

struct EventOptions {
    std::int32_t strong_trigger_dbm{-45};
    std::int32_t strong_rearm_dbm{-50};
    std::int32_t sudden_delta_db{10};
    std::chrono::seconds post_collection_timeout{30};
    std::chrono::seconds merge_window{60};
    std::size_t max_pending_bundles{8U};
};

class EventDetector {
public:
    explicit EventDetector(EventOptions options = {});

    void observe(
        const acquisition::PublishedSnapshot& snapshot,
        std::chrono::steady_clock::time_point observed_at,
        bool fresh);
    void observe(
        const acquisition::PublishedSnapshot& snapshot,
        std::chrono::steady_clock::time_point observed_at,
        bool fresh,
        std::chrono::system_clock::time_point observed_at_utc);
    std::vector<EventBundle> advance(std::chrono::steady_clock::time_point now);
    std::vector<EventBundle> take_completed();

    bool strong_armed() const noexcept;
    std::optional<std::int32_t> previous_valid_peak() const noexcept;
    std::size_t dropped_bundle_count() const noexcept;

private:
    struct ActiveBundle {
        EventBundle bundle;
        std::size_t post_frames{0U};
        bool collection_closed{false};
    };

    static constexpr std::uint8_t kStrongReason = 1U;
    static constexpr std::uint8_t kSuddenReason = 2U;
    static constexpr std::size_t kActiveBundleSlots = 2U;
    static constexpr std::size_t kPostFrameCount = 3U;
    static constexpr std::size_t kHistoryFrameCount = 2U;

    static std::uint8_t reason_bit(EventType type) noexcept;
    static bool has_generation(
        const std::vector<acquisition::PublishedSnapshot>& frames,
        std::uint64_t generation) noexcept;
    static void update_stats(
        EventReasonStats& stats, std::int32_t peak_dbm, std::int32_t delta_db);

    bool valid_peak(const acquisition::PublishedSnapshot& snapshot, bool fresh) const noexcept;
    int find_active(EventType type) const noexcept;
    int create_bundle(
        std::uint8_t reason_mask,
        const acquisition::PublishedSnapshot& snapshot,
        std::chrono::steady_clock::time_point observed_at,
        std::chrono::system_clock::time_point observed_at_utc,
        std::int32_t peak_dbm,
        std::int32_t delta_db);
    void attach_reason(
        ActiveBundle& active,
        EventType type,
        std::int32_t peak_dbm,
        std::int32_t delta_db);
    void append_post_frame(const acquisition::PublishedSnapshot& snapshot);
    void expire(std::chrono::steady_clock::time_point now);
    void enqueue(EventBundle bundle);

    EventOptions options_;
    bool strong_armed_{true};
    std::optional<std::int32_t> previous_valid_peak_;
    std::optional<std::uint64_t> last_generation_;
    std::uint64_t next_bundle_id_{1U};
    std::deque<acquisition::PublishedSnapshot> history_;
    std::array<std::optional<ActiveBundle>, kActiveBundleSlots> active_{};
    std::deque<EventBundle> completed_;
    std::size_t dropped_bundle_count_{0U};
};

}  // namespace uhf::storage
