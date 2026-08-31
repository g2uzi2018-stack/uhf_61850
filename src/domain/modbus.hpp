// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace uhf::domain {

constexpr std::size_t kPd1000RequestCount = 31U;
constexpr std::uint8_t kPd1000SlaveId = 1U;
constexpr std::uint8_t kReadInputRegistersFunction = 0x04U;
constexpr std::uint16_t kPd1000FirstAddress = 10001U;
constexpr std::uint16_t kPd1000LastAddress = 13615U;

struct ModbusReadRequest {
    std::uint8_t slave_id{0};
    std::uint8_t function{0};
    std::uint16_t start_address{0};
    std::uint16_t register_count{0};
    std::array<std::uint8_t, 8U> wire_frame{};
};

std::uint16_t modbus_crc16(const std::uint8_t* data, std::size_t size) noexcept;

std::array<ModbusReadRequest, kPd1000RequestCount> pd1000_request_plan() noexcept;

}  // namespace uhf::domain
