// SPDX-License-Identifier: GPL-3.0-only
#include "iec61850/server.hpp"

#include "domain/snapshot.hpp"

extern "C" {
#include "mms_server_libinternal.h"
}

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

Quality quality_for(uhf::acquisition::Availability availability, bool valid) noexcept {
    if (availability == uhf::acquisition::Availability::invalid || !valid) {
        return static_cast<Quality>(QUALITY_VALIDITY_INVALID);
    }
    if (availability == uhf::acquisition::Availability::stale) {
        return static_cast<Quality>(QUALITY_VALIDITY_QUESTIONABLE | QUALITY_DETAIL_OLD_DATA);
    }
    return static_cast<Quality>(QUALITY_VALIDITY_GOOD);
}

}  // namespace

namespace uhf::iec61850 {

Server::Server(acquisition::SnapshotStore& snapshot_store, ServerOptions options)
    : snapshot_store_(snapshot_store),
      options_(std::move(options)),
      model_(std::make_unique<Model>(options_.ied_name)),
      alarm_provider_(options_.alarm_provider) {
    IedServerConfig server_config = IedServerConfig_create();
    if (server_config == nullptr) {
        throw std::runtime_error("unable to create IEC 61850 server configuration");
    }
    IedServerConfig_setMaxMmsConnections(server_config, 4);
    IedServerConfig_setReportBufferSize(server_config, 65536);
    IedServerConfig_setReportBufferSizeForURCBs(server_config, 65536);
    IedServerConfig_enableFileService(server_config, false);
    IedServerConfig_enableDynamicDataSetService(server_config, false);
    IedServerConfig_enableLogService(server_config, false);
    IedServerConfig_setMaxAssociationSpecificDataSets(server_config, 0);
    IedServerConfig_setMaxDomainSpecificDataSets(server_config, 0);
    IedServerConfig_setMaxDataSetEntries(server_config, 0);
    IedServerConfig_enableEditSG(server_config, false);
    IedServerConfig_enableResvTmsForBRCB(server_config, false);
    IedServerConfig_enableOwnerForRCB(server_config, false);
    IedServerConfig_useIntegratedGoosePublisher(server_config, false);
    IedServerConfig_setReportSetting(server_config, IEC61850_REPORTSETTINGS_RPT_ID, false);
    IedServerConfig_setReportSetting(server_config, IEC61850_REPORTSETTINGS_BUF_TIME, false);
    IedServerConfig_setReportSetting(server_config, IEC61850_REPORTSETTINGS_DATSET, false);
    IedServerConfig_setReportSetting(server_config, IEC61850_REPORTSETTINGS_OPT_FIELDS, false);
    IedServerConfig_setReportSetting(server_config, IEC61850_REPORTSETTINGS_INTG_PD, false);
    server_ = IedServer_createWithConfig(model_->raw(), nullptr, server_config);
    IedServerConfig_destroy(server_config);
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
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    start_locked();
}

void Server::start_locked() {
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
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    stop_locked();
}

void Server::stop_locked() noexcept {
    stop_requested_.store(true);
    if (update_worker_.joinable()) {
        update_worker_.join();
    }
    if (server_ != nullptr && IedServer_isRunning(server_)) {
        IedServer_stop(server_);
    }
    running_.store(false);
}

bool Server::update_endpoint(std::string bind_address, std::uint16_t port) {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    if (options_.bind_address == bind_address && options_.port == port) {
        return true;
    }
    const ServerOptions previous_options = options_;
    const bool was_running = running_.load();
    if (was_running) {
        stop_locked();
    }
    options_.bind_address = std::move(bind_address);
    options_.port = port;
    IedServer_setLocalIpAddress(server_, options_.bind_address.c_str());
    if (!was_running) {
        return true;
    }
    try {
        start_locked();
        return true;
    } catch (...) {
        options_ = previous_options;
        IedServer_setLocalIpAddress(server_, options_.bind_address.c_str());
        try {
            start_locked();
        } catch (...) {
        }
        return false;
    }
}

bool Server::running() const noexcept {
    return running_.load();
}

RuntimeStats Server::stats() const noexcept {
    RuntimeStats result;
    if (server_ != nullptr) {
        const MmsServer mms_server = IedServer_getMmsServer(server_);
        result.active_connections = static_cast<std::uint32_t>(
            MmsServer_getConnectionCounter(mms_server));
        result.connection_rejections = static_cast<std::uint64_t>(
            MmsServer_getConnectionLimitRejectCount(mms_server));
        result.malformed_pdu_rejections = static_cast<std::uint64_t>(
            MmsServer_getMalformedPduRejectCount(mms_server));
        result.oversized_pdu_rejections = static_cast<std::uint64_t>(
            MmsServer_getOversizedPduRejectCount(mms_server));
        result.request_element_rejections = static_cast<std::uint64_t>(
            MmsServer_getRequestElementRejectCount(mms_server));
        result.ber_depth_rejections = static_cast<std::uint64_t>(
            MmsServer_getBerDepthRejectCount(mms_server));
        result.max_outstanding_rejections = static_cast<std::uint64_t>(
            MmsServer_getMaxOutstandingRejectCount(mms_server));
        result.report_buffer_overflows = static_cast<std::uint64_t>(
            MmsServer_getReportBufferOverflowCount(mms_server));
    }
    return result;
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
            server_,
            model_->measurement_quality(index),
            quality_for(acquisition::Availability::invalid, false));
        update_timestamp(model_->measurement_time(index), timestamp_ms);
    }
    IedServer_updateFloatAttributeValue(server_, model_->peak_value(), 0.0F);
    IedServer_updateQuality(
        server_, model_->peak_quality(), quality_for(acquisition::Availability::invalid, false));
    update_timestamp(model_->peak_time(), timestamp_ms);
    IedServer_updateBooleanAttributeValue(server_, model_->alarm_value(), false);
    IedServer_updateQuality(
        server_, model_->alarm_quality(), quality_for(acquisition::Availability::invalid, false));
    update_timestamp(model_->alarm_time(), timestamp_ms);
    IedServer_unlockDataModel(server_);
}

