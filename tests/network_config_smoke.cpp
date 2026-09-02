// SPDX-License-Identifier: GPL-3.0-only
#include "platform/network/network_config.hpp"

#include <cassert>
#include <string>

int main() {
    uhf::network::NetworkConfig config;
    assert(uhf::network::validate(config).valid);
    const std::string json = uhf::network::to_json(config);
    assert(json.find("\"eth0\"") != std::string::npos);
    assert(json.find("\"mode\":\"static\"") != std::string::npos);
    assert(json.find("\"netmask\":\"255.255.255.0\"") != std::string::npos);
    const std::string flat = uhf::network::to_flat_json(config);
    assert(flat.find("\"eth0_netmask\":\"255.255.255.0\"") != std::string::npos);
    uhf::network::NetworkConfig round_trip;
    assert(uhf::network::parse_flat_json(flat, round_trip));
    assert(round_trip.eth0.address == config.eth0.address);
    assert(round_trip.eth0.gateway == config.eth0.gateway);
    assert(round_trip.eth1.address == config.eth1.address);
    assert(round_trip.eth0.prefix == 24U);
    assert(uhf::network::netmask_for_prefix(24U) == "255.255.255.0");
    std::uint8_t prefix = 0U;
    assert(uhf::network::prefix_from_netmask("255.255.255.0", prefix));
    assert(prefix == 24U);

    std::string netmask_only = flat;
    const std::string eth0_prefix = ",\"eth0_prefix\":24";
    const std::size_t eth0_prefix_position = netmask_only.find(eth0_prefix);
    assert(eth0_prefix_position != std::string::npos);
    netmask_only.erase(eth0_prefix_position, eth0_prefix.size());
    const std::string eth1_prefix = ",\"eth1_prefix\":24";
    const std::size_t eth1_prefix_position = netmask_only.find(eth1_prefix);
    assert(eth1_prefix_position != std::string::npos);
    netmask_only.erase(eth1_prefix_position, eth1_prefix.size());
    assert(uhf::network::parse_flat_json(netmask_only, round_trip));
    assert(round_trip.eth0.prefix == 24U && round_trip.eth1.prefix == 24U);
    std::string inconsistent = flat;
    const std::string bad_mask = "\"eth0_netmask\":\"255.255.0.0\"";
    const std::size_t bad_mask_position = inconsistent.find(
        "\"eth0_netmask\":\"255.255.255.0\"");
    assert(bad_mask_position != std::string::npos);
    inconsistent.replace(
        bad_mask_position, std::string("\"eth0_netmask\":\"255.255.255.0\"").size(),
        bad_mask);
    assert(!uhf::network::parse_flat_json(inconsistent, round_trip));

    config.eth0.name = "eno1";
    assert(!uhf::network::validate(config).valid);
    config = uhf::network::NetworkConfig{};
    config.eth0.gateway = "10.0.0.1";
    assert(!uhf::network::validate(config).valid);
    config = uhf::network::NetworkConfig{};
    config.eth1.gateway = "192.168.0.1";
    assert(!uhf::network::validate(config).valid);
    config = uhf::network::NetworkConfig{};
    config.eth0.mode = uhf::network::Mode::dhcp;
    config.eth0.address.clear();
    config.eth0.gateway.clear();
    config.eth0.hostname = "uhf-gateway";
    assert(uhf::network::validate(config).valid);
    config.eth0.address = "192.168.3.230";
    assert(!uhf::network::validate(config).valid);
    config = uhf::network::NetworkConfig{};
    config.eth1.dns_count = 1U;
    config.eth1.dns[0] = "not-an-ip";
    assert(!uhf::network::validate(config).valid);
    config = uhf::network::NetworkConfig{};
    config.eth1.hostname = "bad host";
    assert(!uhf::network::validate(config).valid);
    assert(!uhf::network::parse_flat_json("{\"eth0_mode\":\"static\"}", round_trip));
    assert(!uhf::network::parse_flat_json(
        flat.substr(0U, flat.size() - 1U) + ",\"eth0_mode\":\"dhcp\"}", round_trip));
    assert(!uhf::network::prefix_from_netmask("255.0.255.0", prefix));
    return 0;
}
