// SPDX-License-Identifier: GPL-3.0-only
#include "storage/storage_cleaner.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>
#include <unistd.h>

namespace {

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "storage cleaner smoke failed: " << message << '\n';
        return false;
    }
    return true;
}

std::filesystem::path data_file(
    const std::filesystem::path& directory,
    const char* prefix,
    std::int64_t milliseconds,
    std::uint64_t sequence) {
    return directory / (std::string(prefix) + "-" + std::to_string(milliseconds) + "-" +
                        std::to_string(sequence) + ".bin");
}

bool create_file(const std::filesystem::path& path) {
    std::ofstream output(path, std::ios::binary);
    output << "fixture";
    return static_cast<bool>(output);
}

}  // namespace

int main() {
    const std::filesystem::path root = std::filesystem::temp_directory_path() /
        ("uhf-storage-cleaner-" + std::to_string(static_cast<long long>(::getpid())));
    std::error_code cleanup_error;
    std::filesystem::remove_all(root, cleanup_error);
    try {
        const std::filesystem::path frames = root / "frames";
        const std::filesystem::path events = root / "events";
        std::filesystem::create_directories(frames);
        std::filesystem::create_directories(events);
        const auto now = std::chrono::system_clock::time_point(
            std::chrono::milliseconds(1'700'000'000'000LL));
        const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()).count();
        const std::filesystem::path old_frame = data_file(
            frames, "frame", now_ms - 48LL * 60LL * 60LL * 1000LL, 1U);
        const std::filesystem::path old_event = data_file(
            events, "event", now_ms - 48LL * 60LL * 60LL * 1000LL, 2U);
        const std::filesystem::path recent_event = data_file(
            events, "event", now_ms - 60LL * 60LL * 1000LL, 3U);
        const std::filesystem::path newest_frame = data_file(frames, "frame", now_ms, 4U);
        const std::filesystem::path temporary = frames / "frame-in-progress.tmp";
        if (!expect(create_file(old_frame), "old frame fixture") ||
            !expect(create_file(old_event), "old event fixture") ||
            !expect(create_file(recent_event), "recent event fixture") ||
            !expect(create_file(newest_frame), "newest frame fixture") ||
            !expect(create_file(temporary), "temporary fixture")) {
            return 1;
        }

        uhf::storage::CleanerOptions options;
        options.retention = std::chrono::hours(24);
        options.min_free_bytes = 150U;
        options.low_watermark_percent = 10U;
        options.recovery_percent = 15U;
        options.recovery_extra_bytes = 100U;
        std::vector<std::uintmax_t> readings = {100U, 200U, 300U};
        std::size_t calls = 0U;
        const uhf::storage::SpaceProbe probe =
            [&readings, &calls](const std::filesystem::path&, uhf::storage::DiskSpace& space) {
                const std::size_t index = calls < readings.size() ? calls : readings.size() - 1U;
                ++calls;
                space.total_bytes = 1000U;
                space.available_bytes = readings[index];
                return true;
            };
        uhf::storage::StorageCleaner cleaner(root, options, probe);
        const uhf::storage::CleanupResult result = cleaner.run(now);
        if (!expect(result.low_threshold_bytes == 150U, "low threshold") ||
            !expect(result.recovery_threshold_bytes == 250U, "recovery threshold") ||
            !expect(result.removed_files == 2U, "expired files removed first") ||
            !expect(!result.writes_paused, "writes remain enabled after recovery") ||
            !expect(!std::filesystem::exists(old_frame), "old frame removed") ||
            !expect(!std::filesystem::exists(old_event), "old event removed") ||
            !expect(std::filesystem::exists(recent_event), "recent event preserved") ||
            !expect(std::filesystem::exists(newest_frame), "newest frame protected") ||
            !expect(std::filesystem::exists(temporary), "temporary file protected")) {
            return 1;
        }

        uhf::storage::CleanerOptions updated_options = options;
        updated_options.min_free_bytes = 350U;
        cleaner.update_options(updated_options);
        const uhf::storage::CleanupResult updated = cleaner.run(now);
        if (!expect(updated.low_threshold_bytes == 350U, "updated low threshold") ||
            !expect(updated.writes_paused, "updated cleaner options were not applied")) {
            return 1;
        }

        const std::filesystem::path second_root = root / "second";
        std::filesystem::create_directories(second_root / "frames");
        std::vector<std::uintmax_t> second_readings = {100U, 300U};
        std::size_t second_calls = 0U;
        const uhf::storage::SpaceProbe second_probe =
            [&second_readings, &second_calls](const std::filesystem::path&,
                                               uhf::storage::DiskSpace& space) {
                const std::size_t index = second_calls < second_readings.size()
                    ? second_calls
                    : second_readings.size() - 1U;
                ++second_calls;
                space.total_bytes = 1000U;
                space.available_bytes = second_readings[index];
                return true;
            };
        uhf::storage::StorageCleaner paused_cleaner(second_root, options, second_probe);
        const uhf::storage::CleanupResult paused = paused_cleaner.run(now);
        if (!expect(paused.writes_paused, "low disk pauses writes") ||
            !expect(!paused_cleaner.accepting_writes(), "paused cleaner rejects writes")) {
            return 1;
        }
        const uhf::storage::CleanupResult recovered = paused_cleaner.run(now);
        if (!expect(!recovered.writes_paused, "recovery hysteresis resumes writes") ||
            !expect(paused_cleaner.accepting_writes(), "recovered cleaner accepts writes")) {
            return 1;
        }

        uhf::storage::StorageCleaner failed_cleaner(
            root,
            options,
            [](const std::filesystem::path&, uhf::storage::DiskSpace&) { return false; });
        const uhf::storage::CleanupResult failed = failed_cleaner.run(now);
        if (!expect(failed.writes_paused, "space probe failure pauses writes") ||
            !expect(failed.deletion_failed, "space probe failure is observable")) {
            return 1;
        }

        std::filesystem::remove_all(root, cleanup_error);
        std::cout << "storage cleaner smoke: OK\n";
        return 0;
    } catch (const std::exception& error) {
        std::filesystem::remove_all(root, cleanup_error);
        std::cerr << "storage cleaner smoke failed: " << error.what() << '\n';
        return 1;
    }
}
