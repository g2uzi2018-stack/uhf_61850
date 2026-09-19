// SPDX-License-Identifier: GPL-3.0-only
#include "v3/protocol.hpp"

#include <cmath>
#include <stdexcept>

namespace uhf::v3 {
namespace {
float scaled(float raw, LinearScale scale) {
    if (!std::isfinite(scale.multiplier) || scale.multiplier <= 0 ||
        !std::isfinite(scale.offset)) {
        throw std::invalid_argument("scale requires a finite positive multiplier and finite offset");
    }
    const float result = raw * scale.multiplier + scale.offset;
    if (!std::isfinite(result)) {
        throw std::overflow_error("engineering conversion overflow");
    }
    return result;
}
}  // namespace

ReadRequest make_read_request(std::uint8_t slave, std::uint8_t function,
                              std::uint16_t start, std::uint16_t count) {
    if (slave == 0 || slave == 255 || (function != 3 && function != 4) ||
        count == 0 || count > 125 || static_cast<std::uint32_t>(start) + count > 65536U) {
        throw std::invalid_argument("invalid Modbus read request");
    }
    ReadRequest r{};
    r.slave_id = slave;
    r.function = function;
    r.start_address = start;
    r.register_count = count;
    r.wire_frame = {slave, function, static_cast<std::uint8_t>(start >> 8U),
                    static_cast<std::uint8_t>(start & 255U),
                    static_cast<std::uint8_t>(count >> 8U),
                    static_cast<std::uint8_t>(count & 255U), 0, 0};
    const auto crc = domain::modbus_crc16(r.wire_frame.data(), 6);
    r.wire_frame[6] = static_cast<std::uint8_t>(crc & 255U);
    r.wire_frame[7] = static_cast<std::uint8_t>(crc >> 8U);
    return r;
}

std::array<ReadRequest, kRequestsPerChannel> pd_request_plan(std::size_t channel,
                                                          std::uint8_t slave) {
    if (channel < 1 || channel > kChannelCount) {
        throw std::out_of_range("PD channel must be 1..3");
    }
    std::array<ReadRequest, kRequestsPerChannel> plan{};
    std::uint16_t address = kChannelStarts[channel - 1];
    for (std::size_t i = 0; i < plan.size(); ++i) {
        const std::uint16_t count = i == 0 ? 15 : 120;
        plan[i] = make_read_request(slave, 4, address, count);
        address = static_cast<std::uint16_t>(address + count);
    }
    return plan;
}
ReadRequest current_request(std::uint8_t slave) { return make_read_request(slave, 3, 0x2001, 8); }
ReadRequest temperature_request(std::uint8_t slave) { return make_read_request(slave, 3, 1, 6); }

ReadReply decode_read_reply(const ReadRequest& request, const std::uint8_t* frame,
                           std::size_t length) {
    ReadReply out;
    if (frame == nullptr || length < 5 || length > 255 ||
        request.register_count == 0 || request.register_count > 125) {
        return out;
    }
    const auto crc = domain::modbus_crc16(frame, length - 2);
    if (frame[length - 2] != static_cast<std::uint8_t>(crc & 255U) ||
        frame[length - 1] != static_cast<std::uint8_t>(crc >> 8U)) {
        out.error = ReplyError::crc;
        return out;
    }
    if (frame[0] != request.slave_id) {
        out.error = ReplyError::slave;
        return out;
    }
    if (frame[1] == static_cast<std::uint8_t>(request.function | 0x80U)) {
        if (length == 5) {
            out.error = ReplyError::exception;
            out.exception_code = frame[2];
        }
        return out;
    }
    if (frame[1] != request.function) {
        out.error = ReplyError::function;
        return out;
    }
    const std::size_t bytes = static_cast<std::size_t>(request.register_count) * 2;
    if (frame[2] != bytes) {
        out.error = ReplyError::byte_count;
        return out;
    }
    if (length != bytes + 5) { return out; }
    out.registers.reserve(request.register_count);
    for (std::size_t i = 0; i < request.register_count; ++i) {
        const std::size_t offset = 3 + 2 * i;
        out.registers.push_back(static_cast<std::uint16_t>(
            (static_cast<std::uint16_t>(frame[offset]) << 8U) | frame[offset + 1]));
    }
    out.error = ReplyError::none;
    return out;
}

std::int16_t signed_word(std::uint16_t word) noexcept {
    const std::int32_t value = word < 0x8000U ? static_cast<std::int32_t>(word)
                                            : static_cast<std::int32_t>(word) - 65536;
    return static_cast<std::int16_t>(value);
}
PdChannel decode_pd_channel(const ChannelRegisters& registers,
                            const RegisterValidity& received) {
    PdChannel result;
    result.raw = registers;
    result.received = received;
    for (std::size_t i = 0; i < kFeatureCount; ++i) {
        auto& f = result.features[i];
        f.raw = registers[i];
        f.value = static_cast<float>((i == 0 || i == 2) ?
            static_cast<std::int32_t>(signed_word(f.raw)) : static_cast<std::int32_t>(f.raw));
        f.valid = received[i];
        if (i == 0 || i == 2 || i == 4) { f.valid = f.valid && f.value >= 0 && f.value <= 4096; }
        if (i == 3) { f.valid = f.valid && f.value <= 360; }
        if (i == 5 || i == 6) { f.value *= 0.01F; }
    }
    for (std::size_t i = 0; i < kSpectrumPoints; ++i) {
        result.spectrum_raw[i] = signed_word(registers[kSpectrumOffset + i]);
        result.spectrum_received[i] = received[kSpectrumOffset + i];
    }
    return result;
}
std::array<std::int16_t, 3> temperature_words(
    const std::array<std::uint16_t, 6>& registers) noexcept {
    return {signed_word(registers[0]), signed_word(registers[2]), signed_word(registers[4])};
}
std::array<float, 8> current_values(const std::array<std::uint16_t, 8>& registers,
                                  WordEncoding encoding, LinearScale scale) {
    std::array<float, 8> out{};
    for (std::size_t i = 0; i < out.size(); ++i) {
        const float raw = encoding == WordEncoding::signed16 ?
            static_cast<float>(signed_word(registers[i])) : static_cast<float>(registers[i]);
        out[i] = scaled(raw, scale);
    }
    return out;
}
std::array<float, 3> temperature_values(const std::array<std::uint16_t, 6>& registers,
                                      LinearScale scale) {
    const auto raw = temperature_words(registers);
    return {scaled(static_cast<float>(raw[0]), scale), scaled(static_cast<float>(raw[1]), scale),
            scaled(static_cast<float>(raw[2]), scale)};
}

}  // namespace uhf::v3
