// SPDX-License-Identifier: GPL-3.0-only
#include "storage/frame_store.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <unistd.h>

namespace {

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "frame store smoke failed: " << message << '\n';
        return false;
    }
    return true;
}

uhf::acquisition::PublishedSnapshot snapshot(std::uint64_t generation) {
    uhf::acquisition::PublishedSnapshot result;
    result.generation = generation;
    result.payload.raw_registers.fill(0x1357U);
    result.payload.raw_registers[0] = 0xFFCEU;
    result.payload.payload_status = uhf::domain::PayloadStatus::good;
    return result;
}

}  // namespace

int main() {
    const std::filesystem::path root = std::filesystem::temp_directory_path() /
        ("uhf-frame-store-" + std::to_string(static_cast<long long>(::getpid())));
    std::error_code cleanup_error;
    std::filesystem::remove_all(root, cleanup_error);
    try {
        uhf::storage::FrameStore store(root);
        const auto now = std::chrono::system_clock::time_point(
            std::chrono::milliseconds(1'700'000'123'456LL));
        const auto first = snapshot(1U);
        const auto first_path = store.save(first, now);
        if (!expect(first_path.has_value(), "initial frame write") ||
            !expect(std::filesystem::file_size(*first_path) ==
                        4U + 2U + 8U + 8U + 1U + 1U + 2U +
                            uhf::domain::kPd1000RegisterCount * 2U + 4U,
                "fixed frame size")) {
            return 1;
        }
        const auto first_record = store.read(*first_path);
        if (!expect(first_record.has_value(), "initial frame read") ||
            !expect(first_record->generation == 1U, "generation round trip") ||
            !expect(first_record->timestamp == now, "timestamp round trip") ||
            !expect(first_record->payload_status == uhf::domain::PayloadStatus::good,
                "payload status round trip") ||
            !expect(first_record->raw_registers[0] == 0xFFCEU, "raw register round trip")) {
            return 1;
        }
        const std::vector<std::filesystem::path> listed = store.list();
        if (!expect(listed.size() == 1U, "frame listing") ||
            !expect(uhf::storage::FrameStore::to_csv(*first_record).find("13615") != std::string::npos,
                "frame CSV export")) {
            return 1;
        }

        const std::filesystem::path corrupt_path = root / "corrupt.bin";
        std::filesystem::copy_file(*first_path, corrupt_path);
        std::fstream corrupt(corrupt_path, std::ios::in | std::ios::out | std::ios::binary);
        corrupt.seekp(32);
        const char corrupted = static_cast<char>(0xAA);
        corrupt.write(&corrupted, 1);
        corrupt.close();
        if (!expect(!store.read(corrupt_path).has_value(), "corrupt frame rejected")) {
            return 1;
        }

        uhf::storage::FrameStore periodic_store(root / "periodic");
        if (!expect(periodic_store.save_periodic(first, now), "first periodic save") ||
            !expect(!periodic_store.save_periodic(
                        first, now + std::chrono::seconds(299)),
                "duplicate generation not saved early") ||
            !expect(periodic_store.save_periodic(
                        snapshot(2U), now + std::chrono::seconds(300)),
                "second periodic save")) {
            return 1;
        }

        const auto old_path = store.save(
            snapshot(3U), now - std::chrono::hours(48));
        const auto newest_path = store.save(snapshot(4U), now);
        if (!expect(old_path.has_value() && newest_path.has_value(), "retention fixtures") ||
            !expect(store.cleanup_expired(now, std::chrono::hours(24)), "retention cleanup") ||
            !expect(!std::filesystem::exists(*old_path), "expired frame removed") ||
            !expect(std::filesystem::exists(*newest_path), "newest frame preserved")) {
            return 1;
        }
        std::filesystem::remove_all(root, cleanup_error);
        std::cout << "frame store smoke: OK\n";
        return 0;
    } catch (const std::exception& error) {
        std::filesystem::remove_all(root, cleanup_error);
        std::cerr << "frame store smoke failed: " << error.what() << '\n';
        return 1;
    }
}
