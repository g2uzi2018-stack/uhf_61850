// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "acquisition/acquisition.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace uhf::modbus {

struct ModbusTcpOptions {
    std::string bind_address{"127.0.0.1"};
    std::uint16_t port{502};
    std::uint8_t unit_id{1U};
    std::size_t max_connections{16U};
};

class ModbusTcpServer {
public:
    ModbusTcpServer(acquisition::SnapshotStore& snapshot_store, ModbusTcpOptions options = {});

    int run();
    void stop() noexcept;
    std::uint16_t bound_port() const noexcept;

    std::vector<std::uint8_t> handle_request(const std::vector<std::uint8_t>& request) const;

private:
    acquisition::SnapshotStore& snapshot_store_;
    ModbusTcpOptions options_;
    std::atomic<bool> stop_requested_{false};
    std::atomic<std::uint16_t> bound_port_{0};
};

}  // namespace uhf::modbus
