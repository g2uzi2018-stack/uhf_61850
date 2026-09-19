// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "v3/measurements.hpp"
#include "v3/protocol.hpp"

#include <array>
#include <atomic>
#include <bitset>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <limits>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace uhf::v3 {

/**
 * Small transport interface used by the v3 collectors.  The production
 * adapter is deliberately outside this module so host tests can use PTYs or
 * a deterministic fake without changing protocol code.
 */
class ISerialPort {
public:
    virtual ~ISerialPort() = default;
    virtual bool write_all(const std::uint8_t* data, std::size_t size) = 0;
    virtual bool read_some(std::uint8_t* data, std::size_t capacity,
                           std::chrono::milliseconds timeout,
                           std::size_t& received) = 0;
};

class IClock {
public:
    virtual ~IClock() = default;
    virtual std::chrono::steady_clock::time_point now() const = 0;
    virtual std::chrono::system_clock::time_point utc_now() const = 0;
    virtual void sleep_for(std::chrono::steady_clock::duration duration) = 0;
};

class SteadyClock final : public IClock {
public:
    std::chrono::steady_clock::time_point now() const override;
    std::chrono::system_clock::time_point utc_now() const override;
    void sleep_for(std::chrono::steady_clock::duration duration) override;
};

enum class Source { pd, current, temperature };
constexpr std::size_t kAlarmCount = 12U;

struct AlarmThresholds {
    std::array<float, kAlarmCount> values{};
    AlarmThresholds();
};

struct SourceStatus {
    bool has_sample{false};
    bool online{false};
    bool communication_alarm{false};
    bool stale{false};
    std::uint32_t consecutive_failures{0};
    std::chrono::steady_clock::time_point last_attempt{};
    std::chrono::steady_clock::time_point last_success{};
    std::chrono::system_clock::time_point last_attempt_utc{};
    std::chrono::system_clock::time_point last_success_utc{};
    std::string last_error;
};

struct FreshnessLimits {
    std::chrono::milliseconds pd{std::chrono::minutes(10)};
    std::chrono::milliseconds current{std::chrono::seconds(5)};
    std::chrono::milliseconds temperature{std::chrono::seconds(5)};
};

struct UnifiedSnapshot {
    std::uint64_t generation{0};
    std::array<PdChannel, kChannelCount> pd{};
    std::array<bool, kChannelCount> pd_valid{};
    CurrentValues current{};
    TemperatureValues temperature{};
    ValueTable measurements{};
    std::bitset<kAlarmCount> alarm_active{};
    std::bitset<kAlarmCount> alarm_valid{};
    SourceStatus pd_status;
    SourceStatus current_status;
    SourceStatus temperature_status;
};

class SnapshotStore {
public:
    explicit SnapshotStore(std::uint32_t alarm_after_failures = 3U,
                           AlarmThresholds thresholds = {},
                           FreshnessLimits freshness = {});

    void publish_pd_channel(std::size_t channel, PdChannel value,
                            std::chrono::steady_clock::time_point at,
                            std::chrono::system_clock::time_point utc =
                                std::chrono::system_clock::now());
    void publish_current(CurrentValues value,
                         std::chrono::steady_clock::time_point at,
                         std::chrono::system_clock::time_point utc =
                             std::chrono::system_clock::now());
    void publish_temperature(TemperatureValues value,
                             std::chrono::steady_clock::time_point at,
                             std::chrono::system_clock::time_point utc =
                                 std::chrono::system_clock::now());
    void record_pd_failure(std::size_t channel,
                           std::chrono::steady_clock::time_point at,
                           std::string error,
                           std::chrono::system_clock::time_point utc =
                               std::chrono::system_clock::now());
    void record_failure(Source source, std::chrono::steady_clock::time_point at,
                        std::string error,
                        std::chrono::system_clock::time_point utc =
                            std::chrono::system_clock::now());
    void update_alarm_thresholds(AlarmThresholds thresholds);
    UnifiedSnapshot snapshot() const;
    UnifiedSnapshot snapshot(std::chrono::steady_clock::time_point now) const;

private:
    SourceStatus& status_for(UnifiedSnapshot& value, Source source) const noexcept;
    const SourceStatus& status_for(const UnifiedSnapshot& value, Source source) const noexcept;
    void mark_success(Source source, std::chrono::steady_clock::time_point at,
                      std::chrono::system_clock::time_point utc);
    void apply_staleness(UnifiedSnapshot& value,
                         std::chrono::steady_clock::time_point now) const noexcept;
    void recompute_alarms() noexcept;

