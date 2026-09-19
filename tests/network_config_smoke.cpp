// SPDX-License-Identifier: GPL-3.0-only
#include "platform/network/network_config.hpp"
#include "test_check.hpp"

#include <string>

int main() {
    uhf::network::NetworkConfig config;
    UHF_TEST_CHECK(uhf::network::validate(config).valid);
    const std::string json = uhf::network::to_json(config);
    UHF_TEST_CHECK(json.find("\"eth0\"") != std::string::npos);
    UHF_TEST_CHECK(json.find("\"mode\":\"static\"") != std::string::npos);
    UHF_TEST_CHECK(json.find("\"netmask\":\"255.255.255.0\"") != std::string::npos);
    const std::string flat = uhf::network::to_flat_json(config);
    UHF_TEST_CHECK(flat.find("\"eth0_netmask\":\"255.255.255.0\"") != std::string::npos);
    uhf::network::NetworkConfig round_trip;
    UHF_TEST_CHECK(uhf::network::parse_flat_json(flat, round_trip));
    UHF_TEST_CHECK(round_trip.eth0.address == config.eth0.address);
    UHF_TEST_CHECK(round_trip.eth0.gateway == config.eth0.gateway);
    UHF_TEST_CHECK(round_trip.eth1.address == config.eth1.address);
    UHF_TEST_CHECK(round_trip.eth0.prefix == 24U);
    UHF_TEST_CHECK(uhf::network::netmask_for_prefix(24U) == "255.255.255.0");
    std::uint8_t prefix = 0U;
    UHF_TEST_CHECK(uhf::network::prefix_from_netmask("255.255.255.0", prefix));
    UHF_TEST_CHECK(prefix == 24U);

    std::string netmask_only = flat;
    const std::string eth0_prefix = ",\"eth0_prefix\":24";
    const std::size_t eth0_prefix_position = netmask_only.find(eth0_prefix);
    UHF_TEST_CHECK(eth0_prefix_position != std::string::npos);
    netmask_only.erase(eth0_prefix_position, eth0_prefix.size());
    const std::string eth1_prefix = ",\"eth1_prefix\":24";
    const std::size_t eth1_prefix_position = netmask_only.find(eth1_prefix);
    UHF_TEST_CHECK(eth1_prefix_position != std::string::npos);
    netmask_only.erase(eth1_prefix_position, eth1_prefix.size());
    UHF_TEST_CHECK(uhf::network::parse_flat_json(netmask_only, round_trip));
    UHF_TEST_CHECK(round_trip.eth0.prefix == 24U && round_trip.eth1.prefix == 24U);
    std::string inconsistent = flat;
    const std::string bad_mask = "\"eth0_netmask\":\"255.255.0.0\"";
    const std::size_t bad_mask_position = inconsistent.find(
        "\"eth0_netmask\":\"255.255.255.0\"");
    UHF_TEST_CHECK(bad_mask_position != std::string::npos);
    inconsistent.replace(
        bad_mask_position, std::string("\"eth0_netmask\":\"255.255.255.0\"").size(),
        bad_mask);
    UHF_TEST_CHECK(!uhf::network::parse_flat_json(inconsistent, round_trip));

    config.eth0.name = "eno1";
    UHF_TEST_CHECK(!uhf::network::validate(config).valid);
    config = uhf::network::NetworkConfig{};
    config.eth0.gateway = "10.0.0.1";
    UHF_TEST_CHECK(!uhf::network::validate(config).valid);
    config = uhf::network::NetworkConfig{};
    config.eth1.gateway = "192.168.0.1";
    UHF_TEST_CHECK(!uhf::network::validate(config).valid);
    config = uhf::network::NetworkConfig{};
    config.eth0.mode = uhf::network::Mode::dhcp;
    config.eth0.address.clear();
    config.eth0.gateway.clear();
    config.eth0.hostname = "uhf-gateway";
    UHF_TEST_CHECK(uhf::network::validate(config).valid);
    config.eth0.address = "192.168.3.230";
    UHF_TEST_CHECK(!uhf::network::validate(config).valid);
    config = uhf::network::NetworkConfig{};
    config.eth1.dns_count = 1U;
    config.eth1.dns[0] = "not-an-ip";
    UHF_TEST_CHECK(!uhf::network::validate(config).valid);
    config = uhf::network::NetworkConfig{};
    config.eth1.hostname = "bad host";
    UHF_TEST_CHECK(!uhf::network::validate(config).valid);
    UHF_TEST_CHECK(!uhf::network::parse_flat_json("{\"eth0_mode\":\"static\"}", round_trip));
    UHF_TEST_CHECK(!uhf::network::parse_flat_json(
        flat.substr(0U, flat.size() - 1U) + ",\"eth0_mode\":\"dhcp\"}", round_trip));
    UHF_TEST_CHECK(!uhf::network::prefix_from_netmask("255.0.255.0", prefix));
    return 0;
}
