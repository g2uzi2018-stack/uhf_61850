// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "acquisition/acquisition.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace uhf::modbus {

struct ModbusRtuOptions {
    std::uint8_t unit_id{1U};
    std::size_t max_frame_bytes{512U};
};

class ModbusRtuServer {
public:
    ModbusRtuServer(
        acquisition::ISerialPort& serial_port,
        acquisition::SnapshotStore& snapshot_store,
        ModbusRtuOptions options = {});

    int run();
    void stop() noexcept;

    std::vector<std::uint8_t> handle_request(const std::vector<std::uint8_t>& request) const;

private:
    acquisition::ISerialPort& serial_port_;
    acquisition::SnapshotStore& snapshot_store_;
    ModbusRtuOptions options_;
    std::atomic<bool> stop_requested_{false};
};

}  // namespace uhf::modbus
