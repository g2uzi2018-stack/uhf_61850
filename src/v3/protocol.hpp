// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "domain/modbus.hpp"

#include <array>
#include <bitset>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace uhf::v3 {

constexpr std::size_t kChannelCount = 3;
constexpr std::size_t kChannelRegisterCount = 3615;
constexpr std::size_t kFeatureCount = 7;
constexpr std::size_t kSpectrumOffset = 15;
constexpr std::size_t kSpectrumCycles = 50;
constexpr std::size_t kPhaseBins = 72;
constexpr std::size_t kSpectrumPoints = kSpectrumCycles * kPhaseBins;
constexpr std::size_t kRequestsPerChannel = 31;
constexpr std::array<std::uint16_t, kChannelCount> kChannelStarts{10001, 15001, 20001};
using ChannelRegisters = std::array<std::uint16_t, kChannelRegisterCount>;
using RegisterValidity = std::bitset<kChannelRegisterCount>;
using ReadRequest = domain::ModbusReadRequest;

// Addresses are literal wire addresses; no implicit -1 adjustment. The vendor
// explicitly supports device addresses 1..254 (not a generic Modbus profile).
ReadRequest make_read_request(std::uint8_t slave, std::uint8_t function,
                              std::uint16_t start, std::uint16_t count);
std::array<ReadRequest, kRequestsPerChannel> pd_request_plan(
    std::size_t channel, std::uint8_t slave = 1);
ReadRequest current_request(std::uint8_t slave = 1);
ReadRequest temperature_request(std::uint8_t slave = 1);

enum class ReplyError { none, length, crc, slave, function, byte_count, exception };
struct ReadReply {
    ReplyError error{ReplyError::length};
    std::uint8_t exception_code{0};
    std::vector<std::uint16_t> registers;
    bool ok() const noexcept { return error == ReplyError::none; }
};
// One already-delimited RTU frame; transport timeouts and late-frame isolation
// belong to the acquisition layer. Never returns partial registers on failure.
ReadReply decode_read_reply(const ReadRequest& request, const std::uint8_t* frame,
                           std::size_t length);

struct PdFeature {
    std::uint16_t raw{0};
    float value{0};
    bool valid{false};
};
struct PdChannel {
    ChannelRegisters raw{};
    RegisterValidity received{};
    // mean mV, frequency /s, peak mV, phase degrees, noise mV,
    // 50-Hz percent points, 100-Hz percent points.
    std::array<PdFeature, kFeatureCount> features{};
    // Raw signed samples only: the vendor has not specified spectrum units.
    // In particular, never import PD1000's FFBA/FFBC sentinel meanings.
    std::array<std::int16_t, kSpectrumPoints> spectrum_raw{};
    std::bitset<kSpectrumPoints> spectrum_received{};
};
PdChannel decode_pd_channel(const ChannelRegisters& registers,
                            const RegisterValidity& received);
std::int16_t signed_word(std::uint16_t word) noexcept;
std::array<std::int16_t, 3> temperature_words(
    const std::array<std::uint16_t, 6>& registers) noexcept;

// The unsigned/signed interpretation and engineering scale of the current
// device are not specified in v3.0. Callers must supply them explicitly.
enum class WordEncoding { unsigned16, signed16 };
struct LinearScale { float multiplier; float offset; };
std::array<float, 8> current_values(const std::array<std::uint16_t, 8>& registers,
                                  WordEncoding encoding, LinearScale scale);
std::array<float, 3> temperature_values(const std::array<std::uint16_t, 6>& registers,
                                      LinearScale scale);

}  // namespace uhf::v3
