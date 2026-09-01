// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <cstdint>

namespace uhf::iec61850 {

struct RuntimeStats {
    std::uint64_t connection_rejections{0U};
    std::uint64_t malformed_pdu_rejections{0U};
    std::uint64_t oversized_pdu_rejections{0U};
    std::uint64_t request_element_rejections{0U};
    std::uint64_t ber_depth_rejections{0U};
    std::uint64_t max_outstanding_rejections{0U};
};

}  // namespace uhf::iec61850
