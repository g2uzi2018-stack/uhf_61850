// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "acquisition/acquisition.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace uhf::storage {

constexpr std::uint16_t kFrameFormatVersion = 1U;

struct FrameRecord {
    std::uint64_t generation{0};
    std::chrono::system_clock::time_point timestamp;
    domain::PayloadStatus payload_status{domain::PayloadStatus::degraded};
    std::array<std::uint16_t, domain::kPd1000RegisterCount> raw_registers{};
};

class FrameStore {
public:
    explicit FrameStore(std::filesystem::path directory);

    std::optional<std::filesystem::path> save(
        const acquisition::PublishedSnapshot& snapshot,
        std::chrono::system_clock::time_point timestamp);
    std::optional<FrameRecord> read(const std::filesystem::path& path) const;
    std::vector<std::filesystem::path> list(std::size_t limit = 100U) const;
    static std::string to_csv(const FrameRecord& record);
    bool save_periodic(
        const acquisition::PublishedSnapshot& snapshot,
        std::chrono::system_clock::time_point now,
        std::chrono::seconds period = std::chrono::seconds(300));
    bool cleanup_expired(
        std::chrono::system_clock::time_point now, std::chrono::seconds retention);

private:
    std::filesystem::path directory_;
    std::optional<std::chrono::system_clock::time_point> last_periodic_save_;
    std::uint64_t last_periodic_generation_{0};
};

}  // namespace uhf::storage
