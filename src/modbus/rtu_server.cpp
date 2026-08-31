// SPDX-License-Identifier: GPL-3.0-only
#include "modbus/rtu_server.hpp"

#include "domain/modbus.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace {

constexpr std::size_t kReadRequestBytes = 8U;
constexpr std::size_t kReadResponseHeaderBytes = 3U;
constexpr std::size_t kCrcBytes = 2U;
constexpr std::uint8_t kReadInputRegisters = 0x04U;
constexpr std::uint8_t kIllegalFunction = 0x01U;
constexpr std::uint8_t kIllegalDataAddress = 0x02U;
constexpr std::uint8_t kIllegalDataValue = 0x03U;
constexpr std::uint8_t kServerDeviceFailure = 0x04U;

std::uint16_t read_u16(const std::uint8_t* bytes) noexcept {
    return static_cast<std::uint16_t>(
        static_cast<std::uint16_t>(bytes[0]) << 8U | static_cast<std::uint16_t>(bytes[1]));
}

void append_u16(std::vector<std::uint8_t>& bytes, std::uint16_t value) {
    bytes.push_back(static_cast<std::uint8_t>(value >> 8U));
    bytes.push_back(static_cast<std::uint8_t>(value & 0x00FFU));
}

void append_crc(std::vector<std::uint8_t>& bytes) {
    const std::uint16_t crc = uhf::domain::modbus_crc16(bytes.data(), bytes.size());
    bytes.push_back(static_cast<std::uint8_t>(crc & 0x00FFU));
    bytes.push_back(static_cast<std::uint8_t>(crc >> 8U));
}

std::vector<std::uint8_t> exception_response(
    std::uint8_t unit_id, std::uint8_t function, std::uint8_t exception) {
    std::vector<std::uint8_t> response{unit_id, static_cast<std::uint8_t>(function | 0x80U), exception};
    append_crc(response);
    return response;
}

}  // namespace

namespace uhf::modbus {

ModbusRtuServer::ModbusRtuServer(
    acquisition::ISerialPort& serial_port,
    acquisition::SnapshotStore& snapshot_store,
    ModbusRtuOptions options)
    : serial_port_(serial_port), snapshot_store_(snapshot_store), options_(options) {
    if (options_.unit_id == 0U || options_.unit_id > 247U || options_.max_frame_bytes < 8U) {
        throw std::invalid_argument("invalid Modbus RTU options");
    }
}

std::vector<std::uint8_t> ModbusRtuServer::handle_request(
    const std::vector<std::uint8_t>& request) const {
    if (request.size() < kReadRequestBytes || request.size() > options_.max_frame_bytes ||
        uhf::domain::modbus_crc16(request.data(), request.size() - kCrcBytes) !=
            static_cast<std::uint16_t>(
                request[request.size() - 2U] |
                static_cast<std::uint16_t>(request[request.size() - 1U]) << 8U)) {
        return {};
    }
    if (request[0] != options_.unit_id) {
        return {};
    }
    if (request.size() != kReadRequestBytes) {
        return exception_response(options_.unit_id, request[1], kIllegalDataValue);
    }

    const std::uint8_t function = request[1];
    if (function != kReadInputRegisters) {
        return exception_response(options_.unit_id, function, kIllegalFunction);
    }
    const std::uint16_t start_address = read_u16(request.data() + 2U);
    const std::uint16_t register_count = read_u16(request.data() + 4U);
    if (register_count == 0U || register_count > 125U) {
        return exception_response(options_.unit_id, function, kIllegalDataValue);
    }
    const std::uint32_t last_address =
        static_cast<std::uint32_t>(start_address) + register_count - 1U;
    if (start_address < domain::kPd1000FirstAddress ||
        last_address > domain::kPd1000LastAddress) {
        return exception_response(options_.unit_id, function, kIllegalDataAddress);
    }

    const std::optional<acquisition::PublishedSnapshot> latest = snapshot_store_.latest();
    if (!latest) {
        return exception_response(options_.unit_id, function, kServerDeviceFailure);
    }

    const std::size_t offset = static_cast<std::size_t>(
        start_address - domain::kPd1000FirstAddress);
    const std::size_t count = static_cast<std::size_t>(register_count);
    std::vector<std::uint8_t> response;
    response.reserve(kReadResponseHeaderBytes + count * 2U + kCrcBytes);
    response.push_back(options_.unit_id);
    response.push_back(function);
    response.push_back(static_cast<std::uint8_t>(count * 2U));
    for (std::size_t index = 0; index < count; ++index) {
        append_u16(response, latest->payload.raw_registers[offset + index]);
    }
    append_crc(response);
    return response;
}

int ModbusRtuServer::run() {
    std::vector<std::uint8_t> input;
    input.reserve(options_.max_frame_bytes);
    std::uint8_t buffer[256];
    while (!stop_requested_.load()) {
        std::size_t received = 0;
        if (!serial_port_.read_some(
                buffer, sizeof(buffer), std::chrono::milliseconds(100), received)) {
            continue;
        }
        if (received == 0U) {
            continue;
        }
        input.insert(input.end(), buffer, buffer + received);
        if (input.size() > options_.max_frame_bytes) {
            input.clear();
            continue;
        }
        while (input.size() >= kReadRequestBytes) {
            const std::vector<std::uint8_t> request(
                input.begin(), input.begin() + static_cast<std::ptrdiff_t>(kReadRequestBytes));
            input.erase(
                input.begin(), input.begin() + static_cast<std::ptrdiff_t>(kReadRequestBytes));
            const std::vector<std::uint8_t> response = handle_request(request);
            if (response.empty()) {
                continue;
            }
            std::this_thread::sleep_for(std::chrono::microseconds(1750));
            serial_port_.write_all(response.data(), response.size());
        }
    }
    return 0;
}

void ModbusRtuServer::stop() noexcept {
    stop_requested_.store(true);
}

}  // namespace uhf::modbus
