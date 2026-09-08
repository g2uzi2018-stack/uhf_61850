// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <cstdint>
#include <string>

namespace uhf::iec61850 {

struct RuntimeEndpoint {
    std::string bind_address;
    std::uint16_t port{0U};
};

}  // namespace uhf::iec61850
