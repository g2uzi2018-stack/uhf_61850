// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "storage/event_detector.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace uhf::storage {

constexpr std::uint16_t kEventFormatVersion = 1U;
constexpr std::size_t kMaxEventFrames = 6U;

class EventBundleStore {
public:
    explicit EventBundleStore(std::filesystem::path directory);

    std::optional<std::filesystem::path> save(const EventBundle& bundle) const;
    std::optional<EventBundle> read(const std::filesystem::path& path) const;
    std::vector<std::filesystem::path> list(std::size_t limit = 100U) const;
    static std::string to_csv(const EventBundle& bundle);
    bool cleanup_incomplete() const;

private:
    std::filesystem::path directory_;
};

}  // namespace uhf::storage
