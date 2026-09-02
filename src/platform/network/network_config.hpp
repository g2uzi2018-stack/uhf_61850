// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace uhf::network {

constexpr std::size_t kMaxDnsServers = 2U;

enum class Mode {
    static_address,
    dhcp,
};

struct InterfaceConfig {
    std::string name;
    Mode mode{Mode::static_address};
    std::string address;
    std::uint8_t prefix{24U};
    std::string gateway;
    std::array<std::string, kMaxDnsServers> dns{};
    std::uint8_t dns_count{0U};
    std::string hostname;
    std::uint16_t dhcp_timeout_seconds{15U};
};

struct NetworkConfig {
    InterfaceConfig eth0{
        "eth0", Mode::static_address, "192.168.3.230", 24U, "192.168.3.1", {}, 0U, "", 15U};
    InterfaceConfig eth1{
        "eth1", Mode::static_address, "192.168.0.230", 24U, "", {}, 0U, "", 15U};
};

struct ValidationResult {
    bool valid{false};
    std::string field;
    std::string message;
};

ValidationResult validate(const NetworkConfig& config);
std::string mode_name(Mode mode) noexcept;
std::string netmask_for_prefix(std::uint8_t prefix);
bool prefix_from_netmask(std::string_view netmask, std::uint8_t& prefix) noexcept;
bool parse_flat_json(std::string_view json, NetworkConfig& config);
std::string to_flat_json(const NetworkConfig& config);
std::string to_json(const NetworkConfig& config);

}  // namespace uhf::network
