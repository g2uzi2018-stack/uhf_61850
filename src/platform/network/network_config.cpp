// SPDX-License-Identifier: GPL-3.0-only
#include "platform/network/network_config.hpp"

#include <arpa/inet.h>
#include <charconv>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>

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
    result += "],\"hostname\":\"";
    result += json_escape(config.hostname);
    result += "\",\"dhcp_timeout_seconds\":" +
        std::to_string(config.dhcp_timeout_seconds) + "}";
    return result;
}

class FlatJsonParser {
public:
    explicit FlatJsonParser(std::string_view input) : input_(input) {
        eth0_.name = "eth0";
        eth1_.name = "eth1";
    }

    bool parse(uhf::network::NetworkConfig& config) {
        skip_space();
        if (!consume('{')) {
            return false;
        }
        skip_space();
        if (consume('}')) {
            return false;
        }
        while (position_ < input_.size()) {
            std::string key;
            if (!parse_string(key)) {
                return false;
            }
            skip_space();
            if (!consume(':') || !parse_field(key, config)) {
                return false;
            }
            skip_space();
            if (consume('}')) {
                skip_space();
                return position_ == input_.size() && seen_.size() == kFieldCount &&
                    apply_string_values(config);
            }
            if (!consume(',')) {
                return false;
            }
            skip_space();
        }
        return false;
    }

private:
    static constexpr std::size_t kFieldCount = 16U;

    void skip_space() noexcept {
        while (position_ < input_.size()) {
            const unsigned char character = static_cast<unsigned char>(input_[position_]);
            if (character != ' ' && character != '\t' && character != '\r' && character != '\n') {
                break;
            }
            ++position_;
        }
    }

    bool consume(char expected) noexcept {
        if (position_ >= input_.size() || input_[position_] != expected) {
            return false;
        }
        ++position_;
        skip_space();
        return true;
    }

    bool parse_string(std::string& result) {
        if (position_ >= input_.size() || input_[position_++] != '"') {
            return false;
        }
        result.clear();
        while (position_ < input_.size()) {
            const char character = input_[position_++];
            if (character == '"') {
                return result.size() <= 256U;
            }
            if (static_cast<unsigned char>(character) < 0x20U) {
                return false;
            }
            if (character != '\\') {
                result.push_back(character);
                continue;
            }
            if (position_ >= input_.size()) {
                return false;
            }
            const char escaped = input_[position_++];
            if (escaped != '"' && escaped != '\\') {
                return false;
            }
            result.push_back(escaped);
        }
        return false;
    }

    bool parse_unsigned(std::uint64_t& value) {
        const std::size_t begin = position_;
        while (position_ < input_.size() && input_[position_] >= '0' && input_[position_] <= '9') {
            ++position_;
        }
        if (begin == position_) {
            return false;
        }
        const auto result = std::from_chars(
            input_.data() + begin, input_.data() + position_, value);
        return result.ec == std::errc{} && result.ptr == input_.data() + position_;
    }

    template <typename Integer>
    static bool assign_unsigned(
        std::uint64_t value, Integer minimum, Integer maximum, Integer& output) {
        if (value < static_cast<std::uint64_t>(minimum) ||
            value > static_cast<std::uint64_t>(maximum)) {
            return false;
        }
        output = static_cast<Integer>(value);
        return true;
    }

    uhf::network::InterfaceConfig* interface_for(
        std::string_view key, std::string_view suffix) {
        if (key.size() == suffix.size() + 5U && key.compare(0U, 5U, "eth0_") == 0 &&
            key.compare(5U, suffix.size(), suffix) == 0) {
            return &eth0_;
        }
        if (key.size() == suffix.size() + 5U && key.compare(0U, 5U, "eth1_") == 0 &&
            key.compare(5U, suffix.size(), suffix) == 0) {
            return &eth1_;
        }
        return nullptr;
    }

    std::string* string_field(const std::string& key, std::string_view suffix) {
        if (uhf::network::InterfaceConfig* config = interface_for(key, suffix); config != nullptr) {
            if (suffix == "mode") {
                return &mode_storage_[config == &eth0_ ? 0U : 1U];
            }
            if (suffix == "address") {
                return &config->address;
            }
            if (suffix == "gateway") {
                return &config->gateway;
            }
            if (suffix == "hostname") {
                return &config->hostname;
            }
            if (suffix == "dns1") {
                return &dns_storage_[config == &eth0_ ? 0U : 1U][0U];
            }
            if (suffix == "dns2") {
                return &dns_storage_[config == &eth0_ ? 0U : 1U][1U];
            }
        }
        return nullptr;
    }

