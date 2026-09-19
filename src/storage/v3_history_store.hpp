// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "v3/acquisition.hpp"

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace uhf::storage {

struct V3HistoryRecord {
    std::uint64_t generation{0U};
    std::chrono::system_clock::time_point timestamp;
    v3::UnifiedSnapshot snapshot;
};

/** Durable, bounded-format records for the v3 three-in-one history view. */
class V3HistoryStore {
public:
    explicit V3HistoryStore(std::filesystem::path directory);

    std::optional<std::filesystem::path> save(
        const v3::UnifiedSnapshot& snapshot,
        std::chrono::system_clock::time_point timestamp);
    std::optional<V3HistoryRecord> read(const std::filesystem::path& path) const;
    std::vector<std::filesystem::path> list(std::size_t limit = 100U) const;
    static std::string to_csv(const V3HistoryRecord& record);

private:
    std::filesystem::path directory_;
};

}  // namespace uhf::storage
