// SPDX-License-Identifier: GPL-3.0-only
#include "v3/acquisition.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>

namespace uhf::v3 {
namespace {

constexpr std::size_t kResponseHeaderBytes = 3U;
constexpr std::size_t kResponseCrcBytes = 2U;
constexpr std::size_t kMaxFrameBytes = 255U;
constexpr std::size_t kMaxErrorBytes = 256U;

void invalidate_pd(PdChannel& channel) noexcept {
    channel.received.reset();
    channel.spectrum_received.reset();
    for (auto& feature : channel.features) {
        feature.valid = false;
    }
}

SourceStatus& source_status(UnifiedSnapshot& value, Source source) noexcept {
    switch (source) {
    case Source::pd:
        return value.pd_status;
    case Source::current:
        return value.current_status;
    case Source::temperature:
        return value.temperature_status;
    }
    return value.pd_status;
}

const SourceStatus& source_status(const UnifiedSnapshot& value, Source source) noexcept {
    switch (source) {
    case Source::pd:
        return value.pd_status;
    case Source::current:
        return value.current_status;
    case Source::temperature:
        return value.temperature_status;
    }
    return value.pd_status;
}

CurrentValues current_values_with_quality(const std::array<float, 8>& values) noexcept {
    CurrentValues out{};
    for (std::size_t i = 0U; i < out.size(); ++i) {
        out[i] = valid_value(values[i]);
    }
    return out;
}

TemperatureValues temperature_values_with_quality(
    const std::array<float, 3>& values) noexcept {
    TemperatureValues out{};
    for (std::size_t i = 0U; i < out.size(); ++i) {
        out[i] = valid_value(values[i]);
    }
    return out;
}

}  // namespace

AlarmThresholds::AlarmThresholds() {
    values.fill(std::numeric_limits<float>::quiet_NaN());
}

std::chrono::steady_clock::time_point SteadyClock::now() const {
    return std::chrono::steady_clock::now();
}

void SteadyClock::sleep_for(std::chrono::steady_clock::duration duration) {
    if (duration > std::chrono::steady_clock::duration::zero()) {
        std::this_thread::sleep_for(duration);
    }
}

SnapshotStore::SnapshotStore(std::uint32_t alarm_after_failures,
                             AlarmThresholds thresholds)
    : alarm_after_failures_(alarm_after_failures == 0U ? 1U : alarm_after_failures),
      thresholds_(std::move(thresholds)) {}

SourceStatus& SnapshotStore::status_for(UnifiedSnapshot& value, Source source) const noexcept {
    return source_status(value, source);
}

const SourceStatus& SnapshotStore::status_for(
    const UnifiedSnapshot& value, Source source) const noexcept {
    return source_status(value, source);
}

void SnapshotStore::mark_success(Source source,
                                 std::chrono::steady_clock::time_point at) {
    SourceStatus& status = status_for(value_, source);
    status.has_sample = true;
    status.online = true;
    status.communication_alarm = false;
    status.consecutive_failures = 0U;
    status.last_attempt = at;
    status.last_success = at;
    status.last_error.clear();
}

void SnapshotStore::publish_pd_channel(
    std::size_t channel, PdChannel value, std::chrono::steady_clock::time_point at) {
    if (channel >= kChannelCount) {
        throw std::out_of_range("PD channel must be 0..2");
    }
    std::lock_guard<std::mutex> lock(mutex_);
    value_.pd[channel] = std::move(value);
    value_.pd_valid[channel] = true;
    ++value_.generation;
    pd_channel_online_[channel] = true;
    SourceStatus& status = value_.pd_status;
    status.has_sample = true;
    status.last_attempt = at;
    status.last_success = at;
    status.last_error.clear();
    const bool all_channels_online = std::all_of(
        pd_channel_online_.begin(), pd_channel_online_.end(), [](bool online) { return online; });
    status.online = all_channels_online;
    if (all_channels_online) {
        status.communication_alarm = false;
        status.consecutive_failures = 0U;
    }
    recompute_alarms();
}

void SnapshotStore::publish_current(CurrentValues value,
                                    std::chrono::steady_clock::time_point at) {
    std::lock_guard<std::mutex> lock(mutex_);
    value_.current = std::move(value);
    if (current_sequence_ == std::numeric_limits<std::uint64_t>::max()) {
        calculator_ = MonitoringCalculator(WarmupPolicy::use_available,
                                            MeanPolicy::reject_nonpositive);
        current_sequence_ = 0U;
    }
    ++current_sequence_;
    (void)calculator_.on_current(current_sequence_, value_.current);
    value_.measurements = calculator_.snapshot();
    recompute_alarms();
    ++value_.generation;
    mark_success(Source::current, at);
}

void SnapshotStore::publish_temperature(TemperatureValues value,
                                         std::chrono::steady_clock::time_point at) {
    std::lock_guard<std::mutex> lock(mutex_);
    value_.temperature = std::move(value);
    calculator_.on_temperature(value_.temperature);
    value_.measurements = calculator_.snapshot();
    recompute_alarms();
    ++value_.generation;
    mark_success(Source::temperature, at);
}

void SnapshotStore::record_failure(Source source,
                                   std::chrono::steady_clock::time_point at,
                                   std::string error) {
    if (error.size() > kMaxErrorBytes) {
        error.resize(kMaxErrorBytes);
    }
    std::lock_guard<std::mutex> lock(mutex_);
    SourceStatus& status = status_for(value_, source);
    status.online = false;
    status.last_attempt = at;
    status.last_error = std::move(error);
    if (status.consecutive_failures < std::numeric_limits<std::uint32_t>::max()) {
        ++status.consecutive_failures;
    }
    status.communication_alarm = status.consecutive_failures >= alarm_after_failures_;
    if (source == Source::current) {
        value_.current = {};
        calculator_.invalidate_current();
        value_.measurements = calculator_.snapshot();
    } else if (source == Source::temperature) {
        value_.temperature = {};
        calculator_.invalidate_temperature();
        value_.measurements = calculator_.snapshot();
    } else {
        pd_channel_online_.fill(false);
        for (std::size_t channel = 0; channel < kChannelCount; ++channel) {
            value_.pd_valid[channel] = false;
            invalidate_pd(value_.pd[channel]);
        }
    }
    recompute_alarms();
    ++value_.generation;
}

void SnapshotStore::record_pd_failure(std::size_t channel,
                                      std::chrono::steady_clock::time_point at,
                                      std::string error) {
    if (channel >= kChannelCount) {
        record_failure(Source::pd, at, std::move(error));
        return;
    }
    if (error.size() > kMaxErrorBytes) {
        error.resize(kMaxErrorBytes);
    }
    std::lock_guard<std::mutex> lock(mutex_);
    SourceStatus& status = value_.pd_status;
    pd_channel_online_[channel] = false;
    status.online = false;
    status.last_attempt = at;
    status.last_error = std::move(error);
    if (status.consecutive_failures < std::numeric_limits<std::uint32_t>::max()) {
        ++status.consecutive_failures;
    }
    status.communication_alarm = status.consecutive_failures >= alarm_after_failures_;
    value_.pd_valid[channel] = false;
    invalidate_pd(value_.pd[channel]);
    recompute_alarms();
    ++value_.generation;
}

void SnapshotStore::update_alarm_thresholds(AlarmThresholds thresholds) {
    std::lock_guard<std::mutex> lock(mutex_);
    thresholds_ = std::move(thresholds);
    recompute_alarms();
    ++value_.generation;
}

void SnapshotStore::recompute_alarms() noexcept {
    value_.alarm_active.reset();
    value_.alarm_valid.reset();
    auto set_alarm = [this](std::size_t index, float value, bool valid) {
        if (index >= kAlarmCount || !std::isfinite(thresholds_.values[index]) || !valid) {
            return;
        }
        value_.alarm_valid.set(index);
        value_.alarm_active.set(index, value > thresholds_.values[index]);
    };
    for (std::size_t channel = 0U; channel < kChannelCount; ++channel) {
        const PdChannel& pd = value_.pd[channel];
        set_alarm(channel, pd.features[2].value,
                  value_.pd_valid[channel] && pd.features[2].valid);
    }
    for (std::size_t index = 0U; index < 6U; ++index) {
        set_alarm(3U + index, value_.current[index].value, value_.current[index].valid());
    }
    for (std::size_t index = 0U; index < 3U; ++index) {
        set_alarm(9U + index, value_.temperature[index].value,
                  value_.temperature[index].valid());
    }
}

UnifiedSnapshot SnapshotStore::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return value_;
}