    mutable std::mutex mutex_;
    UnifiedSnapshot value_;
    std::uint32_t alarm_after_failures_;
    MonitoringCalculator calculator_{WarmupPolicy::use_available,
                                     MeanPolicy::reject_nonpositive};
    std::uint64_t current_sequence_{0};
    std::array<bool, kChannelCount> pd_channel_online_{};
    AlarmThresholds thresholds_;
    FreshnessLimits freshness_;
};

struct CollectorOptions {
    std::uint8_t slave_id{1U};
    std::chrono::milliseconds response_timeout{150};
    std::chrono::milliseconds retry_delay{20};
    std::chrono::milliseconds quarantine_duration{20};
    std::chrono::milliseconds pd_segment_interval{3000};
    std::uint8_t max_retries{3U};
    WordEncoding current_encoding{WordEncoding::unsigned16};
    LinearScale current_scale{
        std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::quiet_NaN()};
    LinearScale temperature_scale{
        std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::quiet_NaN()};
};

class PortCollector {
public:
    PortCollector(ISerialPort& port, SnapshotStore& snapshots,
                  IClock& clock, CollectorOptions options = {});

    // Each method is independent. A failed PD channel does not prevent the
    // next channel, current, or temperature collector from running.
    bool poll_pd_channel(std::size_t channel);
    bool poll_pd_all();
    bool poll_current();
    bool poll_temperature();

    void update_options(CollectorOptions options);
    CollectorOptions options() const;
    const std::string& last_error() const noexcept;

private:
    enum class ReadResult { complete, failed };
    ReadResult read_registers(const ReadRequest& request,
                              std::vector<std::uint16_t>& registers);
    bool read_exact(std::uint8_t* data, std::size_t size,
                    std::chrono::steady_clock::time_point deadline);
    void quarantine(std::chrono::milliseconds duration);
    bool fail(Source source, std::string message);

    ISerialPort& port_;
    SnapshotStore& snapshots_;
    IClock& clock_;
    mutable std::mutex mutex_;
    CollectorOptions options_;
    std::string last_error_;
};

struct SchedulerOptions {
    CollectorOptions collector{};
    std::chrono::milliseconds current_period{1000};
    std::chrono::milliseconds temperature_period{1000};
    std::chrono::milliseconds pd_period{1000};
};

/** Runs one independent worker per physical downstream port. */
class AcquisitionScheduler {
public:
    AcquisitionScheduler(ISerialPort& pd_port, ISerialPort& current_port,
                          ISerialPort& temperature_port, SnapshotStore& snapshots,
                          SchedulerOptions options = {});
    AcquisitionScheduler(ISerialPort& pd_port, ISerialPort& current_port,
                          ISerialPort& temperature_port, SnapshotStore& snapshots,
                          IClock& clock, SchedulerOptions options = {});
    ~AcquisitionScheduler();

    AcquisitionScheduler(const AcquisitionScheduler&) = delete;
    AcquisitionScheduler& operator=(const AcquisitionScheduler&) = delete;

    void start();
    void stop() noexcept;
    bool running() const noexcept;

private:
    void run_pd();
    void run_current();
    void run_temperature();
    void wait_period(std::chrono::milliseconds period,
                     std::chrono::steady_clock::time_point started);

    ISerialPort& pd_port_;
    ISerialPort& current_port_;
    ISerialPort& temperature_port_;
    SnapshotStore& snapshots_;
    SchedulerOptions options_;
    std::unique_ptr<SteadyClock> owned_clock_;
    IClock& clock_;
    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> running_{false};
    std::unique_ptr<PortCollector> pd_collector_;
    std::unique_ptr<PortCollector> current_collector_;
    std::unique_ptr<PortCollector> temperature_collector_;
    std::unique_ptr<std::thread> pd_worker_;
    std::unique_ptr<std::thread> current_worker_;
    std::unique_ptr<std::thread> temperature_worker_;
};

}  // namespace uhf::v3
