// SPDX-License-Identifier: GPL-3.0-only
#include "v3/packet_trace.hpp"

#include <limits>
#include <stdexcept>
#include <utility>

namespace uhf::v3 {

PacketTraceBuffer::PacketTraceBuffer(std::size_t max_entries, std::size_t max_bytes)
    : max_entries_(max_entries), max_bytes_(max_bytes) {
    if (max_entries_ == 0U || max_bytes_ == 0U) {
        throw std::invalid_argument("packet trace limits must be positive");
    }
}

void PacketTraceBuffer::record(
    PacketSource source, PacketDirection direction,
    const std::uint8_t* data, std::size_t size,
    std::chrono::system_clock::time_point timestamp) {
    if (data == nullptr || size == 0U) {
        return;
    }
    if (size > max_bytes_) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (dropped_entries_ < std::numeric_limits<std::uint64_t>::max()) {
            ++dropped_entries_;
        }
        return;
    }

    PacketTraceEntry entry;
    entry.timestamp = timestamp;
    entry.source = source;
    entry.direction = direction;
    entry.bytes.assign(data, data + size);

    std::lock_guard<std::mutex> lock(mutex_);
    entry.sequence = next_sequence_;
    if (next_sequence_ < std::numeric_limits<std::uint64_t>::max()) {
        ++next_sequence_;
    }
    while (!entries_.empty() &&
           (entries_.size() >= max_entries_ || retained_bytes_ + size > max_bytes_)) {
        retained_bytes_ -= entries_.front().bytes.size();
        entries_.pop_front();
        if (dropped_entries_ < std::numeric_limits<std::uint64_t>::max()) {
            ++dropped_entries_;
        }
    }
    retained_bytes_ += size;
    entries_.push_back(std::move(entry));
}

PacketTraceSnapshot PacketTraceBuffer::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    PacketTraceSnapshot result;
    result.max_entries = max_entries_;
    result.max_bytes = max_bytes_;
    result.retained_bytes = retained_bytes_;
    result.dropped_entries = dropped_entries_;
    result.entries.assign(entries_.begin(), entries_.end());
    return result;
}

const char* packet_source_name(PacketSource source) noexcept {
    switch (source) {
    case PacketSource::pd:
        return "pd";
    case PacketSource::current:
        return "current";
    case PacketSource::temperature:
        return "temperature";
    }
    return "unknown";
}

const char* packet_direction_name(PacketDirection direction) noexcept {
    switch (direction) {
    case PacketDirection::transmit:
        return "tx";
    case PacketDirection::receive:
        return "rx";
    }
    return "unknown";
}

}  // namespace uhf::v3