void Server::publish_snapshot(const acquisition::ServingView& serving_view) {
    if (!serving_view.snapshot) {
        publish_invalid_values();
        return;
    }
    const std::uint64_t timestamp_ms = now_milliseconds();
    const domain::ParsedSnapshot& payload = serving_view.snapshot->payload;
    IedServer_lockDataModel(server_);
    for (std::size_t index = 0; index < kMeasurementCount; ++index) {
        const domain::Measurement& measurement = payload.measurements[index];
        const bool measurement_usable =
            serving_view.status.availability != acquisition::Availability::invalid &&
            measurement.valid;
        if (model_->measurement_integer(index)) {
            IedServer_updateInt32AttributeValue(
                server_, model_->measurement_value(index),
                measurement_usable ? static_cast<std::int32_t>(measurement.value) : 0);
        } else {
            IedServer_updateFloatAttributeValue(
                server_,
                model_->measurement_value(index),
                measurement_usable ? static_cast<float>(measurement.value) : 0.0F);
        }
        IedServer_updateQuality(
            server_,
            model_->measurement_quality(index),
            quality_for(serving_view.status.availability, measurement.valid));
        update_timestamp(model_->measurement_time(index), timestamp_ms);
    }

    const domain::Measurement& peak = payload.measurements[2U];
    const bool alarm_valid = serving_view.status.availability != acquisition::Availability::invalid &&
        peak.valid && payload.payload_status != domain::PayloadStatus::not_refreshed;
    const bool alarm = serving_view.status.availability == acquisition::Availability::fresh &&
        alarm_valid &&
        (alarm_provider_ ? alarm_provider_() : peak.value >= -45);
    IedServer_updateFloatAttributeValue(
        server_,
        model_->peak_value(),
        peak.valid && serving_view.status.availability != acquisition::Availability::invalid
            ? static_cast<float>(peak.value)
            : 0.0F);
    IedServer_updateQuality(
        server_,
        model_->peak_quality(),
        quality_for(serving_view.status.availability, peak.valid));
    update_timestamp(model_->peak_time(), timestamp_ms);
    IedServer_updateBooleanAttributeValue(server_, model_->alarm_value(), alarm);
    IedServer_updateQuality(
        server_,
        model_->alarm_quality(),
        quality_for(serving_view.status.availability, alarm_valid));
    update_timestamp(model_->alarm_time(), timestamp_ms);
    IedServer_unlockDataModel(server_);
}

void Server::update_loop() {
    std::uint64_t last_generation = 0U;
    std::optional<acquisition::Availability> last_availability;
    bool last_has_snapshot = false;
    while (!stop_requested_.load()) {
        const acquisition::ServingView serving_view = snapshot_store_.serving_view();
        const bool has_snapshot = serving_view.snapshot.has_value();
        const bool status_changed =
            !last_availability || *last_availability != serving_view.status.availability;
        const bool generation_changed = serving_view.snapshot &&
            serving_view.snapshot->generation > last_generation;
        if (status_changed || has_snapshot != last_has_snapshot || generation_changed) {
            if (has_snapshot) {
                publish_snapshot(serving_view);
                last_generation = serving_view.snapshot->generation;
            } else {
                publish_invalid_values();
                last_generation = 0U;
            }
            last_has_snapshot = has_snapshot;
            last_availability = serving_view.status.availability;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

}  // namespace uhf::iec61850