    bool parse_field(const std::string& key, uhf::network::NetworkConfig& config) {
        static_cast<void>(config);
        if (!seen_.insert(key).second) {
            return false;
        }
        std::string* string_output = nullptr;
        for (const std::string_view suffix : {"mode", "address", "gateway", "hostname", "dns1", "dns2"}) {
            string_output = string_field(key, suffix);
            if (string_output != nullptr) {
                break;
            }
        }
        if (string_output != nullptr) {
            if (!parse_string(*string_output)) {
                return false;
            }
            return true;
        }
        std::uint64_t value = 0U;
        if (!parse_unsigned(value)) {
            return false;
        }
        for (const std::string_view suffix : {"prefix", "dhcp_timeout_seconds"}) {
            if (interface_for(key, suffix) == nullptr) {
                continue;
            }
            uhf::network::InterfaceConfig* output = interface_for(key, suffix);
            if (suffix == "prefix") {
                return assign_unsigned(value, std::uint8_t{1U}, std::uint8_t{32U}, output->prefix);
            }
            return assign_unsigned(
                value, std::uint16_t{1U}, std::numeric_limits<std::uint16_t>::max(),
                output->dhcp_timeout_seconds);
        }
        return false;
    }

    bool apply_string_values(uhf::network::NetworkConfig& config) {
        config.eth0 = eth0_;
        config.eth1 = eth1_;
        config.eth0.name = "eth0";
        config.eth1.name = "eth1";
        config.eth0.mode = mode_storage_[0U] == "dhcp"
            ? uhf::network::Mode::dhcp
            : mode_storage_[0U] == "static" ? uhf::network::Mode::static_address :
                                                 uhf::network::Mode::static_address;
        config.eth1.mode = mode_storage_[1U] == "dhcp"
            ? uhf::network::Mode::dhcp
            : mode_storage_[1U] == "static" ? uhf::network::Mode::static_address :
                                                 uhf::network::Mode::static_address;
        if (mode_storage_[0U] != "dhcp" && mode_storage_[0U] != "static") {
            return false;
        }
        if (mode_storage_[1U] != "dhcp" && mode_storage_[1U] != "static") {
            return false;
        }
        config.eth0.dns_count = 0U;
        config.eth1.dns_count = 0U;
        for (std::size_t interface = 0U; interface < 2U; ++interface) {
            uhf::network::InterfaceConfig* output = interface == 0U ? &config.eth0 : &config.eth1;
            for (std::size_t index = 0U; index < uhf::network::kMaxDnsServers; ++index) {
                if (!dns_storage_[interface][index].empty()) {
                    output->dns[output->dns_count++] = dns_storage_[interface][index];
                }
            }
        }
        return uhf::network::validate(config).valid;
    }

    std::string_view input_;
    std::size_t position_{0U};
    std::unordered_set<std::string> seen_;
    uhf::network::InterfaceConfig eth0_{};
    uhf::network::InterfaceConfig eth1_{};
    std::array<std::string, 2U> mode_storage_{};
    std::array<std::array<std::string, 2U>, 2U> dns_storage_{};
};

std::string flat_interface_fields(
    const uhf::network::InterfaceConfig& config, std::string_view prefix) {
    const std::string first_dns = config.dns_count > 0U ? config.dns[0U] : "";
    const std::string second_dns = config.dns_count > 1U ? config.dns[1U] : "";
    return "\"" + std::string(prefix) + "mode\":\"" + uhf::network::mode_name(config.mode) +
        "\",\"" + std::string(prefix) + "address\":\"" + json_escape(config.address) +
        "\",\"" + std::string(prefix) + "prefix\":" + std::to_string(config.prefix) +
        ",\"" + std::string(prefix) + "gateway\":\"" + json_escape(config.gateway) +
        "\",\"" + std::string(prefix) + "dns1\":\"" + json_escape(first_dns) +
        "\",\"" + std::string(prefix) + "dns2\":\"" + json_escape(second_dns) +
        "\",\"" + std::string(prefix) + "hostname\":\"" + json_escape(config.hostname) +
        "\",\"" + std::string(prefix) + "dhcp_timeout_seconds\":" +
        std::to_string(config.dhcp_timeout_seconds);
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

bool parse_flat_json(std::string_view json, NetworkConfig& config) {
    if (json.size() > 16U * 1024U) {
        return false;
    }
    NetworkConfig candidate;
    FlatJsonParser parser(json);
    if (!parser.parse(candidate)) {
        return false;
    }
    config = candidate;
    return true;
}

std::string to_flat_json(const NetworkConfig& config) {
    return "{" + flat_interface_fields(config.eth0, "eth0_") + "," +
        flat_interface_fields(config.eth1, "eth1_") + "}";
}

std::string to_json(const NetworkConfig& config) {
    return "{\"eth0\":" + interface_json(config.eth0) +
        ",\"eth1\":" + interface_json(config.eth1) + "}";
}

}  // namespace uhf::network
