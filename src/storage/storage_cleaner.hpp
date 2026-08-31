// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <vector>

namespace uhf::storage {

struct DiskSpace {
    std::uintmax_t total_bytes{0U};
    std::uintmax_t available_bytes{0U};
};

using SpaceProbe = std::function<bool(const std::filesystem::path&, DiskSpace&)>;

struct CleanerOptions {
    std::chrono::seconds retention{std::chrono::hours(24)};
    std::uintmax_t min_free_bytes{512U * 1024U * 1024U};
    std::uint8_t low_watermark_percent{10U};
    std::uint8_t recovery_percent{15U};
    std::uintmax_t recovery_extra_bytes{256U * 1024U * 1024U};
};

struct CleanupResult {
    bool low_watermark_active{false};
    bool writes_paused{false};
    bool deletion_failed{false};
    std::size_t removed_files{0U};
    std::uintmax_t available_bytes{0U};
    std::uintmax_t low_threshold_bytes{0U};
    std::uintmax_t recovery_threshold_bytes{0U};
};

class StorageCleaner {
public:
    StorageCleaner(
        std::filesystem::path data_root,
        CleanerOptions options = {},
        SpaceProbe space_probe = {});

    CleanupResult run(std::chrono::system_clock::time_point now);
    void update_options(CleanerOptions options);
    bool accepting_writes() const noexcept;

private:
    struct Candidate {
        std::filesystem::path path;
        std::chrono::system_clock::time_point timestamp;
        bool frame{false};
    };

    static bool default_space_probe(
        const std::filesystem::path& path, DiskSpace& space) noexcept;
    static std::optional<std::chrono::system_clock::time_point> filename_timestamp(
        const std::filesystem::path& path);
    bool scan_candidates(std::vector<Candidate>& candidates) const;
    static std::uintmax_t percent_of(
        std::uintmax_t total_bytes, std::uint8_t percent) noexcept;
    static std::uintmax_t saturating_add(
        std::uintmax_t first, std::uintmax_t second) noexcept;
    bool query_space(DiskSpace& space) const;

    std::filesystem::path data_root_;
    CleanerOptions options_;
    SpaceProbe space_probe_;
    bool writes_paused_{false};
};

}  // namespace uhf::storage
