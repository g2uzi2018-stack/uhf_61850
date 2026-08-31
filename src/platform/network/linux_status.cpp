// SPDX-License-Identifier: GPL-3.0-only
#include "platform/network/linux_status.hpp"

#include <arpa/inet.h>
#include <cstdint>
#include <fstream>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>

namespace {

std::uint8_t prefix_from_netmask(const sockaddr* netmask) noexcept {
    if (netmask == nullptr || netmask->sa_family != AF_INET) {
        return 0U;
    }
    const auto* address = reinterpret_cast<const sockaddr_in*>(netmask);
    std::uint32_t mask = ntohl(address->sin_addr.s_addr);
    std::uint8_t prefix = 0U;
    bool zero_seen = false;
    while (mask != 0U) {
        const bool bit = (mask & 0x80000000U) != 0U;
        if (!bit) {
            zero_seen = true;
        } else if (zero_seen) {
            return 0U;
        } else {
            ++prefix;
        }
        mask <<= 1U;
    }
    return prefix;
}

void read_carrier(uhf::network::InterfaceStatus& status) {
    const std::string path = "/sys/class/net/" + status.name + "/carrier";
    std::ifstream input(path);
    int value = -1;
    if (input >> value && (value == 0 || value == 1)) {
        status.carrier_known = true;
        status.link_up = value == 1;
    }
}

void update_interface(
    uhf::network::InterfaceStatus& status, const ifaddrs* current) {
    if (current == nullptr || current->ifa_name == nullptr ||
        status.name != current->ifa_name) {
        return;
    }
    status.exists = true;
    if ((current->ifa_flags & IFF_RUNNING) != 0U && !status.carrier_known) {
        status.link_up = true;
    }
    if (current->ifa_addr == nullptr || current->ifa_addr->sa_family != AF_INET ||
        !status.address.empty()) {
        return;
    }
    const auto* address = reinterpret_cast<const sockaddr_in*>(current->ifa_addr);
    char text[INET_ADDRSTRLEN]{};
    if (::inet_ntop(AF_INET, &address->sin_addr, text, sizeof(text)) != nullptr) {
        status.address = text;
        status.prefix = prefix_from_netmask(current->ifa_netmask);
    }
}

}  // namespace

namespace uhf::network {

bool LinuxStatusReader::read(NetworkStatus& status) const {
    status = NetworkStatus{};
    ifaddrs* addresses = nullptr;
    if (::getifaddrs(&addresses) != 0) {
        return false;
    }
    for (const ifaddrs* current = addresses; current != nullptr; current = current->ifa_next) {
        update_interface(status.eth0, current);
        update_interface(status.eth1, current);
    }
    ::freeifaddrs(addresses);
    read_carrier(status.eth0);
    read_carrier(status.eth1);
    return true;
}

}  // namespace uhf::network
