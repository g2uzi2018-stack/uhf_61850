// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "acquisition/acquisition.hpp"
#include "health/health.hpp"
#include "logging/logger.hpp"
#include "modbus/rtu_server.hpp"
#include "modbus/tcp_server.hpp"
#include "storage/persistence_runtime.hpp"

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace uhf::app {

struct GatewayRuntimeOptions {
    bool simulate{false};
    std::string acquisition_device{"/dev/ttyS1"};
    std::chrono::seconds poll_interval{6};
    bool start_modbus_tcp{true};
    std::string modbus_tcp_bind{"127.0.0.1"};
    std::uint16_t modbus_tcp_port{502};
    bool start_modbus_rtu{true};
    std::string modbus_rtu_device{"/dev/ttyS4"};
    bool start_persistence{true};
    storage::PersistenceOptions persistence_options{};
};

class GatewayRuntime {
public:
    GatewayRuntime(GatewayRuntimeOptions options, logging::Logger& logger);
    ~GatewayRuntime();

    GatewayRuntime(const GatewayRuntime&) = delete;
    GatewayRuntime& operator=(const GatewayRuntime&) = delete;

    void start();
    void stop() noexcept;

    acquisition::SnapshotStore& snapshot_store() noexcept;
    health::Input health_input() const;

private:
    void run();

    GatewayRuntimeOptions options_;
    logging::Logger& logger_;
    std::unique_ptr<acquisition::ISerialPort> serial_port_;
    std::unique_ptr<acquisition::AcquisitionEngine> acquisition_engine_;
    std::unique_ptr<modbus::ModbusTcpServer> modbus_tcp_server_;
    std::unique_ptr<acquisition::ISerialPort> modbus_rtu_serial_port_;
    std::unique_ptr<modbus::ModbusRtuServer> modbus_rtu_server_;
    std::unique_ptr<storage::PersistenceWorker> persistence_worker_;
    acquisition::SnapshotStore snapshot_store_;
    std::atomic<bool> stop_requested_{false};
    std::thread worker_;
    std::thread modbus_tcp_worker_;
    std::thread modbus_rtu_worker_;
    mutable std::mutex status_mutex_;
    bool last_cycle_ok_{false};
    std::optional<std::chrono::steady_clock::time_point> last_success_;
};

}  // namespace uhf::app
