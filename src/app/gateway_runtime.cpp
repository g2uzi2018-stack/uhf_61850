// SPDX-License-Identifier: GPL-3.0-only
#include "app/gateway_runtime.hpp"

#include "acquisition/loopback_pd1000.hpp"

#include <algorithm>
#include <chrono>
#include <string>
#include <utility>

namespace uhf::app {

GatewayRuntime::GatewayRuntime(GatewayRuntimeOptions options, logging::Logger& logger)
    : options_(std::move(options)), logger_(logger) {
    if (options_.simulate) {
        serial_port_ = std::make_unique<acquisition::LoopbackPd1000Port>();
    } else {
        serial_port_ = std::make_unique<acquisition::PosixSerialPort>(options_.acquisition_device);
    }
    acquisition_engine_ = std::make_unique<acquisition::AcquisitionEngine>(
        *serial_port_, snapshot_store_, options_.acquisition_options);
    if (options_.start_modbus_tcp) {
        modbus_tcp_server_ = std::make_unique<modbus::ModbusTcpServer>(
            snapshot_store_,
            modbus::ModbusTcpOptions{
                options_.modbus_tcp_bind,
                options_.modbus_tcp_port,
                options_.modbus_tcp_unit_id,
                16U});
    }
    if (options_.start_modbus_rtu) {
        modbus_rtu_serial_port_ =
            std::make_unique<acquisition::PosixSerialPort>(options_.modbus_rtu_device);
        modbus_rtu_server_ = std::make_unique<modbus::ModbusRtuServer>(
            *modbus_rtu_serial_port_, snapshot_store_, options_.modbus_rtu_options);
    }
    if (options_.start_iec61850) {
        iec61850_server_ = std::make_unique<iec61850::Server>(
            snapshot_store_,
            iec61850::ServerOptions{
                options_.iec61850_bind,
                options_.iec61850_port,
                options_.iec61850_ied_name});
    }
    if (options_.start_persistence) {
        persistence_worker_ = std::make_unique<storage::PersistenceWorker>(
            snapshot_store_, logger_, options_.persistence_options);
    }
}

GatewayRuntime::~GatewayRuntime() {
    stop();
}

void GatewayRuntime::start() {
    if (worker_.joinable()) {
        return;
    }
    stop_requested_.store(false);
    worker_ = std::thread(&GatewayRuntime::run, this);
    if (modbus_tcp_server_) {
        modbus_tcp_worker_ = std::thread([this] {
            const int result = modbus_tcp_server_->run();
            if (result != 0 && !stop_requested_.load()) {
                logger_.log(
                    logging::Level::error,
                    logging::Component::modbus_tcp,
                    "server.failed",
                    "Modbus TCP server stopped unexpectedly");
            }
        });
    }
    if (modbus_rtu_server_) {
        modbus_rtu_worker_ = std::thread([this] {
            const int result = modbus_rtu_server_->run();
            if (result != 0 && !stop_requested_.load()) {
                logger_.log(
                    logging::Level::error,
                    logging::Component::modbus_rtu,
                    "server.failed",
                    "Modbus RTU server stopped unexpectedly");
            }
        });
    }
    if (iec61850_server_) {
        iec61850_server_->start();
    }
    if (persistence_worker_) {
        persistence_worker_->start();
    }
}

void GatewayRuntime::stop() noexcept {
    stop_requested_.store(true);
    if (modbus_tcp_server_) {
        modbus_tcp_server_->stop();
    }
    if (modbus_tcp_worker_.joinable()) {
        modbus_tcp_worker_.join();
    }
    if (modbus_rtu_server_) {
        modbus_rtu_server_->stop();
    }
    if (modbus_rtu_worker_.joinable()) {
        modbus_rtu_worker_.join();
    }
    if (iec61850_server_) {
        iec61850_server_->stop();
    }
    if (worker_.joinable()) {
        worker_.join();
    }
    if (persistence_worker_) {
        persistence_worker_->stop();
    }
}

acquisition::SnapshotStore& GatewayRuntime::snapshot_store() noexcept {
    return snapshot_store_;
}

health::Input GatewayRuntime::health_input() const {
    health::Input input;
    {
        std::lock_guard<std::mutex> lock(status_mutex_);
        input.last_acquisition_success = last_success_;
        input.acquisition_last_cycle_ok = last_cycle_ok_;
    }
    input.storage_writable = true;
    input.modbus_tcp_listening =
        modbus_tcp_server_ != nullptr && modbus_tcp_server_->bound_port() != 0U;
    input.modbus_rtu_ready = modbus_rtu_server_ != nullptr;
    input.iec61850_enabled =
        iec61850_server_ != nullptr && iec61850_server_->running();
    if (persistence_worker_) {
        const storage::PersistenceStats stats = persistence_worker_->stats();
        input.storage_writable = !stats.writes_paused && !stats.cleanup_failed;
        input.storage_low_watermark = stats.low_watermark_active;
    }
    return input;
}

void GatewayRuntime::run() {
    bool previous_cycle_failed = false;
    std::chrono::steady_clock::time_point next_poll = std::chrono::steady_clock::now();
    while (!stop_requested_.load()) {
        const bool success = acquisition_engine_->poll_once();
        const auto completed_at = std::chrono::steady_clock::now();
        {
            std::lock_guard<std::mutex> lock(status_mutex_);
            last_cycle_ok_ = success;
            if (success) {
                last_success_ = completed_at;
            }
        }
        if (success) {
            if (previous_cycle_failed) {
                logger_.recovered(
                    logging::Component::acquisition,
                    "poll.failed",
                    "PD1000 polling recovered");
            }
            previous_cycle_failed = false;
        } else {
            logger_.log(
                logging::Level::error,
                logging::Component::acquisition,
                "poll.failed",
                acquisition_engine_->last_error());
            previous_cycle_failed = true;
        }

        next_poll += options_.poll_interval;
        const auto now = std::chrono::steady_clock::now();
        if (next_poll <= now) {
            next_poll = now + options_.poll_interval;
        }
        while (!stop_requested_.load()) {
            const auto remaining = next_poll - std::chrono::steady_clock::now();
            if (remaining <= std::chrono::steady_clock::duration::zero()) {
                break;
            }
            std::this_thread::sleep_for(std::min(
                remaining,
                std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                    std::chrono::milliseconds(100))));
        }
    }
}

}  // namespace uhf::app
