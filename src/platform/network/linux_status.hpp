// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <cstdint>
#include <string>

namespace uhf::network {

struct InterfaceStatus {
    std::string name;
    bool exists{false};
    bool carrier_known{false};
    bool link_up{false};
    std::string address;
    std::uint8_t prefix{0U};
};

struct NetworkStatus {
    InterfaceStatus eth0{"eth0", false, false, false, {}, 0U};
    InterfaceStatus eth1{"eth1", false, false, false, {}, 0U};
};

class StatusReader {
public:
    virtual ~StatusReader() = default;
    virtual bool read(NetworkStatus& status) const = 0;
};

class LinuxStatusReader final : public StatusReader {
public:
    bool read(NetworkStatus& status) const override;
};

}  // namespace uhf::network
