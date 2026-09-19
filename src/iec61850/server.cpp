// SPDX-License-Identifier: GPL-3.0-only
#include "iec61850/server.hpp"

#include "domain/snapshot.hpp"

extern "C" {
#include "mms_server_libinternal.h"
}

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
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

std::uint64_t timestamp_milliseconds(
    std::chrono::system_clock::time_point timestamp,
    std::uint64_t fallback) noexcept {
    const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
        timestamp.time_since_epoch()).count();
    return milliseconds > 0 ? static_cast<std::uint64_t>(milliseconds) : fallback;
}

bool temperature_measurement(std::size_t index) noexcept {
    return (index >= 8U && index <= 11U) || (index >= 19U && index <= 22U) ||
        (index >= 32U && index <= 34U);
}

Quality v3_quality(bool valid, bool stale, bool last_good_available) noexcept {
    if (stale && last_good_available) {
        return static_cast<Quality>(
            QUALITY_VALIDITY_QUESTIONABLE | QUALITY_DETAIL_OLD_DATA);
    }
    return valid
        ? static_cast<Quality>(QUALITY_VALIDITY_GOOD)
        : static_cast<Quality>(QUALITY_VALIDITY_INVALID);
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
      model_(options_.model_definition
          ? std::make_unique<Model>(std::move(*options_.model_definition))
          : options_.v3_snapshot_store != nullptr
          ? std::make_unique<Model>(default_v3_model_definition(options_.ied_name))
          : std::make_unique<Model>(options_.ied_name)),
      alarm_provider_(options_.alarm_provider),
      v3_snapshot_store_(options_.v3_snapshot_store) {
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

RuntimeEndpoint Server::endpoint() const {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    return RuntimeEndpoint{options_.bind_address, options_.port};
}

SclModelDefinition Server::model_definition() const {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    return model_->definition();
}

void Server::update_timestamp(DataAttribute* attribute, std::uint64_t timestamp_ms) {
    IedServer_updateUTCTimeAttributeValue(server_, attribute, timestamp_ms);
}

void Server::update_communication_alarm(bool active, std::uint64_t timestamp_ms) {
    IedServer_updateBooleanAttributeValue(
        server_, model_->communication_alarm_value(), active);
    IedServer_updateQuality(
        server_,
        model_->communication_alarm_quality(),
        static_cast<Quality>(QUALITY_VALIDITY_GOOD));
    update_timestamp(model_->communication_alarm_time(), timestamp_ms);
}

void Server::publish_invalid_values(bool communication_alarm) {
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
    update_communication_alarm(communication_alarm, timestamp_ms);
    if (model_->is_v3()) {
        publish_v3_invalid_values();
    }
    IedServer_unlockDataModel(server_);
}

void Server::publish_v3_invalid_values() {
    const std::uint64_t timestamp_ms = now_milliseconds();
    const auto invalid_quality = static_cast<Quality>(QUALITY_VALIDITY_INVALID);
    for (std::size_t index = 0U; index < 3U; ++index) {
        IedServer_updateFloatAttributeValue(server_, model_->v3_pd_peak_value(index), 0.0F);
        IedServer_updateQuality(server_, model_->v3_pd_peak_quality(index), invalid_quality);
        update_timestamp(model_->v3_pd_peak_time(index), timestamp_ms);
    }
    for (std::size_t index = 0U; index < 35U; ++index) {
        IedServer_updateFloatAttributeValue(server_, model_->v3_measurement_value(index), 0.0F);
        IedServer_updateQuality(server_, model_->v3_measurement_quality(index), invalid_quality);
        update_timestamp(model_->v3_measurement_time(index), timestamp_ms);
    }
    for (std::size_t index = 0U; index < 3U; ++index) {
        IedServer_updateFloatAttributeValue(server_, model_->v3_temperature_value(index), 0.0F);
        IedServer_updateQuality(server_, model_->v3_temperature_quality(index), invalid_quality);
        update_timestamp(model_->v3_temperature_time(index), timestamp_ms);
    }
    for (std::size_t index = 0U; index < 15U; ++index) {
        IedServer_updateBooleanAttributeValue(server_, model_->v3_discrete_value(index), false);
        IedServer_updateQuality(server_, model_->v3_discrete_quality(index), invalid_quality);
        update_timestamp(model_->v3_discrete_time(index), timestamp_ms);
    }
}

void Server::publish_v3_snapshot(const v3::UnifiedSnapshot& snapshot) {
    const std::uint64_t now_ms = now_milliseconds();
    const std::uint64_t pd_timestamp_ms = timestamp_milliseconds(
        snapshot.pd_status.last_success_utc, now_ms);
    const std::uint64_t current_timestamp_ms = timestamp_milliseconds(
        snapshot.current_status.last_success_utc, now_ms);
    const std::uint64_t temperature_timestamp_ms = timestamp_milliseconds(
        snapshot.temperature_status.last_success_utc, now_ms);
    IedServer_lockDataModel(server_);
    for (std::size_t channel = 0U; channel < 3U; ++channel) {
        const v3::PdFeature& peak = snapshot.pd[channel].features[2];
        const bool valid = snapshot.pd_valid[channel] && peak.valid;
        const bool last_good_available = peak.valid && std::isfinite(peak.value);
        IedServer_updateFloatAttributeValue(
            server_, model_->v3_pd_peak_value(channel),
            valid || (snapshot.pd_status.stale && last_good_available)
                ? peak.value : 0.0F);
        IedServer_updateQuality(
            server_, model_->v3_pd_peak_quality(channel),
            v3_quality(valid, snapshot.pd_status.stale, last_good_available));
        update_timestamp(model_->v3_pd_peak_time(channel), pd_timestamp_ms);
    }
    for (std::size_t index = 0U; index < snapshot.measurements.size(); ++index) {
        const v3::Value& value = snapshot.measurements[index];
        const bool temperature = temperature_measurement(index);
        const bool stale = temperature
            ? snapshot.temperature_status.stale : snapshot.current_status.stale;
        const bool last_good_available = std::isfinite(value.value);
        IedServer_updateFloatAttributeValue(
            server_, model_->v3_measurement_value(index),
            value.valid() || (stale && last_good_available) ? value.value : 0.0F);
        IedServer_updateQuality(
            server_, model_->v3_measurement_quality(index),
            v3_quality(value.valid(), stale, last_good_available));
        update_timestamp(
            model_->v3_measurement_time(index),
            temperature ? temperature_timestamp_ms : current_timestamp_ms);
    }
    for (std::size_t index = 0U; index < snapshot.temperature.size(); ++index) {
        const v3::Value& value = snapshot.temperature[index];
        const bool last_good_available = std::isfinite(value.value);
        IedServer_updateFloatAttributeValue(
            server_, model_->v3_temperature_value(index),
            value.valid() || (snapshot.temperature_status.stale && last_good_available)
                ? value.value : 0.0F);
        IedServer_updateQuality(
            server_, model_->v3_temperature_quality(index),
            v3_quality(
                value.valid(), snapshot.temperature_status.stale, last_good_available));
        update_timestamp(model_->v3_temperature_time(index), temperature_timestamp_ms);
    }
    const std::array<bool, 15U> discrete_valid = {
        snapshot.pd_status.has_sample,
        snapshot.current_status.has_sample,
        snapshot.temperature_status.has_sample,
        snapshot.alarm_valid[0], snapshot.alarm_valid[1], snapshot.alarm_valid[2],
        snapshot.alarm_valid[3], snapshot.alarm_valid[4], snapshot.alarm_valid[5],
        snapshot.alarm_valid[6], snapshot.alarm_valid[7], snapshot.alarm_valid[8],
        snapshot.alarm_valid[9], snapshot.alarm_valid[10], snapshot.alarm_valid[11]};
    const std::array<bool, 15U> discrete_active = {
        !snapshot.pd_status.online, !snapshot.current_status.online,
        !snapshot.temperature_status.online,
        snapshot.alarm_active[0], snapshot.alarm_active[1], snapshot.alarm_active[2],
        snapshot.alarm_active[3], snapshot.alarm_active[4], snapshot.alarm_active[5],
        snapshot.alarm_active[6], snapshot.alarm_active[7], snapshot.alarm_active[8],
        snapshot.alarm_active[9], snapshot.alarm_active[10], snapshot.alarm_active[11]};
    const std::array<const v3::SourceStatus*, 15U> discrete_sources{
        &snapshot.pd_status, &snapshot.current_status, &snapshot.temperature_status,
        &snapshot.pd_status, &snapshot.pd_status, &snapshot.pd_status,
        &snapshot.current_status, &snapshot.current_status, &snapshot.current_status,
        &snapshot.current_status, &snapshot.current_status, &snapshot.current_status,
        &snapshot.temperature_status, &snapshot.temperature_status,
        &snapshot.temperature_status};
    for (std::size_t index = 0U; index < discrete_valid.size(); ++index) {
        const v3::SourceStatus& source = *discrete_sources[index];
        IedServer_updateBooleanAttributeValue(
            server_, model_->v3_discrete_value(index), discrete_active[index]);
        IedServer_updateQuality(
            server_, model_->v3_discrete_quality(index),
            v3_quality(discrete_valid[index], source.stale, source.has_sample));
        update_timestamp(
            model_->v3_discrete_time(index),
            timestamp_milliseconds(source.last_success_utc, now_ms));
    }
    std::uint64_t communication_timestamp_ms = std::max({
        timestamp_milliseconds(snapshot.pd_status.last_attempt_utc, 0U),
        timestamp_milliseconds(snapshot.current_status.last_attempt_utc, 0U),
        timestamp_milliseconds(snapshot.temperature_status.last_attempt_utc, 0U)});
    if (communication_timestamp_ms == 0U) communication_timestamp_ms = now_ms;
    update_communication_alarm(
        snapshot.pd_status.communication_alarm || snapshot.current_status.communication_alarm ||
            snapshot.temperature_status.communication_alarm,
        communication_timestamp_ms);
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
    update_communication_alarm(serving_view.status.communication_alarm, timestamp_ms);
    IedServer_unlockDataModel(server_);
}

void Server::update_loop() {
    if (v3_snapshot_store_ != nullptr && model_->is_v3()) {
        std::uint64_t last_generation = 0U;
        std::array<bool, 3U> last_stale{};
        while (!stop_requested_.load()) {
            const v3::UnifiedSnapshot snapshot = v3_snapshot_store_->snapshot();
            const std::array<bool, 3U> stale{
                snapshot.pd_status.stale,
                snapshot.current_status.stale,
                snapshot.temperature_status.stale};
            if (snapshot.generation != last_generation || stale != last_stale) {
                publish_v3_snapshot(snapshot);
                last_generation = snapshot.generation;
                last_stale = stale;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        return;
    }
    std::uint64_t last_generation = 0U;
    std::optional<acquisition::Availability> last_availability;
    bool last_has_snapshot = false;
    bool last_communication_alarm = false;
    while (!stop_requested_.load()) {
        const acquisition::ServingView serving_view = snapshot_store_.serving_view();
        const bool has_snapshot = serving_view.snapshot.has_value();
        const bool status_changed =
            !last_availability || *last_availability != serving_view.status.availability;
        const bool generation_changed = serving_view.snapshot &&
            serving_view.snapshot->generation > last_generation;
        const bool communication_alarm_changed =
            serving_view.status.communication_alarm != last_communication_alarm;
        if (status_changed || has_snapshot != last_has_snapshot || generation_changed ||
            communication_alarm_changed) {
            if (has_snapshot) {
                publish_snapshot(serving_view);
                last_generation = serving_view.snapshot->generation;
            } else {
                publish_invalid_values(serving_view.status.communication_alarm);
                last_generation = 0U;
            }
            last_has_snapshot = has_snapshot;
            last_availability = serving_view.status.availability;
            last_communication_alarm = serving_view.status.communication_alarm;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

}  // namespace uhf::iec61850
