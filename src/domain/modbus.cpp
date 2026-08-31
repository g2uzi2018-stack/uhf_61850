// SPDX-License-Identifier: GPL-3.0-only
#include "domain/modbus.hpp"

namespace uhf::domain {

std::uint16_t modbus_crc16(const std::uint8_t* data, std::size_t size) noexcept {
    std::uint16_t crc = 0xFFFFU;
    for (std::size_t index = 0; index < size; ++index) {
        crc = static_cast<std::uint16_t>(crc ^ static_cast<std::uint16_t>(data[index]));
        for (std::size_t bit = 0; bit < 8U; ++bit) {
            if ((crc & 0x0001U) != 0U) {
                crc = static_cast<std::uint16_t>((crc >> 1U) ^ 0xA001U);
            } else {
                crc = static_cast<std::uint16_t>(crc >> 1U);
            }
        }
    }
    return crc;
}

std::array<ModbusReadRequest, kPd1000RequestCount> pd1000_request_plan(
    std::uint8_t slave_id) noexcept {
    std::array<ModbusReadRequest, kPd1000RequestCount> plan{};
    for (std::size_t index = 0; index < plan.size(); ++index) {
        ModbusReadRequest& request = plan[index];
        const std::uint32_t start_address = index == 0U
            ? static_cast<std::uint32_t>(kPd1000FirstAddress)
            : 10016U + static_cast<std::uint32_t>((index - 1U) * 120U);
        const std::uint16_t register_count = index == 0U ? 15U : 120U;
        request.slave_id = slave_id;
        request.function = kReadInputRegistersFunction;
        request.start_address = static_cast<std::uint16_t>(start_address);
        request.register_count = register_count;
        request.wire_frame[0] = request.slave_id;
        request.wire_frame[1] = request.function;
        request.wire_frame[2] = static_cast<std::uint8_t>(request.start_address >> 8U);
        request.wire_frame[3] = static_cast<std::uint8_t>(request.start_address & 0x00FFU);
        request.wire_frame[4] = static_cast<std::uint8_t>(register_count >> 8U);
        request.wire_frame[5] = static_cast<std::uint8_t>(register_count & 0x00FFU);
        const std::uint16_t crc = modbus_crc16(request.wire_frame.data(), 6U);
        request.wire_frame[6] = static_cast<std::uint8_t>(crc & 0x00FFU);
        request.wire_frame[7] = static_cast<std::uint8_t>(crc >> 8U);
    }
    return plan;
}

}  // namespace uhf::domain