PortCollector::PortCollector(ISerialPort& port, SnapshotStore& snapshots,
                             IClock& clock, CollectorOptions options)
    : port_(port), snapshots_(snapshots), clock_(clock), options_(std::move(options)) {}

bool PortCollector::read_exact(
    std::uint8_t* data, std::size_t size,
    std::chrono::steady_clock::time_point deadline) {
    std::size_t offset = 0U;
    while (offset < size) {
        const auto now = clock_.now();
        if (now >= deadline) {
            return false;
        }
        auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
        if (remaining.count() <= 0) {
            remaining = std::chrono::milliseconds(1);
        }
        std::size_t received = 0U;
        if (!port_.read_some(data + offset, size - offset, remaining, received)) {
            return false;
        }
        if (received > size - offset) {
            return false;
        }
        offset += received;
    }
    return true;
}

void PortCollector::quarantine(std::chrono::milliseconds duration) {
    const auto deadline = clock_.now() + duration;
    std::array<std::uint8_t, 256U> discarded{};
    while (clock_.now() < deadline) {
        auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - clock_.now());
        if (remaining.count() <= 0) {
            remaining = std::chrono::milliseconds(1);
        }
        std::size_t received = 0U;
        if (!port_.read_some(discarded.data(), discarded.size(), remaining, received)) {
            return;
        }
        if (received == 0U) {
            clock_.sleep_for(std::chrono::milliseconds(1));
        }
    }
}

