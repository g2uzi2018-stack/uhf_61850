// SPDX-License-Identifier: GPL-3.0-only
#include "acquisition/loopback_pd1000.hpp"

#include "domain/modbus.hpp"

#include <algorithm>
#include <cstdint>
#include <vector>

namespace uhf::acquisition {

LoopbackPd1000Port::LoopbackPd1000Port() {
    registers_.fill(0U);
    registers_[0U] = static_cast<std::uint16_t>(-55);
    registers_[1U] = 12U;
    registers_[2U] = static_cast<std::uint16_t>(-50);
    registers_[3U] = 180U;
    registers_[4U] = 240U;
    for (std::size_t index = 0; index < domain::kPd1000SpectrumPointCount; ++index) {
        const std::int32_t value = -60 + static_cast<std::int32_t>(index % 6U);
        registers_[domain::kPd1000SpectrumOffset + index] = static_cast<std::uint16_t>(value);
    }
}

bool LoopbackPd1000Port::write_all(const std::uint8_t* data, std::size_t size) {
    if (data == nullptr || size != 8U) {
        return false;
    }
    const auto plan = domain::pd1000_request_plan();
    const domain::ModbusReadRequest& request = plan[next_request_index_];
    if (!std::equal(request.wire_frame.begin(), request.wire_frame.end(), data)) {
        return false;
    }

    std::vector<std::uint8_t> response;
    response.reserve(3U + static_cast<std::size_t>(request.register_count) * 2U + 2U);
    response.push_back(request.slave_id);
    response.push_back(request.function);
    response.push_back(static_cast<std::uint8_t>(request.register_count * 2U));
    const std::size_t start_index = static_cast<std::size_t>(
        request.start_address - domain::kPd1000FirstAddress);
    for (std::size_t index = 0; index < request.register_count; ++index) {
        const std::uint16_t value = registers_[start_index + index];
        response.push_back(static_cast<std::uint8_t>(value >> 8U));
        response.push_back(static_cast<std::uint8_t>(value & 0x00FFU));
    }
    const std::uint16_t checksum = domain::modbus_crc16(response.data(), response.size());
    response.push_back(static_cast<std::uint8_t>(checksum & 0x00FFU));
    response.push_back(static_cast<std::uint8_t>(checksum >> 8U));

    {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_response_.insert(pending_response_.end(), response.begin(), response.end());
        next_request_index_ = (next_request_index_ + 1U) % plan.size();
    }
    response_ready_.notify_one();
    return true;
}

bool LoopbackPd1000Port::read_some(
    std::uint8_t* data,
    std::size_t capacity,
    std::chrono::milliseconds timeout,
    std::size_t& received) {
    received = 0U;
    if (capacity == 0U) {
        return true;
    }
    if (data == nullptr) {
        return false;
    }
    std::unique_lock<std::mutex> lock(mutex_);
    if (pending_response_.empty() && timeout.count() > 0) {
        response_ready_.wait_for(lock, timeout, [this] { return !pending_response_.empty(); });
    }
    const std::size_t count = std::min(capacity, pending_response_.size());
    for (std::size_t index = 0; index < count; ++index) {
        data[index] = pending_response_.front();
        pending_response_.pop_front();
    }
    received = count;
    return true;
}

}  // namespace uhf::acquisition
