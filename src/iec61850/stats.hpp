// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <cstdint>

namespace uhf::iec61850 {

struct RuntimeStats {
    std::uint64_t connection_rejections{0U};
    std::uint64_t max_outstanding_rejections{0U};
};

}  // namespace uhf::iec61850
