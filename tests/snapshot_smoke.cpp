// SPDX-License-Identifier: GPL-3.0-only
#include "domain/snapshot.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>

namespace {

bool expect(bool condition, const char* message) {
    return condition || (std::cerr << "snapshot smoke failed: " << message << '\n', false);
}

}  // namespace

int main() {
    using namespace uhf::domain;

    if (!expect(decode_int16(0x0000U) == 0, "zero int16") ||
        !expect(decode_int16(0xFFBAU) == -70, "signed marker value") ||
        !expect(decode_int16(0x8000U) == -32768, "minimum int16") ||
        !expect(spectrum_index(49U, 71U) == 3599U, "spectrum index")) {
        return 1;
    }

    std::array<std::uint16_t, kPd1000RegisterCount> raw{};
    raw[0] = 0xFFD8U;  // -40 dBm
    raw[1] = 65535U;
    raw[2] = 0xFFCEU;  // -50 dBm
    raw[3] = 360U;
    raw[4] = 4096U;
    for (std::size_t index = 0; index < kPd1000SpectrumPointCount; ++index) {
        raw[kPd1000SpectrumOffset + index] = 0xFFCEU;
    }
    raw[kPd1000SpectrumOffset + spectrum_index(1U, 5U)] = kInvalidDataMarker;
    raw[kPd1000SpectrumOffset + spectrum_index(2U, 7U)] = kNotRefreshedMarker;
    const ParsedSnapshot partial = parse_pd1000_registers(raw);
    if (!expect(partial.payload_status == PayloadStatus::degraded, "partial spectrum degraded") ||
        !expect(!partial.spectrum_valid[spectrum_index(1U, 5U)], "FFBA validity") ||
        !expect(partial.spectrum[spectrum_index(1U, 5U)] == -70, "FFBA decoded value") ||
        !expect(!partial.spectrum_valid[spectrum_index(2U, 7U)], "FFBC validity") ||
        !expect(partial.raw_registers[kPd1000SpectrumOffset + spectrum_index(2U, 7U)] ==
                    kNotRefreshedMarker,
            "raw marker preserved")) {
        return 1;
    }

    raw[0] = 16U;     // outside the signed dBm range
    raw[3] = 361U;    // outside the phase range
    raw[4] = 4097U;   // outside the noise range
    const ParsedSnapshot invalid_measurements = parse_pd1000_registers(raw);
    if (!expect(!invalid_measurements.measurements[0].valid, "mean range") ||
        !expect(invalid_measurements.measurements[1].valid, "frequency full uint16 range") ||
        !expect(!invalid_measurements.measurements[3].valid, "phase range") ||
        !expect(!invalid_measurements.measurements[4].valid, "noise range")) {
        return 1;
    }

    raw.fill(kNotRefreshedMarker);
    const ParsedSnapshot not_refreshed = parse_pd1000_registers(raw);
    if (!expect(not_refreshed.payload_status == PayloadStatus::not_refreshed, "all FFBC priority") ||
        !expect(not_refreshed.raw_registers[0] == kNotRefreshedMarker, "raw measurement marker") ||
        !expect(!not_refreshed.measurements[0].valid, "marker measurement invalid")) {
        return 1;
    }

    std::cout << "snapshot smoke: OK\n";
    return 0;
}
