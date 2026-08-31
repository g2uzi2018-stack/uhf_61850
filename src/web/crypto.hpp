// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace uhf::web {

std::string random_hex(std::size_t byte_count);

std::vector<unsigned char> pbkdf2_sha256(
    std::string_view password,
    const std::vector<unsigned char>& salt,
    std::uint32_t iterations);

bool constant_time_equal(std::string_view left, std::string_view right) noexcept;

}  // namespace uhf::web
