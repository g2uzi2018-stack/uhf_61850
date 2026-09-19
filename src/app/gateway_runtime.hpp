// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "acquisition/acquisition.hpp"
#include "activation/activation_manager.hpp"
#include "config/config_store.hpp"
#include "health/health.hpp"
#include "iec61850/server.hpp"
#include "logging/logger.hpp"
#include "modbus/rtu_server.hpp"
#include "modbus/tcp_server.hpp"
#include "storage/persistence_runtime.hpp"
#include "v3/acquisition.hpp"

#include <atomic>
#include <chrono>
#include <memory>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace uhf::app {

struct GatewayRuntimeOptions {
    bool simulate{false};
    bool v3_enabled{false};
    activation::Options activation_options{};
    config::ConfigStore* config_store{nullptr};
    std::string acquisition_device{"/dev/ttyS1"};
    std::string v3_pd_device{"/dev/ttyS1"};
    std::string v3_current_device{"/dev/ttyS2"};
    std::string v3_temperature_device{"/dev/ttyS3"};
    v3::SchedulerOptions v3_scheduler_options{};
    acquisition::AcquisitionOptions acquisition_options{};
    std::chrono::milliseconds poll_interval{std::chrono::seconds(6)};
    bool start_modbus_tcp{true};
    bool reload_modbus_tcp_endpoint{true};
    bool modbus_tcp_bind_all{false};
    std::string modbus_tcp_bind{"127.0.0.1"};
    std::uint16_t modbus_tcp_port{502};
    std::uint8_t modbus_tcp_unit_id{1U};
    bool start_modbus_rtu{true};
    std::string modbus_rtu_device{"/dev/ttyS4"};
    modbus::ModbusRtuOptions modbus_rtu_options{};
    bool start_iec61850{true};
    bool reload_iec61850_endpoint{true};
    bool iec61850_bind_all{false};
    std::string iec61850_bind{"127.0.0.1"};
    std::uint16_t iec61850_port{102U};
    std::string iec61850_ied_name{"UHFPD1"};
    std::filesystem::path iec61850_icd_override{"/var/lib/uhf-gateway/UHFPD1.icd"};
    std::filesystem::path iec61850_icd_packaged{"/etc/uhf-gateway/UHFPD1.icd"};
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
    const v3::SnapshotStore* v3_snapshot_store() const noexcept;
    health::Input health_input() const;
    iec61850::RuntimeStats iec61850_stats() const noexcept;
    std::optional<iec61850::RuntimeEndpoint> iec61850_endpoint() const;
    std::optional<iec61850::SclModelDefinition> iec61850_model_definition() const;
    bool reload_iec61850_model();

private:
    void apply_runtime_configuration(std::uint64_t& applied_version);
    void run();
    std::optional<iec61850::SclModelDefinition> load_iec61850_definition() const;

    GatewayRuntimeOptions options_;
    logging::Logger& logger_;
    std::unique_ptr<acquisition::ISerialPort> serial_port_;
    std::unique_ptr<acquisition::AcquisitionEngine> acquisition_engine_;
    std::unique_ptr<acquisition::ISerialPort> v3_pd_serial_port_;
    std::unique_ptr<acquisition::ISerialPort> v3_current_serial_port_;
    std::unique_ptr<acquisition::ISerialPort> v3_temperature_serial_port_;
    std::unique_ptr<v3::ISerialPort> v3_pd_adapter_;
    std::unique_ptr<v3::ISerialPort> v3_current_adapter_;
    std::unique_ptr<v3::ISerialPort> v3_temperature_adapter_;
    std::unique_ptr<v3::SnapshotStore> v3_snapshot_store_;
    std::unique_ptr<activation::Manager> activation_manager_;
    std::unique_ptr<v3::AcquisitionScheduler> v3_scheduler_;
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
    mutable std::mutex iec_mutex_;
    std::string iec_current_bind_;
    std::uint16_t iec_current_port_{0U};
};

}  // namespace uhf::app
