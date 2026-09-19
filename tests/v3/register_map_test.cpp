// SPDX-License-Identifier: GPL-3.0-only
#include "v3/register_map.hpp"
#include "test_support.hpp"
#include <algorithm>
#include <random>
using namespace uhf::v3;
namespace {
std::vector<std::uint8_t> read(const UpstreamSnapshot& snapshot, std::uint8_t function,
                              std::uint16_t start, std::uint16_t count,
                              InvalidHoldingPolicy policy = InvalidHoldingPolicy::exception) {
    const std::array<std::uint8_t, 5> pdu{function, static_cast<std::uint8_t>(start >> 8U),
        static_cast<std::uint8_t>(start & 255U), static_cast<std::uint8_t>(count >> 8U),
        static_cast<std::uint8_t>(count & 255U)};
    return serve_read_pdu(snapshot, pdu.data(), pdu.size(), policy);
}
}
int main() {
    try {
        UpstreamSnapshot snapshot;
        for (std::size_t i = 0; i < kValueCount; ++i) { snapshot.measurements[i] = valid_value(static_cast<float>(i) + 1); }
        for (std::size_t c = 0; c < kChannelCount; ++c) {
            snapshot.pd[c].received.set();
            for (std::size_t i = 0; i < kChannelRegisterCount; ++i) {
                snapshot.pd[c].raw[i] = static_cast<std::uint16_t>(c * 10000 + i);
            }
        }
        snapshot.discrete_valid.set();
        snapshot.discrete.set(0); snapshot.discrete.set(8); snapshot.discrete.set(14);
        check(read(snapshot,3,1,2) == std::vector<std::uint8_t>{3,4,0x3f,0x80,0,0}, "float 1 golden");
        check(read(snapshot,3,2,2) == std::vector<std::uint8_t>{3,4,0,0,0x40,0}, "unaligned half-float read");
        const auto full = read(snapshot,3,1,70);
        check(full.size() == 142 && full[1] == 140, "all 35 floats = 70 registers");
        for (std::uint16_t start = 1; start <= 70; ++start) {
            const auto part = read(snapshot,3,start,1);
            const std::size_t offset = 2U * static_cast<std::size_t>(start);
            check(part == std::vector<std::uint8_t>{3,2,full[offset],full[offset+1]}, "arbitrary holding start");
        }
        check(read(snapshot,3,0,1) == std::vector<std::uint8_t>{0x83,2}, "no zero-based holding alias");
        check(read(snapshot,3,70,2) == std::vector<std::uint8_t>{0x83,2}, "holding range end");
        check(read(snapshot,3,1,0) == std::vector<std::uint8_t>{0x83,3}, "zero count");
        check(read(snapshot,4,10001,126) == std::vector<std::uint8_t>{0x84,3}, "125 limit");
        check(read(snapshot,4,65535,2) == std::vector<std::uint8_t>{0x84,2}, "address wrap");
        check(read(snapshot,4,1,1) == std::vector<std::uint8_t>{0x84,2}, "separate register spaces");
        check(read(snapshot,6,1,1) == std::vector<std::uint8_t>{0x86,1}, "read-only service");
        std::mt19937 random(61850);
        for (std::size_t c = 0; c < kChannelCount; ++c) {
            for (int n = 0; n < 500; ++n) {
                const auto count = static_cast<std::uint16_t>(1 + random() % 125);
                const auto offset = static_cast<std::uint16_t>(random() % (kChannelRegisterCount - count + 1));
                const auto start = static_cast<std::uint16_t>(kChannelStarts[c] + offset);
                const auto reply = read(snapshot,4,start,count);
                check(reply.size() == 2U + 2U * count, "PD partial response length");
                for (std::size_t i = 0; i < count; ++i) {
                    const auto word = snapshot.pd[c].raw[static_cast<std::size_t>(offset) + i];
                    check(reply[2 + 2*i] == (word >> 8U) && reply[3 + 2*i] == (word & 255U), "raw PD mirror unchanged");
                }
            }
            check(read(snapshot,4,static_cast<std::uint16_t>(kChannelStarts[c] + 3614),2) ==
                std::vector<std::uint8_t>{0x84,2}, "PD boundary cannot bridge unmapped gap");
        }
        snapshot.pd[0].received.reset(2);
        check(read(snapshot,4,10001,3) == std::vector<std::uint8_t>{0x84,4}, "unknown PD word cannot be fake zero");
        check(read(snapshot,4,15001,3).size() == 8, "bad PD channel does not poison another");
        snapshot.measurements[0] = {};
        check(read(snapshot,3,1,2) == std::vector<std::uint8_t>{0x83,4}, "explicit invalid-value exception");
        check(read(snapshot,3,1,2,InvalidHoldingPolicy::quiet_nan) ==
            std::vector<std::uint8_t>{3,4,0x7f,0xc0,0,0}, "explicit NaN policy");
        check(read(snapshot,2,0,15) == std::vector<std::uint8_t>{2,2,1,0x41}, "discrete bit order and padding");
        check(read(snapshot,2,7,8) == std::vector<std::uint8_t>{2,1,0x82}, "non-byte-aligned discrete read");
        check(read(snapshot,2,14,2) == std::vector<std::uint8_t>{0x82,2}, "discrete range");
        snapshot.discrete_valid.reset(8);
        check(read(snapshot,2,8,1) == std::vector<std::uint8_t>{0x82,4}, "unknown state cannot mean healthy");
        check(read(snapshot,2,0,1) == std::vector<std::uint8_t>{2,1,1}, "unrelated discrete still readable");
        const auto csv = discrete_point_table_csv();
        check(std::count(csv.begin(),csv.end(),'\n') == 16, "generated table contains 15 points");
        check(csv.find("1,2,14,TC_high\n") != std::string::npos, "published table matches wire order");
        const auto full_csv = point_table_csv();
        check(std::count(full_csv.begin(), full_csv.end(), '\n') == 78,
              "full point table contains all grouped mappings");
        check(full_csv.find("1,3,69,2,TCG,ieee754_binary32_abcd\n") != std::string::npos,
              "full point table contains last calculated value");
        check(full_csv.find("1,4,20016,3600,pd_ch3_spectrum,int16_raw\n") !=
                  std::string::npos,
              "full point table contains third PD spectrum range");
        check(full_csv.find("1,2,14,1,TC_high,bit\n") != std::string::npos,
              "full point table contains last discrete point");
        check(serve_read_pdu(snapshot,nullptr,10,InvalidHoldingPolicy::exception).empty(), "null request");
        const std::array<std::uint8_t,2> short_pdu{3,1};
        check(serve_read_pdu(snapshot,short_pdu.data(),short_pdu.size(),InvalidHoldingPolicy::exception) ==
            std::vector<std::uint8_t>{0x83,3}, "truncated PDU");
        for (int n = 0; n < 10000; ++n) {
            std::array<std::uint8_t,16> bytes{};
            for (auto& byte : bytes) { byte = static_cast<std::uint8_t>(random() & 255U); }
            const auto reply = serve_read_pdu(snapshot,bytes.data(),random() % 17,InvalidHoldingPolicy::exception);
            check(reply.size() <= 252, "response allocation bounded for malformed requests");
        }
        std::cout << "v3 register map: partial reads, quality, point table and malformed-input checks passed\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
