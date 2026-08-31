// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <string_view>

namespace uhf::systemd {

bool notify(std::string_view message) noexcept;

}  // namespace uhf::systemd
