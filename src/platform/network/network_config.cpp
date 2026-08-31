// SPDX-License-Identifier: GPL-3.0-only
#include "platform/network/network_config.hpp"

#include <arpa/inet.h>
#include <cctype>
#include <cstddef>
#include <string>
#include <string_view>

namespace {

uhf::network::ValidationResult valid_result() {
    return uhf::network::ValidationResult{true, {}, {}};
}

uhf::network::ValidationResult invalid_result(
    std::string field, std::string message) {
    return uhf::network::ValidationResult{false, std::move(field), std::move(message)};
}

bool valid_ipv4(std::string_view value) noexcept {
    in_addr address{};
    return !value.empty() && ::inet_pton(AF_INET, std::string(value).c_str(), &address) == 1;
}

bool same_subnet(
    std::string_view address, std::uint8_t prefix, std::string_view gateway) noexcept {
    in_addr parsed_address{};
    in_addr parsed_gateway{};
    if (::inet_pton(AF_INET, std::string(address).c_str(), &parsed_address) != 1 ||
        ::inet_pton(AF_INET, std::string(gateway).c_str(), &parsed_gateway) != 1) {
        return false;
    }
    const std::uint32_t address_host = ntohl(parsed_address.s_addr);
    const std::uint32_t gateway_host = ntohl(parsed_gateway.s_addr);
    const std::uint32_t mask = prefix == 0U
        ? 0U
        : static_cast<std::uint32_t>(0xFFFFFFFFU << (32U - prefix));
    return (address_host & mask) == (gateway_host & mask) && address_host != gateway_host;
}

bool valid_hostname(std::string_view value) noexcept {
    if (value.empty() || value.size() > 63U || value.front() == '.' || value.back() == '.') {
        return false;
    }
    char previous = '\0';
    for (const char character : value) {
        const unsigned char unsigned_character = static_cast<unsigned char>(character);
        if (!(std::isalnum(unsigned_character) != 0 || character == '-' || character == '_' ||
              character == '.')) {
            return false;
        }
        if (character == '.' && previous == '.') {
            return false;
        }
        previous = character;
    }
    return true;
}

bool needs_json_escape(char character) noexcept {
    return character == '"' || character == '\\' ||
        static_cast<unsigned char>(character) < 0x20U;
}

std::string json_escape(std::string_view value) {
    std::string escaped;
    escaped.reserve(value.size());
    for (const char character : value) {
        if (character == '"' || character == '\\') {
            escaped.push_back('\\');
            escaped.push_back(character);
        } else if (needs_json_escape(character)) {
            escaped += "\\u00";
            constexpr char hex[] = "0123456789abcdef";
            const unsigned char code = static_cast<unsigned char>(character);
            escaped.push_back(hex[code >> 4U]);
            escaped.push_back(hex[code & 0x0FU]);
        } else {
            escaped.push_back(character);
        }
    }
    return escaped;
}

std::string interface_json(const uhf::network::InterfaceConfig& config) {
    std::string result = "{\"mode\":\"" + uhf::network::mode_name(config.mode) +
        "\",\"address\":\"" + json_escape(config.address) +
        "\",\"prefix\":" + std::to_string(config.prefix) +
        ",\"gateway\":\"" + json_escape(config.gateway) + "\",\"dns\":[";
    for (std::size_t index = 0U; index < config.dns_count; ++index) {
        if (index != 0U) {
            result.push_back(',');
        }
        result += "\"" + json_escape(config.dns[index]) + "\"";
    }
    result += "] ,\"hostname\":\"";
    result += json_escape(config.hostname);
    result += "\",\"dhcp_timeout_seconds\":" +
        std::to_string(config.dhcp_timeout_seconds) + "}";
    return result;
}

uhf::network::ValidationResult validate_interface(
    const uhf::network::InterfaceConfig& config, std::string_view expected_name) {
    const std::string prefix = std::string(expected_name) + ".";
    if (config.name != expected_name) {
        return invalid_result(prefix + "name", "interface must be eth0 or eth1");
    }
    if (config.dns_count > uhf::network::kMaxDnsServers) {
        return invalid_result(prefix + "dns", "at most two DNS servers are allowed");
    }
    if (config.mode == uhf::network::Mode::static_address) {
        if (!valid_ipv4(config.address)) {
            return invalid_result(prefix + "address", "static address must be IPv4");
        }
        if (config.prefix == 0U || config.prefix > 32U) {
            return invalid_result(prefix + "prefix", "prefix must be between 1 and 32");
        }
        if (!config.gateway.empty() &&
            (!valid_ipv4(config.gateway) || !same_subnet(config.address, config.prefix, config.gateway))) {
            return invalid_result(prefix + "gateway", "gateway must be IPv4 and in the interface subnet");
        }
    } else if (config.mode == uhf::network::Mode::dhcp) {
        if (!config.address.empty() || !config.gateway.empty() || config.dns_count != 0U) {
            return invalid_result(prefix, "DHCP must not include static address, gateway or DNS");
        }
        if (config.dhcp_timeout_seconds != 15U) {
            return invalid_result(prefix + "dhcp_timeout_seconds", "DHCP timeout is fixed at 15 seconds");
        }
    } else {
        return invalid_result(prefix + "mode", "mode must be static or dhcp");
    }
    if (!config.hostname.empty() && !valid_hostname(config.hostname)) {
        return invalid_result(prefix + "hostname", "hostname contains unsupported characters");
    }
    for (std::size_t index = 0U; index < config.dns_count; ++index) {
        if (!valid_ipv4(config.dns[index])) {
            return invalid_result(prefix + ".dns", "DNS server must be IPv4");
        }
    }
    return valid_result();
}

}  // namespace

namespace uhf::network {

ValidationResult validate(const NetworkConfig& config) {
    ValidationResult result = validate_interface(config.eth0, "eth0");
    if (!result.valid) {
        return result;
    }
    result = validate_interface(config.eth1, "eth1");
    if (!result.valid) {
        return result;
    }
    if (config.eth0.mode == Mode::static_address && config.eth1.mode == Mode::static_address &&
        config.eth0.address == config.eth1.address) {
        return invalid_result("eth1.address", "interfaces must not share an address");
    }
    const bool eth0_gateway = !config.eth0.gateway.empty();
    const bool eth1_gateway = !config.eth1.gateway.empty();
    if (eth0_gateway && eth1_gateway) {
        return invalid_result("gateway", "only one default gateway is allowed");
    }
    return valid_result();
}

std::string mode_name(Mode mode) noexcept {
    return mode == Mode::dhcp ? "dhcp" : "static";
}

std::string to_json(const NetworkConfig& config) {
    return "{\"eth0\":" + interface_json(config.eth0) +
        ",\"eth1\":" + interface_json(config.eth1) + "}";
}

}  // namespace uhf::network
