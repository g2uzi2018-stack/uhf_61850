// SPDX-License-Identifier: GPL-3.0-only
#include "iec61850/server.hpp"

#include "domain/snapshot.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <thread>
#include <utility>

namespace {

std::uint64_t now_milliseconds() noexcept {
    const auto duration = std::chrono::system_clock::now().time_since_epoch();
    const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(duration);
    return static_cast<std::uint64_t>(milliseconds.count());
}

Quality quality_for(bool valid) noexcept {
    return valid ? static_cast<Quality>(QUALITY_VALIDITY_GOOD)
                 : static_cast<Quality>(QUALITY_VALIDITY_INVALID);
}

}  // namespace

namespace uhf::iec61850 {

Server::Server(acquisition::SnapshotStore& snapshot_store, ServerOptions options)
    : snapshot_store_(snapshot_store),
      options_(std::move(options)),
      model_(std::make_unique<Model>(options_.ied_name)),
      alarm_provider_(options_.alarm_provider) {
    server_ = IedServer_create(model_->raw());
    if (server_ == nullptr) {
        throw std::runtime_error("unable to create IEC 61850 MMS server");
    }
    IedServer_setLocalIpAddress(server_, options_.bind_address.c_str());
}

Server::~Server() {
    stop();
    if (server_ != nullptr) {
        IedServer_destroy(server_);
        server_ = nullptr;
    }
}

void Server::start() {
    if (running_.load()) {
        return;
    }
    stop_requested_.store(false);
    publish_invalid_values();
    IedServer_start(server_, static_cast<int>(options_.port));
    if (!IedServer_isRunning(server_)) {
        throw std::runtime_error("unable to bind IEC 61850 MMS server");
    }
    running_.store(true);
    update_worker_ = std::thread(&Server::update_loop, this);
}

void Server::stop() noexcept {
    stop_requested_.store(true);
    if (update_worker_.joinable()) {
        update_worker_.join();
    }
    if (server_ != nullptr && IedServer_isRunning(server_)) {
        IedServer_stop(server_);
    }
    running_.store(false);
}

bool Server::running() const noexcept {
    return running_.load();
}

void Server::update_timestamp(DataAttribute* attribute, std::uint64_t timestamp_ms) {
    IedServer_updateUTCTimeAttributeValue(server_, attribute, timestamp_ms);
}

void Server::publish_invalid_values() {
    const std::uint64_t timestamp_ms = now_milliseconds();
    IedServer_lockDataModel(server_);
    for (std::size_t index = 0; index < kMeasurementCount; ++index) {
        if (model_->measurement_integer(index)) {
            IedServer_updateInt32AttributeValue(server_, model_->measurement_value(index), 0);
        } else {
            IedServer_updateFloatAttributeValue(server_, model_->measurement_value(index), 0.0F);
        }
        IedServer_updateQuality(
            server_, model_->measurement_quality(index), quality_for(false));
        update_timestamp(model_->measurement_time(index), timestamp_ms);
    }
    IedServer_updateFloatAttributeValue(server_, model_->peak_value(), 0.0F);
    IedServer_updateBooleanAttributeValue(server_, model_->alarm_value(), false);
    IedServer_updateQuality(server_, model_->alarm_quality(), quality_for(false));
    update_timestamp(model_->alarm_time(), timestamp_ms);
    IedServer_unlockDataModel(server_);
}

void Server::publish_snapshot(const acquisition::PublishedSnapshot& snapshot) {
    const std::uint64_t timestamp_ms = now_milliseconds();
    const domain::ParsedSnapshot& payload = snapshot.payload;
    IedServer_lockDataModel(server_);
    for (std::size_t index = 0; index < kMeasurementCount; ++index) {
        const domain::Measurement& measurement = payload.measurements[index];
        if (model_->measurement_integer(index)) {
            IedServer_updateInt32AttributeValue(
                server_, model_->measurement_value(index),
                static_cast<std::int32_t>(measurement.value));
        } else {
            IedServer_updateFloatAttributeValue(
                server_, model_->measurement_value(index), static_cast<float>(measurement.value));
        }
        IedServer_updateQuality(
            server_, model_->measurement_quality(index), quality_for(measurement.valid));
        update_timestamp(model_->measurement_time(index), timestamp_ms);
    }

    const domain::Measurement& peak = payload.measurements[2U];
    const bool alarm_valid = peak.valid && payload.payload_status != domain::PayloadStatus::not_refreshed;
    const bool alarm = alarm_valid &&
        (alarm_provider_ ? alarm_provider_() : peak.value >= -45);
    IedServer_updateFloatAttributeValue(
        server_, model_->peak_value(), static_cast<float>(peak.value));
    IedServer_updateBooleanAttributeValue(server_, model_->alarm_value(), alarm);
    IedServer_updateQuality(server_, model_->alarm_quality(), quality_for(alarm_valid));
    update_timestamp(model_->alarm_time(), timestamp_ms);
    IedServer_unlockDataModel(server_);
}

void Server::update_loop() {
    std::uint64_t last_generation = 0U;
    while (!stop_requested_.load()) {
        const std::optional<acquisition::PublishedSnapshot> latest = snapshot_store_.latest();
        if (latest && latest->generation > last_generation) {
            publish_snapshot(*latest);
            last_generation = latest->generation;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

}  // namespace uhf::iec61850
