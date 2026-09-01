// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "acquisition/acquisition.hpp"
#include "config/config_store.hpp"
#include "health/health.hpp"
#include "iec61850/server.hpp"
#include "logging/logger.hpp"
#include "modbus/rtu_server.hpp"
#include "modbus/tcp_server.hpp"
#include "storage/persistence_runtime.hpp"

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>

namespace uhf::app {

struct GatewayRuntimeOptions {
    bool simulate{false};
    config::ConfigStore* config_store{nullptr};
    std::string acquisition_device{"/dev/ttyS1"};
    acquisition::AcquisitionOptions acquisition_options{};
    std::chrono::milliseconds poll_interval{std::chrono::seconds(6)};
    bool start_modbus_tcp{true};
    bool reload_modbus_tcp_endpoint{true};
    std::string modbus_tcp_bind{"127.0.0.1"};
    std::uint16_t modbus_tcp_port{502};
    std::uint8_t modbus_tcp_unit_id{1U};
    bool start_modbus_rtu{true};
    std::string modbus_rtu_device{"/dev/ttyS4"};
    modbus::ModbusRtuOptions modbus_rtu_options{};
    bool start_iec61850{true};
    bool reload_iec61850_endpoint{true};
    std::string iec61850_bind{"127.0.0.1"};
    std::uint16_t iec61850_port{102U};
    std::string iec61850_ied_name{"UHFPD1"};
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
    iec61850::RuntimeStats iec61850_stats() const noexcept;

private:
    void apply_runtime_configuration(std::uint64_t& applied_version);
    void run();

    GatewayRuntimeOptions options_;
    logging::Logger& logger_;
    std::unique_ptr<acquisition::ISerialPort> serial_port_;
    std::unique_ptr<acquisition::AcquisitionEngine> acquisition_engine_;
    std::unique_ptr<modbus::ModbusTcpServer> modbus_tcp_server_;
    std::unique_ptr<acquisition::ISerialPort> modbus_rtu_serial_port_;
    std::unique_ptr<modbus::ModbusRtuServer> modbus_rtu_server_;
    std::unique_ptr<iec61850::Server> iec61850_server_;
    std::unique_ptr<storage::PersistenceWorker> persistence_worker_;
    acquisition::SnapshotStore snapshot_store_;
    std::atomic<bool> stop_requested_{false};
    std::thread worker_;
    std::thread modbus_tcp_worker_;
    std::thread modbus_rtu_worker_;
};

}  // namespace uhf::app
