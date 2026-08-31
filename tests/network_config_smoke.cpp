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
    const std::string flat = uhf::network::to_flat_json(config);
    uhf::network::NetworkConfig round_trip;
    assert(uhf::network::parse_flat_json(flat, round_trip));
    assert(round_trip.eth0.address == config.eth0.address);
    assert(round_trip.eth0.gateway == config.eth0.gateway);
    assert(round_trip.eth1.address == config.eth1.address);

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
    return 0;
}
