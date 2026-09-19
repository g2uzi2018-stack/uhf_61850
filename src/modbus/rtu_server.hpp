// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "acquisition/acquisition.hpp"
#include "v3/acquisition.hpp"

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
        ModbusRtuOptions options = {},
        const v3::SnapshotStore* v3_snapshot_store = nullptr);

    int run();
    void stop() noexcept;

    std::vector<std::uint8_t> handle_request(const std::vector<std::uint8_t>& request) const;

private:
    acquisition::ISerialPort& serial_port_;
    acquisition::SnapshotStore& snapshot_store_;
    const v3::SnapshotStore* v3_snapshot_store_{nullptr};
    ModbusRtuOptions options_;
    std::atomic<bool> stop_requested_{false};
};

}  // namespace uhf::modbus
