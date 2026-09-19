// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <array>
#include <charconv>
#include <cstdint>
#include <optional>
#include <string_view>
#include <system_error>

namespace uhf::acquisition {

enum class SerialParity { none, even, odd };

struct SerialSettings {
    std::uint32_t baud{115200U};
    std::uint8_t data_bits{8U};
    SerialParity parity{SerialParity::none};
    std::uint8_t stop_bits{1U};
};

inline bool supported_serial_baud(std::uint32_t baud) noexcept {
    constexpr std::array<std::uint32_t, 9U> supported{
        1200U, 2400U, 4800U, 9600U, 19200U,
        38400U, 57600U, 115200U, 230400U};
    for (const std::uint32_t candidate : supported) {
        if (candidate == baud) {
            return true;
        }
    }
    return false;
}

// Canonical form: "115200/8N1". An absent optional means the explicit
// string "unconfigured"; malformed or unsupported profiles are rejected.
inline std::optional<SerialSettings> parse_serial_profile(
    std::string_view profile, bool& valid) noexcept {
    valid = false;
    if (profile == "unconfigured") {
        valid = true;
        return std::nullopt;
    }
    const std::size_t separator = profile.find('/');
    if (separator == std::string_view::npos || separator == 0U ||
        separator + 4U != profile.size()) {
        return std::nullopt;
    }
    std::uint32_t baud = 0U;
    const auto parsed = std::from_chars(
        profile.data(), profile.data() + separator, baud);
    if (parsed.ec != std::errc{} || parsed.ptr != profile.data() + separator ||
        !supported_serial_baud(baud)) {
        return std::nullopt;
    }
    const char data_bits = profile[separator + 1U];
    const char parity = profile[separator + 2U];
    const char stop_bits = profile[separator + 3U];
    if ((data_bits != '7' && data_bits != '8') ||
        (parity != 'N' && parity != 'E' && parity != 'O') ||
        (stop_bits != '1' && stop_bits != '2')) {
        return std::nullopt;
    }
    SerialSettings settings;
    settings.baud = baud;
    settings.data_bits = static_cast<std::uint8_t>(data_bits - '0');
    settings.parity = parity == 'E' ? SerialParity::even
        : parity == 'O' ? SerialParity::odd : SerialParity::none;
    settings.stop_bits = static_cast<std::uint8_t>(stop_bits - '0');
    valid = true;
    return settings;
}

}  // namespace uhf::acquisition