PortCollector::ReadResult PortCollector::read_registers(
    const ReadRequest& request, std::vector<std::uint16_t>& registers) {
    const CollectorOptions options = options_;
    for (std::uint16_t attempt = 0U;
         attempt <= static_cast<std::uint16_t>(options.max_retries); ++attempt) {
        if (attempt > 0U) {
            clock_.sleep_for(options.retry_delay);
        }
        if (!port_.write_all(request.wire_frame.data(), request.wire_frame.size())) {
            quarantine(options.quarantine_duration);
            continue;
        }
        const auto deadline = clock_.now() + options.response_timeout;
        std::array<std::uint8_t, kMaxFrameBytes> response{};
        if (!read_exact(response.data(), kResponseHeaderBytes, deadline)) {
            quarantine(options.quarantine_duration);
            continue;
        }
        std::size_t response_size = kResponseHeaderBytes;
        const bool exception = response[1] ==
            static_cast<std::uint8_t>(request.function | 0x80U);
        if (exception) {
            response_size = kResponseHeaderBytes + kResponseCrcBytes;
        } else {
            const std::size_t data_bytes = static_cast<std::size_t>(response[2]);
            if (data_bytes != static_cast<std::size_t>(request.register_count) * 2U ||
                data_bytes + response_size + kResponseCrcBytes > response.size()) {
                quarantine(options.quarantine_duration);
                continue;
            }
            response_size += data_bytes + kResponseCrcBytes;
        }
        if (!read_exact(response.data() + kResponseHeaderBytes,
                        response_size - kResponseHeaderBytes, deadline)) {
            quarantine(options.quarantine_duration);
            continue;
        }
        const ReadReply decoded = decode_read_reply(request, response.data(), response_size);
        if (decoded.ok()) {
            registers = decoded.registers;
            return ReadResult::complete;
        }
        quarantine(options.quarantine_duration);
    }
    return ReadResult::failed;
}

bool PortCollector::fail(Source source, std::string message) {
    last_error_ = std::move(message);
    snapshots_.record_failure(source, clock_.now(), last_error_);
    return false;
}

bool PortCollector::poll_pd_channel(std::size_t channel) {
    std::lock_guard<std::mutex> lock(mutex_);
    last_error_.clear();
    if (channel < 1U || channel > kChannelCount) {
        return fail(Source::pd, "PD channel must be 1..3");
    }
    ChannelRegisters raw{};
    RegisterValidity received;
    const auto plan = pd_request_plan(channel, options_.slave_id);
    for (std::size_t index = 0U; index < plan.size(); ++index) {
        if (index > 0U) {
            clock_.sleep_for(options_.pd_segment_interval);
        }
        std::vector<std::uint16_t> registers;
        if (read_registers(plan[index], registers) != ReadResult::complete) {
            last_error_ = "PD channel segment read failed";
            snapshots_.record_pd_failure(channel - 1U, clock_.now(), last_error_);
            return false;
        }
        const std::size_t offset = static_cast<std::size_t>(
            plan[index].start_address - kChannelStarts[channel - 1U]);
        std::copy(registers.begin(), registers.end(), raw.begin() +
                  static_cast<std::ptrdiff_t>(offset));
        for (std::size_t i = 0U; i < registers.size(); ++i) {
            received.set(offset + i);
        }
    }
    snapshots_.publish_pd_channel(channel - 1U, decode_pd_channel(raw, received), clock_.now());
    return true;
}

bool PortCollector::poll_pd_all() {
    bool success = true;
    for (std::size_t channel = 1U; channel <= kChannelCount; ++channel) {
        if (!poll_pd_channel(channel)) {
            success = false;
        }
    }
    return success;
}

bool PortCollector::poll_current() {
    std::lock_guard<std::mutex> lock(mutex_);
    last_error_.clear();
    std::vector<std::uint16_t> registers;
    if (read_registers(current_request(options_.slave_id), registers) != ReadResult::complete ||
        registers.size() != 8U) {
        return fail(Source::current, "current device read failed");
    }
    std::array<std::uint16_t, 8> words{};
    std::copy(registers.begin(), registers.end(), words.begin());
    snapshots_.publish_current(current_values_with_quality(current_values(
        words, options_.current_encoding, options_.current_scale)), clock_.now());
    return true;
}

