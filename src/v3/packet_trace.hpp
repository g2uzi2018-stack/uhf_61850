// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <vector>

namespace uhf::v3 {

enum class PacketSource { pd, current, temperature };
enum class PacketDirection { transmit, receive };

struct PacketTraceEntry {
    std::uint64_t sequence{0U};
    std::chrono::system_clock::time_point timestamp{};
    PacketSource source{PacketSource::pd};
    PacketDirection direction{PacketDirection::transmit};
    std::vector<std::uint8_t> bytes;
};

struct PacketTraceSnapshot {
    std::size_t max_entries{0U};
    std::size_t max_bytes{0U};
    std::size_t retained_bytes{0U};
    std::uint64_t dropped_entries{0U};
    std::vector<PacketTraceEntry> entries;
};

class PacketTraceBuffer {
public:
    explicit PacketTraceBuffer(std::size_t max_entries = 256U,
                               std::size_t max_bytes = 64U * 1024U);

    void record(PacketSource source, PacketDirection direction,
                const std::uint8_t* data, std::size_t size,
                std::chrono::system_clock::time_point timestamp =
                    std::chrono::system_clock::now());
    PacketTraceSnapshot snapshot() const;

private:
    mutable std::mutex mutex_;
    std::deque<PacketTraceEntry> entries_;
    std::size_t max_entries_;
    std::size_t max_bytes_;
    std::size_t retained_bytes_{0U};
    std::uint64_t next_sequence_{1U};
    std::uint64_t dropped_entries_{0U};
};

const char* packet_source_name(PacketSource source) noexcept;
const char* packet_direction_name(PacketDirection direction) noexcept;

}  // namespace uhf::v3
