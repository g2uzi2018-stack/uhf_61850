// SPDX-License-Identifier: GPL-3.0-only
#include "app/gateway_runtime.hpp"

#include "acquisition/loopback_pd1000.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <stdexcept>
#include <utility>

namespace uhf::app {

namespace {

class V3SerialAdapter final : public v3::ISerialPort {
public:
    V3SerialAdapter(acquisition::ISerialPort& port, v3::PacketTraceBuffer& trace,
                    v3::PacketSource source)
        : port_(port), trace_(trace), source_(source) {}
    bool write_all(const std::uint8_t* data, std::size_t size) override {
        const bool written = port_.write_all(data, size);
        if (written) {
            trace_.record(source_, v3::PacketDirection::transmit, data, size);
        }
        return written;
    }
    bool read_some(std::uint8_t* data, std::size_t capacity,
                   std::chrono::milliseconds timeout, std::size_t& received) override {
        const bool read = port_.read_some(data, capacity, timeout, received);
        if (read && received > 0U) {
            trace_.record(source_, v3::PacketDirection::receive, data, received);
        }
        return read;
    }
private:
    acquisition::ISerialPort& port_;
    v3::PacketTraceBuffer& trace_;
    v3::PacketSource source_;
};

std::optional<iec61850::SclModelDefinition> parse_file(
    const std::filesystem::path& path) {
    std::error_code error;
    if (std::filesystem::is_symlink(path, error) || error ||
        !std::filesystem::is_regular_file(path, error) || error) {
        return std::nullopt;
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return std::nullopt;
    }
    const std::string contents(
        (std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    if (input.bad()) {
        return std::nullopt;
    }
    iec61850::SclModelDefinition definition;
    std::string parse_error;
    if (!iec61850::parse_scl_model(contents, definition, parse_error)) {
        return std::nullopt;
    }
    return definition;
}

v3::AlarmThresholds alarm_thresholds_from_config(const config::Values& values) {
    v3::AlarmThresholds thresholds;
    static_assert(
        config::kV3AlarmThresholdCount == v3::kAlarmCount,
        "configuration and runtime alarm counts must remain aligned");
    for (std::size_t index = 0U; index < thresholds.values.size(); ++index) {
        if (values.v3_alarm_thresholds[index]) {
            thresholds.values[index] = *values.v3_alarm_thresholds[index];
        }
    }
    return thresholds;
}

}  // namespace

GatewayRuntime::GatewayRuntime(GatewayRuntimeOptions options, logging::Logger& logger)
    : options_(std::move(options)),
      logger_(logger),
    iec_current_bind_(options_.iec61850_bind),
      iec_current_port_(options_.iec61850_port) {
    if (options_.v3_enabled) {
        activation_manager_ = std::make_unique<activation::Manager>(
            options_.activation_options);
        if (!activation_manager_->active()) {
            throw std::runtime_error(
                "v3 activation is required before acquisition and gateway services start");
        }
        if (options_.simulate) {
            throw std::invalid_argument(
                "v3 mode requires explicit PTY/device paths; --simulate is for the legacy loopback");
        }
        v3_pd_serial_port_ = std::make_unique<acquisition::PosixSerialPort>(options_.v3_pd_device);
        v3_current_serial_port_ = std::make_unique<acquisition::PosixSerialPort>(options_.v3_current_device);
        v3_temperature_serial_port_ = std::make_unique<acquisition::PosixSerialPort>(options_.v3_temperature_device);
        v3_packet_trace_ = std::make_unique<v3::PacketTraceBuffer>();
        v3_pd_adapter_ = std::make_unique<V3SerialAdapter>(
            *v3_pd_serial_port_, *v3_packet_trace_, v3::PacketSource::pd);
        v3_current_adapter_ = std::make_unique<V3SerialAdapter>(
            *v3_current_serial_port_, *v3_packet_trace_, v3::PacketSource::current);
        v3_temperature_adapter_ = std::make_unique<V3SerialAdapter>(
            *v3_temperature_serial_port_, *v3_packet_trace_, v3::PacketSource::temperature);
        const v3::AlarmThresholds alarm_thresholds = options_.config_store != nullptr
            ? alarm_thresholds_from_config(options_.config_store->snapshot().values)
            : v3::AlarmThresholds{};
        v3_snapshot_store_ = std::make_unique<v3::SnapshotStore>(
            3U, alarm_thresholds);
        v3_scheduler_ = std::make_unique<v3::AcquisitionScheduler>(
            *v3_pd_adapter_, *v3_current_adapter_, *v3_temperature_adapter_,
            *v3_snapshot_store_, options_.v3_scheduler_options);
    } else {
        if (options_.simulate) {
            serial_port_ = std::make_unique<acquisition::LoopbackPd1000Port>();
        } else {
            serial_port_ = std::make_unique<acquisition::PosixSerialPort>(options_.acquisition_device);
        }
        acquisition_engine_ = std::make_unique<acquisition::AcquisitionEngine>(
            *serial_port_, snapshot_store_, options_.acquisition_options);
    }
    if (options_.start_modbus_tcp) {
        modbus_tcp_server_ = std::make_unique<modbus::ModbusTcpServer>(
            snapshot_store_,
            modbus::ModbusTcpOptions{
                options_.modbus_tcp_bind,
                options_.modbus_tcp_port,
                options_.modbus_tcp_unit_id,
                16U},
            v3_snapshot_store_.get());
    }
    if (options_.start_modbus_rtu) {
        modbus_rtu_serial_port_ =
            std::make_unique<acquisition::PosixSerialPort>(options_.modbus_rtu_device);
        modbus_rtu_server_ = std::make_unique<modbus::ModbusRtuServer>(
            *modbus_rtu_serial_port_, snapshot_store_, options_.modbus_rtu_options,
            v3_snapshot_store_.get());
    }
    if (options_.start_persistence) {
        options_.persistence_options.v3_snapshot_store = v3_snapshot_store_.get();
        persistence_worker_ = std::make_unique<storage::PersistenceWorker>(
            snapshot_store_, logger_, options_.persistence_options);
    }
    if (options_.start_iec61850) {
        iec61850::ServerOptions iec_options;
        iec_options.bind_address = options_.iec61850_bind;
        iec_options.port = options_.iec61850_port;
        iec_options.ied_name = options_.iec61850_ied_name;
        const std::optional<iec61850::SclModelDefinition> definition =
            options_.v3_enabled ? std::optional<iec61850::SclModelDefinition>{
                iec61850::default_v3_model_definition(options_.iec61850_ied_name)}
                : load_iec61850_definition();
        if (definition) {
            iec_options.model_definition = *definition;
            iec_options.model_definition->ied_name = options_.iec61850_ied_name;
        } else {
            logger_.log(
                logging::Level::warning,
                logging::Component::iec61850,
                "model.load_failed",
                "configured ICD could not be loaded; using built-in IEC 61850 model");
        }
        if (persistence_worker_) {
            iec_options.alarm_provider = [this] { return persistence_worker_->alarm_active(); };
        }
        iec_options.v3_snapshot_store = v3_snapshot_store_.get();
        iec61850_server_ = std::make_unique<iec61850::Server>(
            snapshot_store_, std::move(iec_options));
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
    if (v3_scheduler_) {
        v3_scheduler_->start();
    }
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
    {
        std::lock_guard<std::mutex> lock(iec_mutex_);
        if (iec61850_server_) {
            iec61850_server_->start();
        }
    }
    if (persistence_worker_) {
        persistence_worker_->start();
    }
}

void GatewayRuntime::stop() noexcept {
    stop_requested_.store(true);
    if (v3_scheduler_) {
        v3_scheduler_->stop();
    }
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
    {
        std::lock_guard<std::mutex> lock(iec_mutex_);
        if (iec61850_server_) {
            iec61850_server_->stop();
        }
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

const v3::SnapshotStore* GatewayRuntime::v3_snapshot_store() const noexcept {
    return v3_snapshot_store_.get();
}

const v3::PacketTraceBuffer* GatewayRuntime::v3_packet_trace() const noexcept {
    return v3_packet_trace_.get();
}

health::Input GatewayRuntime::health_input() const {
    health::Input input;
    if (v3_snapshot_store_) {
        const v3::UnifiedSnapshot snapshot = v3_snapshot_store_->snapshot();
        const bool any_success = snapshot.pd_status.has_sample ||
            snapshot.current_status.has_sample || snapshot.temperature_status.has_sample;
        if (any_success) {
            input.last_acquisition_success = std::max({
                snapshot.pd_status.last_success,
                snapshot.current_status.last_success,
                snapshot.temperature_status.last_success});
        }
        input.acquisition_last_cycle_ok = snapshot.pd_status.online &&
            snapshot.current_status.online && snapshot.temperature_status.online;
        input.consecutive_no_response = std::max({
            snapshot.pd_status.consecutive_failures,
            snapshot.current_status.consecutive_failures,
            snapshot.temperature_status.consecutive_failures});
        input.communication_alarm = snapshot.pd_status.communication_alarm ||
            snapshot.current_status.communication_alarm || snapshot.temperature_status.communication_alarm;
    }
    if (!v3_snapshot_store_) {
        const acquisition::ServingView serving_view = snapshot_store_.serving_view();
        if (serving_view.snapshot) {
            input.last_acquisition_success = serving_view.snapshot->completed_at;
        }
        input.acquisition_last_cycle_ok =
            serving_view.status.availability == acquisition::Availability::fresh;
        input.consecutive_no_response = serving_view.status.consecutive_no_response;
        input.communication_alarm = serving_view.status.communication_alarm;
    }
    input.storage_writable = true;
    input.modbus_tcp_listening =
        modbus_tcp_server_ != nullptr && modbus_tcp_server_->bound_port() != 0U;
    input.modbus_rtu_ready = modbus_rtu_server_ != nullptr;
    {
        std::lock_guard<std::mutex> lock(iec_mutex_);
        input.iec61850_enabled =
            iec61850_server_ != nullptr && iec61850_server_->running();
    }
    if (persistence_worker_) {
        const storage::PersistenceStats stats = persistence_worker_->stats();
        input.storage_writable =
            !stats.writes_paused && !stats.cleanup_failed && !stats.write_failed;
        input.storage_low_watermark = stats.low_watermark_active;
    }
    return input;
}

iec61850::RuntimeStats GatewayRuntime::iec61850_stats() const noexcept {
    std::lock_guard<std::mutex> lock(iec_mutex_);
    if (!iec61850_server_) {
        return {};
    }
    return iec61850_server_->stats();
}

std::optional<iec61850::RuntimeEndpoint> GatewayRuntime::iec61850_endpoint() const {
    std::lock_guard<std::mutex> lock(iec_mutex_);
    if (!iec61850_server_) {
        return std::nullopt;
    }
    return iec61850_server_->endpoint();
}

std::optional<iec61850::SclModelDefinition> GatewayRuntime::iec61850_model_definition() const {
    std::lock_guard<std::mutex> lock(iec_mutex_);
    if (!iec61850_server_) {
        return std::nullopt;
    }
    return iec61850_server_->model_definition();
}

std::optional<iec61850::SclModelDefinition> GatewayRuntime::load_iec61850_definition() const {
    if (const std::optional<iec61850::SclModelDefinition> definition =
            parse_file(options_.iec61850_icd_override)) {
        return definition;
    }
    return parse_file(options_.iec61850_icd_packaged);
}

bool GatewayRuntime::reload_iec61850_model() {
    std::optional<iec61850::SclModelDefinition> definition = options_.v3_enabled
        ? std::optional<iec61850::SclModelDefinition>{
              iec61850::default_v3_model_definition(options_.iec61850_ied_name)}
        : load_iec61850_definition();
    if (!definition) {
        return false;
    }
    if (options_.config_store != nullptr) {
        definition->ied_name = options_.config_store->snapshot().values.iec_ied_name;
    } else {
        definition->ied_name = options_.iec61850_ied_name;
    }

    std::unique_lock<std::mutex> lock(iec_mutex_);
    if (!iec61850_server_) {
        return true;
    }
    iec61850::ServerOptions candidate_options;
    candidate_options.bind_address = iec_current_bind_;
    candidate_options.port = iec_current_port_;
    candidate_options.ied_name = definition->ied_name;
    candidate_options.model_definition = *definition;
    candidate_options.v3_snapshot_store = v3_snapshot_store_.get();
    if (persistence_worker_) {
        candidate_options.alarm_provider = [this] { return persistence_worker_->alarm_active(); };
    }
    std::unique_ptr<iec61850::Server> candidate;
    try {
        candidate = std::make_unique<iec61850::Server>(
            snapshot_store_, std::move(candidate_options));
    } catch (...) {
        return false;
    }

    std::unique_ptr<iec61850::Server> previous;
    const bool was_running = iec61850_server_->running();
    if (was_running) {
        iec61850_server_->stop();
    }
    try {
        if (was_running) {
            candidate->start();
        }
    } catch (...) {
        try {
            if (was_running) {
                iec61850_server_->start();
            }
        } catch (...) {
        }
        return false;
    }
    previous = std::move(iec61850_server_);
    iec61850_server_ = std::move(candidate);
    lock.unlock();
    previous.reset();
    return true;
}

void GatewayRuntime::run() {
    bool previous_cycle_failed = false;
    bool communication_alarm_active = false;
    std::uint64_t applied_config_version = 0U;
    std::chrono::steady_clock::time_point next_poll = std::chrono::steady_clock::now();
    while (!stop_requested_.load()) {
        apply_runtime_configuration(applied_config_version);
        if (v3_scheduler_) {
            while (!stop_requested_.load()) {
                apply_runtime_configuration(applied_config_version);
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            return;
        }
        const bool success = acquisition_engine_->poll_once();
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
        const acquisition::AcquisitionStatus acquisition_status =
            snapshot_store_.serving_view().status;
        if (acquisition_status.communication_alarm && !communication_alarm_active) {
            logger_.log(
                logging::Level::error,
                logging::Component::acquisition,
                "communication.alarm",
                "PD1000 communication alarm raised after three consecutive no-response cycles",
                {logging::Field{
                    "consecutive_no_response",
                    std::to_string(acquisition_status.consecutive_no_response)}});
        } else if (!acquisition_status.communication_alarm && communication_alarm_active) {
            logger_.recovered(
                logging::Component::acquisition,
                "communication.alarm",
                "PD1000 communication recovered");
        }
        communication_alarm_active = acquisition_status.communication_alarm;

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

void GatewayRuntime::apply_runtime_configuration(std::uint64_t& applied_version) {
    if (options_.config_store == nullptr) {
        return;
    }
    const config::Snapshot configured = options_.config_store->snapshot();
    if (configured.version == applied_version) {
        return;
    }

    if (acquisition_engine_) {
        options_.acquisition_options.slave_id = configured.values.acquisition_slave_id;
        options_.acquisition_options.response_timeout = std::chrono::milliseconds(
            configured.values.acquisition_response_timeout_ms);
        options_.acquisition_options.max_retries = configured.values.acquisition_max_retries;
        options_.poll_interval = std::chrono::milliseconds(configured.values.acquisition_period_ms);
        acquisition_engine_->update_options(options_.acquisition_options);
    }
    if (v3_snapshot_store_) {
        v3_snapshot_store_->update_alarm_thresholds(
            alarm_thresholds_from_config(configured.values));
    }
    if (modbus_tcp_server_) {
        modbus::ModbusTcpOptions modbus_options;
        modbus_options.bind_address = options_.modbus_tcp_bind_all
            ? "0.0.0.0"
            : options_.reload_modbus_tcp_endpoint
            ? configured.values.modbus_tcp_bind
            : options_.modbus_tcp_bind;
        modbus_options.port = options_.reload_modbus_tcp_endpoint
            ? configured.values.modbus_tcp_port
            : options_.modbus_tcp_port;
        modbus_options.unit_id = configured.values.modbus_tcp_unit_id;
        modbus_options.max_connections = 16U;
        if (!modbus_tcp_server_->update_options(std::move(modbus_options))) {
            logger_.log(
                logging::Level::error,
                logging::Component::modbus_tcp,
                "configuration.reload_failed",
                "invalid Modbus TCP settings were rejected");
        }
    }
    const std::optional<iec61850::SclModelDefinition> active_definition =
        iec61850_model_definition();
    if (active_definition && active_definition->ied_name != configured.values.iec_ied_name) {
        if (!reload_iec61850_model()) {
            logger_.log(
                logging::Level::error,
                logging::Component::iec61850,
                "configuration.reload_failed",
                "IEC 61850 IED name reload failed; previous model was restored");
        } else {
            options_.iec61850_ied_name = configured.values.iec_ied_name;
        }
    }
    {
        std::lock_guard<std::mutex> lock(iec_mutex_);
        if (iec61850_server_ && !configured.values.iec_enabled && iec61850_server_->running()) {
            iec61850_server_->stop();
        } else if (iec61850_server_ && configured.values.iec_enabled &&
                   iec61850_server_->running() &&
                   !iec61850_server_->update_endpoint(
                       options_.iec61850_bind_all
                           ? "0.0.0.0"
                           : options_.reload_iec61850_endpoint
                           ? configured.values.modbus_tcp_bind
                           : options_.iec61850_bind,
                       options_.reload_iec61850_endpoint
                           ? configured.values.iec_port
                           : options_.iec61850_port)) {
            logger_.log(
                logging::Level::error,
                logging::Component::iec61850,
                "configuration.reload_failed",
                "IEC 61850 endpoint reload failed; previous endpoint was restored");
        } else if (iec61850_server_ && configured.values.iec_enabled) {
            iec_current_bind_ = options_.iec61850_bind_all
                ? "0.0.0.0"
                : options_.reload_iec61850_endpoint
                ? configured.values.modbus_tcp_bind
                : options_.iec61850_bind;
            iec_current_port_ = options_.reload_iec61850_endpoint
                ? configured.values.iec_port
                : options_.iec61850_port;
        }
    }
    applied_version = configured.version;
    logger_.log(
        logging::Level::info,
        logging::Component::config,
        "configuration.reloaded",
        "hot-reloadable runtime settings applied",
        {logging::Field{"version", std::to_string(applied_version)}});
}

}  // namespace uhf::app