bool PortCollector::poll_temperature() {
    std::lock_guard<std::mutex> lock(mutex_);
    last_error_.clear();
    std::vector<std::uint16_t> registers;
    if (read_registers(temperature_request(options_.slave_id), registers) !=
            ReadResult::complete || registers.size() != 6U) {
        return fail(Source::temperature, "temperature device read failed");
    }
    std::array<std::uint16_t, 6> words{};
    std::copy(registers.begin(), registers.end(), words.begin());
    snapshots_.publish_temperature(temperature_values_with_quality(
        temperature_values(words, options_.temperature_scale)), clock_.now());
    return true;
}

void PortCollector::update_options(CollectorOptions options) {
    std::lock_guard<std::mutex> lock(mutex_);
    options_ = std::move(options);
}

CollectorOptions PortCollector::options() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return options_;
}

const std::string& PortCollector::last_error() const noexcept { return last_error_; }

AcquisitionScheduler::AcquisitionScheduler(
    ISerialPort& pd_port, ISerialPort& current_port, ISerialPort& temperature_port,
    SnapshotStore& snapshots, SchedulerOptions options)
    : pd_port_(pd_port), current_port_(current_port), temperature_port_(temperature_port),
      snapshots_(snapshots), options_(std::move(options)),
      owned_clock_(std::make_unique<SteadyClock>()), clock_(*owned_clock_) {
    pd_collector_ = std::make_unique<PortCollector>(pd_port_, snapshots_, clock_, options_.collector);
    current_collector_ = std::make_unique<PortCollector>(current_port_, snapshots_, clock_, options_.collector);
    temperature_collector_ = std::make_unique<PortCollector>(temperature_port_, snapshots_, clock_, options_.collector);
}

AcquisitionScheduler::AcquisitionScheduler(
    ISerialPort& pd_port, ISerialPort& current_port, ISerialPort& temperature_port,
    SnapshotStore& snapshots, IClock& clock, SchedulerOptions options)
    : pd_port_(pd_port), current_port_(current_port), temperature_port_(temperature_port),
      snapshots_(snapshots), options_(std::move(options)), owned_clock_(nullptr), clock_(clock) {
    pd_collector_ = std::make_unique<PortCollector>(pd_port_, snapshots_, clock_, options_.collector);
    current_collector_ = std::make_unique<PortCollector>(current_port_, snapshots_, clock_, options_.collector);
    temperature_collector_ = std::make_unique<PortCollector>(temperature_port_, snapshots_, clock_, options_.collector);
}

AcquisitionScheduler::~AcquisitionScheduler() { stop(); }

void AcquisitionScheduler::start() {
    if (running_.exchange(true)) {
        return;
    }
    stop_requested_.store(false);
    pd_worker_ = std::make_unique<std::thread>(&AcquisitionScheduler::run_pd, this);
    current_worker_ = std::make_unique<std::thread>(&AcquisitionScheduler::run_current, this);
    temperature_worker_ = std::make_unique<std::thread>(&AcquisitionScheduler::run_temperature, this);
}

void AcquisitionScheduler::stop() noexcept {
    stop_requested_.store(true);
    for (std::unique_ptr<std::thread>* worker : {&pd_worker_, &current_worker_, &temperature_worker_}) {
        if (*worker && (*worker)->joinable()) {
            (*worker)->join();
        }
        worker->reset();
    }
    running_.store(false);
}

bool AcquisitionScheduler::running() const noexcept { return running_.load(); }

void AcquisitionScheduler::wait_period(
    std::chrono::milliseconds period, std::chrono::steady_clock::time_point started) {
    const auto deadline = started + period;
    while (!stop_requested_.load()) {
        const auto now = clock_.now();
        if (now >= deadline) {
            return;
        }
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
        clock_.sleep_for(std::min(remaining, std::chrono::milliseconds(50)));
    }
}

void AcquisitionScheduler::run_pd() {
    while (!stop_requested_.load()) {
        const auto started = clock_.now();
        (void)pd_collector_->poll_pd_all();
        wait_period(options_.pd_period, started);
    }
}

void AcquisitionScheduler::run_current() {
    while (!stop_requested_.load()) {
        const auto started = clock_.now();
        (void)current_collector_->poll_current();
        wait_period(options_.current_period, started);
    }
}

void AcquisitionScheduler::run_temperature() {
    while (!stop_requested_.load()) {
        const auto started = clock_.now();
        (void)temperature_collector_->poll_temperature();
        wait_period(options_.temperature_period, started);
    }
}

}  // namespace uhf::v3
