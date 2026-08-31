// SPDX-License-Identifier: GPL-3.0-only
#include "platform/network/linux_status.hpp"

#include <cassert>

int main() {
    uhf::network::LinuxStatusReader reader;
    uhf::network::NetworkStatus status;
    assert(reader.read(status));
    assert(status.eth0.name == "eth0");
    assert(status.eth1.name == "eth1");
    if (!status.eth0.address.empty()) {
        assert(status.eth0.prefix <= 32U);
    }
    if (!status.eth1.address.empty()) {
        assert(status.eth1.prefix <= 32U);
    }
    return 0;
}
