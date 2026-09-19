// SPDX-License-Identifier: GPL-3.0-only
#include "storage/storage_cleaner.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <sys/statvfs.h>
#include <system_error>
#include <utility>

namespace {

std::uintmax_t multiply_saturated(std::uintmax_t first, std::uintmax_t second) noexcept {
    if (first != 0U && second > std::numeric_limits<std::uintmax_t>::max() / first) {
        return std::numeric_limits<std::uintmax_t>::max();
    }
    return first * second;
}

}  // namespace

namespace uhf::storage {

StorageCleaner::StorageCleaner(
    std::filesystem::path data_root, CleanerOptions options, SpaceProbe space_probe)
    : data_root_(std::move(data_root)), options_(options), space_probe_(std::move(space_probe)) {
    if (!space_probe_) {
        space_probe_ = default_space_probe;
    }
}

bool StorageCleaner::default_space_probe(
    const std::filesystem::path& path, DiskSpace& space) noexcept {
    struct statvfs statistics {};
    if (::statvfs(path.c_str(), &statistics) != 0) {
        return false;
    }
    const std::uintmax_t block_size = static_cast<std::uintmax_t>(statistics.f_frsize);
    space.total_bytes = multiply_saturated(
        static_cast<std::uintmax_t>(statistics.f_blocks), block_size);
    space.available_bytes = multiply_saturated(
        static_cast<std::uintmax_t>(statistics.f_bavail), block_size);
    return true;
}

std::optional<std::chrono::system_clock::time_point> StorageCleaner::filename_timestamp(
    const std::filesystem::path& path) {
    const std::string name = path.filename().string();
    std::size_t number_start = 0U;
    if (name.rfind("frame-", 0U) == 0U) {
        number_start = 6U;
    } else if (name.rfind("event-", 0U) == 0U) {
        number_start = 6U;
    } else if (name.rfind("v3-", 0U) == 0U) {
        number_start = 3U;
    } else {
        return std::nullopt;
    }
    const std::size_t number_end = name.find('-', number_start);
    if (number_end == std::string::npos || number_end == number_start) {
        return std::nullopt;
    }
    std::uint64_t milliseconds = 0U;
    const auto parse_result = std::from_chars(
        name.data() + number_start, name.data() + number_end, milliseconds);
    if (parse_result.ec != std::errc{} || parse_result.ptr != name.data() + number_end ||
        milliseconds > static_cast<std::uint64_t>(
                           std::numeric_limits<std::chrono::milliseconds::rep>::max())) {
        return std::nullopt;
    }
    return std::chrono::system_clock::time_point(std::chrono::milliseconds(milliseconds));
}

bool StorageCleaner::scan_candidates(std::vector<Candidate>& candidates) const {
    candidates.clear();
    constexpr std::array<std::string_view, 3U> directories = {"frames", "events", "v3"};
    for (const std::string_view directory_name : directories) {
        const std::filesystem::path directory = data_root_ / directory_name;
        std::error_code error;
        if (!std::filesystem::exists(directory, error)) {
            if (error) {
                return false;
            }
            continue;
        }
        if (!std::filesystem::is_directory(directory, error) || error) {
            return false;
        }
        for (const std::filesystem::directory_entry& entry :
             std::filesystem::directory_iterator(directory, error)) {
            if (error) {
                return false;
            }
            if (entry.path().extension() != ".bin") {
                continue;
            }
            if (!entry.is_regular_file(error)) {
                if (error) {
                    return false;
                }
                continue;
            }
            const std::optional<std::chrono::system_clock::time_point> timestamp =
                filename_timestamp(entry.path());
            if (!timestamp) {
                continue;
            }
            candidates.push_back(Candidate{
                entry.path(), *timestamp, directory_name == "frames" || directory_name == "v3"});
        }
    }
    std::sort(candidates.begin(), candidates.end(), [](const Candidate& first, const Candidate& second) {
        if (first.timestamp != second.timestamp) {
            return first.timestamp < second.timestamp;
        }
        return first.path.generic_string() < second.path.generic_string();
    });
    return true;
}

std::uintmax_t StorageCleaner::percent_of(
    std::uintmax_t total_bytes, std::uint8_t percent) noexcept {
    const std::uint8_t bounded_percent = percent > 100U ? 100U : percent;
    const std::uintmax_t whole = total_bytes / 100U;
    const std::uintmax_t remainder = total_bytes % 100U;
    return whole * bounded_percent + remainder * bounded_percent / 100U;
}

std::uintmax_t StorageCleaner::saturating_add(
    std::uintmax_t first, std::uintmax_t second) noexcept {
    if (second > std::numeric_limits<std::uintmax_t>::max() - first) {
        return std::numeric_limits<std::uintmax_t>::max();
    }
    return first + second;
}

bool StorageCleaner::query_space(DiskSpace& space) const {
    try {
        return space_probe_(data_root_, space);
    } catch (...) {
        return false;
    }
}

CleanupResult StorageCleaner::run(std::chrono::system_clock::time_point now) {
    CleanupResult result;
    DiskSpace space;
    if (!query_space(space)) {
        writes_paused_ = true;
        result.writes_paused = true;
        result.deletion_failed = true;
        return result;
    }

    result.available_bytes = space.available_bytes;
    result.low_threshold_bytes = std::max(
        options_.min_free_bytes, percent_of(space.total_bytes, options_.low_watermark_percent));
    result.recovery_threshold_bytes = std::max(
        saturating_add(result.low_threshold_bytes, options_.recovery_extra_bytes),
        percent_of(space.total_bytes, options_.recovery_percent));

    if (writes_paused_ && space.available_bytes >= result.recovery_threshold_bytes) {
        writes_paused_ = false;
        result.low_watermark_active = false;
        result.writes_paused = false;
        return result;
    }
    if (!writes_paused_ && space.available_bytes >= result.low_threshold_bytes) {
        result.low_watermark_active = false;
        result.writes_paused = false;
        return result;
    }

    std::vector<Candidate> candidates;
    if (!scan_candidates(candidates)) {
        writes_paused_ = true;
        result.low_watermark_active = true;
        result.writes_paused = true;
        result.deletion_failed = true;
        return result;
    }

    std::optional<std::filesystem::path> newest_frame;
    std::optional<std::chrono::system_clock::time_point> newest_frame_timestamp;
    for (const Candidate& candidate : candidates) {
        if (candidate.frame &&
            (!newest_frame_timestamp || candidate.timestamp > *newest_frame_timestamp)) {
            newest_frame = candidate.path;
            newest_frame_timestamp = candidate.timestamp;
        }
    }

    const std::chrono::system_clock::time_point cutoff = now - options_.retention;
    auto remove_candidate = [&](Candidate& candidate) -> bool {
        if (candidate.path.empty()) {
            return true;
        }
        std::error_code error;
        if (!std::filesystem::remove(candidate.path, error) || error) {
            result.deletion_failed = true;
            return false;
        }
        candidate.path.clear();
        ++result.removed_files;
        if (!query_space(space)) {
            result.deletion_failed = true;
            return false;
        }
        result.available_bytes = space.available_bytes;
        return true;
    };

    for (Candidate& candidate : candidates) {
        if (space.available_bytes >= result.recovery_threshold_bytes) {
            break;
        }
        if (candidate.timestamp < cutoff &&
            (!newest_frame || candidate.path != *newest_frame)) {
            if (!remove_candidate(candidate)) {
                break;
            }
        }
    }
    if (!result.deletion_failed && space.available_bytes < result.recovery_threshold_bytes) {
        for (Candidate& candidate : candidates) {
            if (space.available_bytes >= result.recovery_threshold_bytes) {
                break;
            }
            if (!candidate.path.empty() && (!newest_frame || candidate.path != *newest_frame)) {
                if (!remove_candidate(candidate)) {
                    break;
                }
            }
        }
    }

    if (result.deletion_failed || space.available_bytes < result.recovery_threshold_bytes) {
        writes_paused_ = true;
    } else {
        writes_paused_ = false;
    }
    result.low_watermark_active = space.available_bytes < result.low_threshold_bytes;
    result.writes_paused = writes_paused_;
    return result;
}

void StorageCleaner::update_options(CleanerOptions options) {
    options_ = options;
}

bool StorageCleaner::accepting_writes() const noexcept {
    return !writes_paused_;
}

}  // namespace uhf::storage
