// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <array>
#include <bitset>
#include <cstddef>
#include <cstdint>

namespace uhf::domain {

constexpr std::size_t kPd1000RegisterCount = 3615U;
constexpr std::size_t kPd1000SpectrumOffset = 15U;
constexpr std::size_t kPd1000SpectrumCycles = 50U;
constexpr std::size_t kPd1000SpectrumPhaseBins = 72U;
constexpr std::size_t kPd1000SpectrumPointCount =
    kPd1000SpectrumCycles * kPd1000SpectrumPhaseBins;
constexpr std::uint16_t kInvalidDataMarker = 0xFFBAU;
constexpr std::uint16_t kNotRefreshedMarker = 0xFFBCU;

enum class PayloadStatus {
    good,
    degraded,
    not_refreshed,
};

struct Measurement {
    std::uint16_t raw{0};
    std::int32_t value{0};
    bool valid{false};
};

struct ParsedSnapshot {
    std::array<std::uint16_t, kPd1000RegisterCount> raw_registers{};
    std::array<Measurement, 5U> measurements{};
    std::array<std::int16_t, kPd1000SpectrumPointCount> spectrum{};
    std::bitset<kPd1000SpectrumPointCount> spectrum_valid;
    PayloadStatus payload_status{PayloadStatus::degraded};
};

std::int16_t decode_int16(std::uint16_t raw) noexcept;

std::size_t spectrum_index(std::size_t cycle, std::size_t phase_bin) noexcept;

ParsedSnapshot parse_pd1000_registers(
    const std::array<std::uint16_t, kPd1000RegisterCount>& raw_registers);

}  // namespace uhf::domain
