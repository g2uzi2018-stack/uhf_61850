// SPDX-License-Identifier: GPL-3.0-only
#include "storage/event_store.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <unistd.h>
#include <vector>

namespace {

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "event store smoke failed: " << message << '\n';
        return false;
    }
    return true;
}

uhf::acquisition::PublishedSnapshot frame(std::uint64_t generation) {
    uhf::acquisition::PublishedSnapshot result;
    result.generation = generation;
    result.payload.raw_registers.fill(0U);
    result.payload.raw_registers[2U] = 0xFFCEU;
    result.payload.raw_registers[100U] = 0xFFCEU;
    result.payload = uhf::domain::parse_pd1000_registers(result.payload.raw_registers);
    return result;
}

uhf::storage::EventBundle bundle() {
    uhf::storage::EventBundle result;
    result.id = 17U;
    result.first_triggered_at = std::chrono::steady_clock::time_point(
        std::chrono::seconds(23));
    result.first_triggered_at_utc = std::chrono::system_clock::time_point(
        std::chrono::milliseconds(1'700'000'123'456LL));
    result.reason_mask = 3U;
    result.partial = true;
    result.strong = uhf::storage::EventReasonStats{2U, -40, 0};
    result.sudden = uhf::storage::EventReasonStats{3U, -40, 15};
    for (std::uint64_t generation = 1U; generation <= 6U; ++generation) {
        result.frames.push_back(frame(generation));
    }
    return result;
}

}  // namespace

int main() {
    const std::filesystem::path root = std::filesystem::temp_directory_path() /
        ("uhf-event-store-" + std::to_string(static_cast<long long>(::getpid())));
    std::error_code cleanup_error;
    std::filesystem::remove_all(root, cleanup_error);
    try {
        uhf::storage::EventBundleStore store(root);
        const uhf::storage::EventBundle original = bundle();
        const std::optional<std::filesystem::path> path = store.save(original);
        if (!expect(path.has_value(), "event write") ||
            !expect(std::filesystem::file_size(*path) > 54U, "event file written")) {
            return 1;
        }
        const std::optional<uhf::storage::EventBundle> restored = store.read(*path);
        if (!expect(restored.has_value(), "event read") || !restored) {
            return 1;
        }
        if (!expect(restored->id == original.id, "event ID round trip") ||
            !expect(restored->first_triggered_at_utc == original.first_triggered_at_utc,
                "event timestamp round trip") ||
            !expect(restored->reason_mask == original.reason_mask, "reason round trip") ||
            !expect(restored->partial, "partial round trip") ||
            !expect(restored->strong.trigger_count == 2U, "strong statistics round trip") ||
            !expect(restored->sudden.max_delta_db == 15, "sudden statistics round trip") ||
            !expect(restored->frames.size() == 6U, "event frame count round trip") ||
            !expect(restored->frames[5U].generation == 6U, "event generation round trip") ||
            !expect(restored->frames[5U].payload.raw_registers[2U] == 0xFFCEU,
                "event raw register round trip") ||
            !expect(restored->frames[5U].payload.measurements[2U].value == -50,
                "event parsed value round trip")) {
            return 1;
        }

        const std::filesystem::path corrupt_path = root / "corrupt.bin";
        std::filesystem::copy_file(*path, corrupt_path);
        std::fstream corrupt(corrupt_path, std::ios::in | std::ios::out | std::ios::binary);
        corrupt.seekp(100);
        const char corrupted = static_cast<char>(0xAA);
        corrupt.write(&corrupted, 1);
        corrupt.close();
        if (!expect(!store.read(corrupt_path).has_value(), "corrupt event rejected")) {
            return 1;
        }

        uhf::storage::EventBundle duplicate = original;
        duplicate.id = 18U;
        duplicate.frames[5U] = frame(1U);
        if (!expect(!store.save(duplicate).has_value(), "duplicate generation rejected")) {
            return 1;
        }

        const std::filesystem::path temporary = root / "event-stale.tmp";
        std::ofstream stale(temporary, std::ios::binary);
        stale << "incomplete";
        stale.close();
        if (!expect(std::filesystem::exists(temporary), "stale fixture") ||
            !expect(uhf::storage::EventBundleStore(root).cleanup_incomplete(),
                "temporary cleanup") ||
            !expect(!std::filesystem::exists(temporary), "temporary removed")) {
            return 1;
        }

        std::filesystem::remove_all(root, cleanup_error);
        std::cout << "event store smoke: OK\n";
        return 0;
    } catch (const std::exception& error) {
        std::filesystem::remove_all(root, cleanup_error);
        std::cerr << "event store smoke failed: " << error.what() << '\n';
        return 1;
    }
}
