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
        *serial_port_, snapshot_store_);
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
}

void GatewayRuntime::stop() noexcept {
    stop_requested_.store(true);
    if (worker_.joinable()) {
        worker_.join();
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
