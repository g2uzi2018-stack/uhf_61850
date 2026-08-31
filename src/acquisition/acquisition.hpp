// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "domain/snapshot.hpp"
#include "domain/modbus.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace uhf::acquisition {

class ISerialPort {
public:
    virtual ~ISerialPort() = default;

    virtual bool write_all(const std::uint8_t* data, std::size_t size) = 0;
    virtual bool read_some(
        std::uint8_t* data,
        std::size_t capacity,
        std::chrono::milliseconds timeout,
        std::size_t& received) = 0;
};

class PosixSerialPort final : public ISerialPort {
public:
    explicit PosixSerialPort(const std::string& device);
    ~PosixSerialPort() override;

    PosixSerialPort(const PosixSerialPort&) = delete;
    PosixSerialPort& operator=(const PosixSerialPort&) = delete;

    bool write_all(const std::uint8_t* data, std::size_t size) override;
    bool read_some(
        std::uint8_t* data,
        std::size_t capacity,
        std::chrono::milliseconds timeout,
        std::size_t& received) override;

private:
    int file_descriptor_;
};

struct AcquisitionOptions {
    std::uint8_t slave_id{1U};
    std::chrono::milliseconds response_timeout{150};
    std::chrono::milliseconds cycle_deadline{6000};
    std::chrono::microseconds inter_frame_silence{1750};
    std::uint8_t max_retries{3U};
    std::chrono::milliseconds retry_delay{100};
    std::chrono::milliseconds quarantine_duration{6000};
};

struct PublishedSnapshot {
    std::uint64_t generation{0};
    std::chrono::steady_clock::time_point started_at;
    std::chrono::steady_clock::time_point completed_at;
    std::chrono::milliseconds poll_duration{0};
    domain::ParsedSnapshot payload;
};

class SnapshotStore {
public:
    void publish(
        domain::ParsedSnapshot payload,
        std::chrono::steady_clock::time_point started_at,
        std::chrono::steady_clock::time_point completed_at);

    std::optional<PublishedSnapshot> latest() const;

private:
    mutable std::mutex mutex_;
    std::optional<PublishedSnapshot> latest_;
    std::uint64_t next_generation_{1U};
};

class AcquisitionEngine {
public:
    AcquisitionEngine(
        ISerialPort& serial_port, SnapshotStore& snapshot_store, AcquisitionOptions options = {});

    bool poll_once();
    const std::string& last_error() const noexcept;

private:
    enum class ResponseResult {
        complete,
        retryable_error,
        fatal_error,
    };

    bool read_exact(
        std::uint8_t* data,
        std::size_t size,
        std::chrono::steady_clock::time_point deadline);
    ResponseResult read_response(
        const domain::ModbusReadRequest& request,
        std::array<std::uint8_t, 3U + 240U + 2U>& response,
        std::size_t& response_size,
        std::chrono::steady_clock::time_point deadline);
    bool sleep_until(
        std::chrono::steady_clock::time_point deadline, std::chrono::microseconds duration);
    void quarantine();
    bool fail_and_quarantine(std::string message);
    bool fail(std::string message);

    ISerialPort& serial_port_;
    SnapshotStore& snapshot_store_;
    AcquisitionOptions options_;
    std::string last_error_;
};

}  // namespace uhf::acquisition
