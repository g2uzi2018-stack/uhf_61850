// SPDX-License-Identifier: GPL-3.0-only
#include "domain/snapshot.hpp"

namespace {

bool is_reserved(std::uint16_t raw) noexcept {
    return raw == uhf::domain::kInvalidDataMarker || raw == uhf::domain::kNotRefreshedMarker;
}

uhf::domain::Measurement signed_measurement(std::uint16_t raw, std::int32_t minimum, std::int32_t maximum) {
    const std::int16_t decoded = uhf::domain::decode_int16(raw);
    const std::int32_t value = decoded;
    return uhf::domain::Measurement{raw, value, !is_reserved(raw) && value >= minimum && value <= maximum};
}

uhf::domain::Measurement unsigned_measurement(
    std::uint16_t raw, std::uint32_t minimum, std::uint32_t maximum) {
    const std::uint32_t value = raw;
    return uhf::domain::Measurement{
        raw,
        static_cast<std::int32_t>(value),
        !is_reserved(raw) && value >= minimum && value <= maximum};
}

}  // namespace

namespace uhf::domain {

std::int16_t decode_int16(std::uint16_t raw) noexcept {
    if (raw <= 0x7FFFU) {
        return static_cast<std::int16_t>(raw);
    }
    const std::int32_t signed_value = static_cast<std::int32_t>(raw) - 0x10000;
    return static_cast<std::int16_t>(signed_value);
}

std::size_t spectrum_index(std::size_t cycle, std::size_t phase_bin) noexcept {
    return cycle * kPd1000SpectrumPhaseBins + phase_bin;
}

ParsedSnapshot parse_pd1000_registers(
    const std::array<std::uint16_t, kPd1000RegisterCount>& raw_registers) {
    ParsedSnapshot snapshot;
    snapshot.raw_registers = raw_registers;
    snapshot.measurements[0] = signed_measurement(raw_registers[0], -70, 15);
    snapshot.measurements[1] = unsigned_measurement(raw_registers[1], 0U, 65535U);
    snapshot.measurements[2] = signed_measurement(raw_registers[2], -70, 15);
    snapshot.measurements[3] = unsigned_measurement(raw_registers[3], 0U, 360U);
    snapshot.measurements[4] = unsigned_measurement(raw_registers[4], 0U, 4096U);

    bool all_not_refreshed = true;
    bool all_valid = true;
    for (std::size_t index = 0; index < kPd1000SpectrumPointCount; ++index) {
        const std::uint16_t raw = raw_registers[kPd1000SpectrumOffset + index];
        const std::int16_t value = decode_int16(raw);
        const bool valid = !is_reserved(raw) && value >= -70 && value <= 15;
        snapshot.spectrum[index] = value;
        snapshot.spectrum_valid.set(index, valid);
        all_not_refreshed = all_not_refreshed && raw == kNotRefreshedMarker;
        all_valid = all_valid && valid;
    }

    bool measurements_valid = true;
    for (const Measurement& measurement : snapshot.measurements) {
        measurements_valid = measurements_valid && measurement.valid;
    }
    if (all_not_refreshed) {
        snapshot.payload_status = PayloadStatus::not_refreshed;
    } else if (!all_valid || !measurements_valid) {
        snapshot.payload_status = PayloadStatus::degraded;
    } else {
        snapshot.payload_status = PayloadStatus::good;
    }
    return snapshot;
}

}  // namespace uhf::domain
