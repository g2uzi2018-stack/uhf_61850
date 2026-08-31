// SPDX-License-Identifier: GPL-3.0-only
#include "domain/modbus.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>

namespace {

int fail(const std::string& message) {
    std::cerr << "domain smoke failed: " << message << '\n';
    return 1;
}

}  // namespace

int main(int argc, char* argv[]) {
    if (argc != 2) {
        return fail("expected the request-plan fixture path");
    }

    std::ifstream fixture(argv[1]);
    if (!fixture) {
        return fail("unable to open request-plan fixture");
    }

    const auto plan = uhf::domain::pd1000_request_plan();
    std::string line;
    std::size_t line_number = 0;
    while (std::getline(fixture, line)) {
        ++line_number;
        std::istringstream input(line);
        std::size_t ordinal = 0;
        std::uint32_t start_address = 0;
        std::uint32_t register_count = 0;
        std::array<unsigned int, 8U> expected{};
        if (!(input >> ordinal >> start_address >> register_count)) {
            return fail("malformed fixture line " + std::to_string(line_number));
        }
        for (unsigned int& byte : expected) {
            input >> std::hex >> byte;
            if (!input || byte > 0xFFU) {
                return fail("malformed frame on fixture line " + std::to_string(line_number));
            }
        }
        if (ordinal != line_number || ordinal > plan.size() ||
            start_address != plan[ordinal - 1U].start_address ||
            register_count != plan[ordinal - 1U].register_count) {
            return fail("address/count mismatch on fixture line " + std::to_string(line_number));
        }
        for (std::size_t index = 0; index < expected.size(); ++index) {
            if (plan[ordinal - 1U].wire_frame[index] != expected[index]) {
                std::ostringstream message;
                message << "frame mismatch on fixture line " << line_number << " byte "
                        << index;
                return fail(message.str());
            }
        }
        const std::uint16_t crc = uhf::domain::modbus_crc16(
            plan[ordinal - 1U].wire_frame.data(), 6U);
        const std::uint16_t frame_crc = static_cast<std::uint16_t>(
            plan[ordinal - 1U].wire_frame[6] |
            static_cast<std::uint16_t>(plan[ordinal - 1U].wire_frame[7]) << 8U);
        if (crc != frame_crc) {
            return fail("CRC mismatch on fixture line " + std::to_string(line_number));
        }
    }

    if (line_number != uhf::domain::kPd1000RequestCount ||
        plan.back().start_address + plan.back().register_count - 1U !=
            uhf::domain::kPd1000LastAddress ||
        plan.front().wire_frame[2] != 0x27U || plan.front().wire_frame[3] != 0x11U) {
        return fail("request plan does not cover the confirmed address range");
    }

    std::cout << "domain smoke: OK\n";
    return 0;
}
