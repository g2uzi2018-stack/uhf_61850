// SPDX-License-Identifier: GPL-3.0-only
#include "platform/network/linux_status.hpp"
#include "test_check.hpp"

int main() {
    uhf::network::LinuxStatusReader reader;
    uhf::network::NetworkStatus status;
    UHF_TEST_CHECK(reader.read(status));
    UHF_TEST_CHECK(status.eth0.name == "eth0");
    UHF_TEST_CHECK(status.eth1.name == "eth1");
    if (!status.eth0.address.empty()) {
        UHF_TEST_CHECK(status.eth0.prefix <= 32U);
    }
    if (!status.eth1.address.empty()) {
        UHF_TEST_CHECK(status.eth1.prefix <= 32U);
    }
    return 0;
}
