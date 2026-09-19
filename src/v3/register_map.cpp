// SPDX-License-Identifier: GPL-3.0-only
#include "v3/register_map.hpp"
#include <limits>

namespace uhf::v3 {
namespace {
std::vector<std::uint8_t> exception(std::uint8_t function, std::uint8_t code) {
    return {static_cast<std::uint8_t>(function | 0x80U), code};
}
void append_word(std::vector<std::uint8_t>& out, std::uint16_t word) {
    out.push_back(static_cast<std::uint8_t>(word >> 8U));
    out.push_back(static_cast<std::uint8_t>(word & 0xffU));
}
}  // namespace
std::vector<std::uint8_t> serve_read_pdu(const UpstreamSnapshot& snapshot,
                                       const std::uint8_t* request, std::size_t length,
                                       InvalidHoldingPolicy policy) {
    if (request == nullptr || length == 0) { return {}; }
    const auto function = request[0];
    if (function != 2 && function != 3 && function != 4) { return exception(function, 1); }
    if (length != 5) { return exception(function, 3); }
    const std::uint32_t start = (static_cast<std::uint32_t>(request[1]) << 8U) | request[2];
    const std::uint32_t count = (static_cast<std::uint32_t>(request[3]) << 8U) | request[4];
    if (count == 0 || count > (function == 2 ? 2000U : 125U)) { return exception(function, 3); }
    if (start + count > 65536U) { return exception(function, 2); }
    if (function == 2) {
        if (start + count > kDiscreteCount) { return exception(function, 2); }
        const auto bytes = static_cast<std::size_t>((count + 7U) / 8U);
        std::vector<std::uint8_t> out(2 + bytes, 0);
        out[0] = function;
        out[1] = static_cast<std::uint8_t>(bytes);
        for (std::size_t i = 0; i < count; ++i) {
            if (!snapshot.discrete_valid[start + i]) { return exception(function, 4); }
            if (snapshot.discrete[start + i]) {
                out[2 + i / 8] |= static_cast<std::uint8_t>(1U << (i % 8));
            }
        }
        return out;
    }
    std::vector<std::uint8_t> out{function, static_cast<std::uint8_t>(2U * count)};
    out.reserve(2 + 2 * count);
    if (function == 3) {
        if (start < 1 || start + count > 1 + 2 * kValueCount) { return exception(function, 2); }
        for (std::size_t i = 0; i < count; ++i) {
            const auto offset = static_cast<std::size_t>(start - 1) + i;
            const auto& value = snapshot.measurements[offset / 2];
            if (!value.valid() && policy == InvalidHoldingPolicy::exception) { return exception(function, 4); }
            const auto words = float32_registers(value.valid() ? value.value :
                                                  std::numeric_limits<float>::quiet_NaN());
            append_word(out, words[offset % 2]);
        }
        return out;
    }
    for (std::size_t channel = 0; channel < kChannelCount; ++channel) {
        const std::uint32_t base = kChannelStarts[channel];
        if (start < base || start + count > base + kChannelRegisterCount) { continue; }
        for (std::size_t i = 0; i < count; ++i) {
            const auto index = static_cast<std::size_t>(start - base) + i;
            if (!snapshot.pd[channel].received[index]) { return exception(function, 4); }
            append_word(out, snapshot.pd[channel].raw[index]);
        }
        return out;
    }
    return exception(function, 2);
}
std::string discrete_point_table_csv() {
    std::string out = "profile_version,function_code,wire_address,name\n";
    for (std::size_t i = 0; i < kDiscreteCount; ++i) {
        out += "1,2," + std::to_string(i) + "," + std::string(kDiscreteNames[i]) + "\n";
    }
    return out;
}
}  // namespace uhf::v3
