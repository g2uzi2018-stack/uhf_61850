// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "acquisition/acquisition.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <utility>
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
    bool update_options(ModbusTcpOptions options);
    void stop() noexcept;
    std::uint16_t bound_port() const noexcept;

    std::vector<std::uint8_t> handle_request(const std::vector<std::uint8_t>& request) const;

private:
    std::pair<ModbusTcpOptions, std::uint64_t> configuration() const;

    acquisition::SnapshotStore& snapshot_store_;
    mutable std::mutex options_mutex_;
    ModbusTcpOptions options_;
    std::uint64_t options_generation_{0U};
    std::atomic<bool> stop_requested_{false};
    std::atomic<std::uint16_t> bound_port_{0};
};

}  // namespace uhf::modbus
