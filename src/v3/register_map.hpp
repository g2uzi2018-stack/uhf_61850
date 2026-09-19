// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "v3/measurements.hpp"
#include "v3/protocol.hpp"
#include <array>
#include <bitset>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace uhf::v3 {
constexpr std::size_t kDiscreteCount = 15;
// One canonical order generates the published table. Integration must retain
// this version's addresses across configuration changes and export the table.
constexpr std::array<std::string_view, kDiscreteCount> kDiscreteNames{
    "pd_communication_fault", "current_communication_fault", "temperature_communication_fault",
    "pd_ch1_peak_high", "pd_ch2_peak_high", "pd_ch3_peak_high",
    "Ia_high", "Ib_high", "Ic_high", "IA_high", "IB_high", "IC_high",
    "TA_high", "TB_high", "TC_high"};
struct UpstreamSnapshot {
    ValueTable measurements{};
    std::array<PdChannel, kChannelCount> pd{};
    std::bitset<kDiscreteCount> discrete{};
    std::bitset<kDiscreteCount> discrete_valid{};
};
enum class InvalidHoldingPolicy { quiet_nan, exception };

// Pure application PDU handler shared by future RTU/TCP transports. The caller
// must pin an immutable snapshot; this function owns no sockets or serial ports.
// 03: 70 registers starting at literal wire address 1 (proposed v3 profile).
// 04: unchanged raw PD mirrors. 02: generated version-1 points at addresses 0..14.
// Missing raw PD words or unknown discrete states return exception 04 rather
// than silently publishing zero or an invented vendor sentinel.
std::vector<std::uint8_t> serve_read_pdu(const UpstreamSnapshot& snapshot,
                                       const std::uint8_t* request, std::size_t length,
                                       InvalidHoldingPolicy policy);
std::string discrete_point_table_csv();
std::string point_table_csv();
}  // namespace uhf::v3
