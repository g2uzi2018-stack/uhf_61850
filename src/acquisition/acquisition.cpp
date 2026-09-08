// SPDX-License-Identifier: GPL-3.0-only
#include "acquisition/acquisition.hpp"

#include "domain/modbus.hpp"

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <fcntl.h>
#include <limits>
#include <poll.h>
#include <stdexcept>
#include <string>
#include <string_view>
#include <termios.h>
#include <thread>
#include <unistd.h>
#include <utility>

namespace {

constexpr std::size_t kRequestFrameBytes = 8U;
constexpr std::size_t kResponseHeaderBytes = 3U;
constexpr std::size_t kResponseCrcBytes = 2U;
constexpr std::size_t kMaxAcquisitionErrorBytes = 256U;
constexpr speed_t kSerialSpeed = B115200;

int timeout_milliseconds(std::chrono::milliseconds timeout) {
    if (timeout.count() <= 0) {
        return 0;
    }
    const auto count = timeout.count();
    const auto maximum = static_cast<std::chrono::milliseconds::rep>(
        std::numeric_limits<int>::max());
    if (count >= maximum) {
        return std::numeric_limits<int>::max();
    }
    return static_cast<int>(count);
}

}  // namespace

namespace uhf::acquisition {

std::string_view availability_name(Availability availability) noexcept {
    switch (availability) {
    case Availability::fresh:
        return "fresh";
    case Availability::stale:
        return "stale";
    case Availability::invalid:
        return "invalid";
    }
    return "invalid";
}

PosixSerialPort::PosixSerialPort(const std::string& device) : file_descriptor_(-1) {
    file_descriptor_ = ::open(device.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if (file_descriptor_ < 0) {
        throw std::runtime_error("unable to open serial device");
    }

    termios attributes{};
    if (::tcgetattr(file_descriptor_, &attributes) < 0) {
        ::close(file_descriptor_);
        throw std::runtime_error("unable to read serial settings");
    }
    ::cfmakeraw(&attributes);
    attributes.c_cflag = static_cast<tcflag_t>(
        (attributes.c_cflag & static_cast<tcflag_t>(~(CSIZE | PARENB | CSTOPB | CRTSCTS))) |
        CS8 | CLOCAL | CREAD);
    if (::cfsetispeed(&attributes, kSerialSpeed) < 0 ||
        ::cfsetospeed(&attributes, kSerialSpeed) < 0) {
        ::close(file_descriptor_);
        throw std::runtime_error("unable to set serial speed");
    }
    if (::tcsetattr(file_descriptor_, TCSANOW, &attributes) < 0) {
        ::close(file_descriptor_);
        throw std::runtime_error("unable to apply serial settings");
    }
    if (::tcflush(file_descriptor_, TCIFLUSH) < 0) {
        ::close(file_descriptor_);
        throw std::runtime_error("unable to flush serial input");
    }
}

PosixSerialPort::~PosixSerialPort() {
    if (file_descriptor_ >= 0) {
        ::close(file_descriptor_);
    }
}

bool PosixSerialPort::write_all(const std::uint8_t* data, std::size_t size) {
    std::size_t written = 0;
    while (written < size) {
        const ssize_t result = ::write(file_descriptor_, data + written, size - written);
        if (result > 0) {
            written += static_cast<std::size_t>(result);
            continue;
        }
        if (result < 0 && errno == EINTR) {
            continue;
        }
        if (result < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            pollfd descriptor{file_descriptor_, POLLOUT, 0};
            if (::poll(&descriptor, 1, 1000) <= 0) {
                return false;
            }
            continue;
        }
        return false;
    }
    return true;
}

bool PosixSerialPort::read_some(
    std::uint8_t* data,
    std::size_t capacity,
    std::chrono::milliseconds timeout,
    std::size_t& received) {
    received = 0;
    if (capacity == 0U) {
        return true;
    }
    pollfd descriptor{file_descriptor_, POLLIN, 0};
    const int result = ::poll(&descriptor, 1, timeout_milliseconds(timeout));
    if (result == 0) {
        return true;
    }
    if (result < 0) {
        return errno == EINTR;
    }
    if ((descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
        return false;
    }

    const ssize_t read_count = ::read(file_descriptor_, data, capacity);
    if (read_count > 0) {
        received = static_cast<std::size_t>(read_count);
        return true;
    }
    if (read_count < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) {
        return true;
    }
    return false;
}

void SnapshotStore::publish(
    domain::ParsedSnapshot payload,
    std::chrono::steady_clock::time_point started_at,
    std::chrono::steady_clock::time_point completed_at) {
    PublishedSnapshot published;
    published.started_at = started_at;
    published.completed_at = completed_at;
    published.poll_duration = std::chrono::duration_cast<std::chrono::milliseconds>(
        completed_at - started_at);
    published.payload = std::move(payload);

    std::lock_guard<std::mutex> lock(mutex_);
    published.generation = next_generation_++;
    latest_ = std::move(published);
    status_.availability = Availability::fresh;
    status_.attempt_known = true;
    status_.last_attempt_at = completed_at;
    status_.last_error.clear();
    status_.consecutive_no_response = 0U;
    status_.communication_alarm = false;
}

void SnapshotStore::record_failure(
    std::chrono::steady_clock::time_point attempted_at,
    std::string error,
    FailureReason reason) {
    if (error.size() > kMaxAcquisitionErrorBytes) {
        error.resize(kMaxAcquisitionErrorBytes);
    }
    std::lock_guard<std::mutex> lock(mutex_);
    status_.availability = latest_ ? Availability::stale : Availability::invalid;
    status_.attempt_known = true;
    status_.last_attempt_at = attempted_at;
    status_.last_error = std::move(error);
    if (reason == FailureReason::no_response) {
        if (status_.consecutive_no_response < std::numeric_limits<std::uint32_t>::max()) {
            ++status_.consecutive_no_response;
        }
    } else {
        status_.consecutive_no_response = 0U;
    }
    status_.communication_alarm = status_.consecutive_no_response >= 3U;
}

std::optional<PublishedSnapshot> SnapshotStore::latest() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return latest_;
}

ServingView SnapshotStore::serving_view() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return ServingView{latest_, status_};
}

AcquisitionEngine::AcquisitionEngine(
    ISerialPort& serial_port, SnapshotStore& snapshot_store, AcquisitionOptions options)
    : serial_port_(serial_port), snapshot_store_(snapshot_store), options_(options) {}

bool AcquisitionEngine::read_exact(
    std::uint8_t* data,
    std::size_t size,
    std::chrono::steady_clock::time_point deadline,
    std::size_t* received_total) {
    std::size_t offset = 0;
    if (received_total != nullptr) {
        *received_total = 0U;
    }
    while (offset < size) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            return false;
        }
        auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
        if (remaining.count() <= 0) {
            remaining = std::chrono::milliseconds(1);
        }
        std::size_t received = 0;
        if (!serial_port_.read_some(data + offset, size - offset, remaining, received)) {
            return false;
        }
        if (received == 0U) {
            return false;
        }
        offset += received;
        if (received_total != nullptr) {
            *received_total = offset;
        }
    }
    return true;
}

AcquisitionEngine::ResponseResult AcquisitionEngine::read_response(
    const domain::ModbusReadRequest& request,
    const AcquisitionOptions& options,
    std::array<std::uint8_t, 3U + 240U + 2U>& response,
    std::size_t& response_size,
    std::chrono::steady_clock::time_point deadline) {
    response_size = 0;
    std::array<std::uint8_t, kResponseHeaderBytes> response_header{};
    std::size_t header_received = 0U;
    if (!read_exact(
            response_header.data(), response_header.size(), deadline, &header_received)) {
        return header_received == 0U ? ResponseResult::no_response
                                     : ResponseResult::fatal_error;
    }
    std::copy(response_header.begin(), response_header.end(), response.begin());
    response_size = response_header.size();

    const std::size_t expected_data_bytes =
        static_cast<std::size_t>(request.register_count) * 2U;
    if (response_header[0] != options.slave_id) {
        return ResponseResult::fatal_error;
    }
    if (response_header[1] == static_cast<std::uint8_t>(
                                domain::kReadInputRegistersFunction | 0x80U)) {
        response_size = 5U;
        if (!read_exact(response.data() + 3U, 2U, deadline)) {
            return ResponseResult::fatal_error;
        }
        const std::uint16_t expected_crc =
            domain::modbus_crc16(response.data(), response_size - 2U);
        const std::uint16_t received_crc = static_cast<std::uint16_t>(
            response[response_size - 2U] |
            static_cast<std::uint16_t>(response[response_size - 1U]) << 8U);
        if (expected_crc != received_crc) {
            return ResponseResult::retryable_error;
        }
        return ResponseResult::retryable_error;
    }
    if (response_header[1] != domain::kReadInputRegistersFunction ||
        response_header[2] != expected_data_bytes) {
        return ResponseResult::fatal_error;
    }

    response_size = kResponseHeaderBytes + expected_data_bytes + kResponseCrcBytes;
    if (!read_exact(
            response.data() + kResponseHeaderBytes,
            expected_data_bytes + kResponseCrcBytes,
            deadline)) {
        return ResponseResult::fatal_error;
    }
    const std::uint16_t expected_crc =
        domain::modbus_crc16(response.data(), response_size - 2U);
    const std::uint16_t received_crc = static_cast<std::uint16_t>(
        response[response_size - 2U] |
        static_cast<std::uint16_t>(response[response_size - 1U]) << 8U);
    return expected_crc == received_crc ? ResponseResult::complete
                                         : ResponseResult::retryable_error;
}

bool AcquisitionEngine::sleep_until(
    std::chrono::steady_clock::time_point deadline, std::chrono::microseconds duration) {
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
        return false;
    }
    const auto remaining = std::chrono::duration_cast<std::chrono::microseconds>(deadline - now);
    std::this_thread::sleep_for(std::min(duration, remaining));
    return std::chrono::steady_clock::now() < deadline;
}

void AcquisitionEngine::quarantine(std::chrono::milliseconds duration) {
    auto quiet_deadline = std::chrono::steady_clock::now() + duration;
    std::array<std::uint8_t, 256U> discarded{};
    while (std::chrono::steady_clock::now() < quiet_deadline) {
        const auto now = std::chrono::steady_clock::now();
        auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(quiet_deadline - now);
        if (remaining.count() <= 0) {
            remaining = std::chrono::milliseconds(1);
        }
        std::size_t received = 0;
        if (serial_port_.read_some(discarded.data(), discarded.size(), remaining, received) &&
            received > 0U) {
            quiet_deadline = std::chrono::steady_clock::now() + duration;
        } else if (received == 0U) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
}

bool AcquisitionEngine::fail_and_quarantine(
    std::string message, std::chrono::milliseconds duration, FailureReason reason) {
    last_error_ = std::move(message);
    snapshot_store_.record_failure(std::chrono::steady_clock::now(), last_error_, reason);
    quarantine(duration);
    return false;
}

bool AcquisitionEngine::fail(std::string message, FailureReason reason) {
    last_error_ = std::move(message);
    snapshot_store_.record_failure(std::chrono::steady_clock::now(), last_error_, reason);
    return false;
}

bool AcquisitionEngine::poll_once() {
    last_error_.clear();
    AcquisitionOptions options;
    {
        std::lock_guard<std::mutex> lock(options_mutex_);
        options = options_;
    }
    const auto started_at = std::chrono::steady_clock::now();
    const auto deadline = started_at + options.cycle_deadline;
    const auto plan = domain::pd1000_request_plan(options.slave_id);
    std::array<std::uint16_t, domain::kPd1000RegisterCount> raw_registers{};

    for (std::size_t plan_index = 0; plan_index < plan.size(); ++plan_index) {
        const domain::ModbusReadRequest& request = plan[plan_index];
        if (request.slave_id != options.slave_id) {
            return fail_and_quarantine(
                "request plan slave ID does not match acquisition configuration",
                options.quarantine_duration);
        }

        bool complete = false;
        std::string retry_error;
        for (std::uint16_t attempt = 0;
             attempt <= static_cast<std::uint16_t>(options.max_retries);
             ++attempt) {
            if (attempt > 0U) {
                if (!sleep_until(
                        deadline,
                        std::chrono::duration_cast<std::chrono::microseconds>(
                        options.retry_delay))) {
                    return fail_and_quarantine(
                        "poll cycle deadline reached during retry delay",
                        options.quarantine_duration);
                }
            } else if (plan_index != 0U &&
                       !sleep_until(deadline, options.inter_frame_silence)) {
                return fail_and_quarantine(
                    "poll cycle deadline reached during frame silence",
                    options.quarantine_duration);
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                return fail_and_quarantine(
                    "poll cycle deadline reached before request", options.quarantine_duration);
            }
            if (!serial_port_.write_all(request.wire_frame.data(), kRequestFrameBytes)) {
                return fail_and_quarantine(
                    "unable to write Modbus request", options.quarantine_duration);
            }

            const auto response_deadline = std::min(
                deadline, std::chrono::steady_clock::now() + options.response_timeout);
            std::array<std::uint8_t, 3U + 240U + 2U> response{};
            std::size_t response_size = 0;
            const ResponseResult result =
                read_response(request, options, response, response_size, response_deadline);
            if (result == ResponseResult::complete) {
                const std::size_t start_index = static_cast<std::size_t>(
                    request.start_address - domain::kPd1000FirstAddress);
                for (std::size_t register_index = 0; register_index < request.register_count;
                     ++register_index) {
                    const std::size_t byte_index = kResponseHeaderBytes + register_index * 2U;
                    raw_registers[start_index + register_index] = static_cast<std::uint16_t>(
                        static_cast<std::uint16_t>(response[byte_index]) << 8U |
                        static_cast<std::uint16_t>(response[byte_index + 1U]));
                }
                complete = true;
                break;
            }
            if (result == ResponseResult::no_response) {
                return fail_and_quarantine(
                    "Modbus response timeout with no data",
                    options.quarantine_duration,
                    FailureReason::no_response);
            }
            if (result == ResponseResult::fatal_error) {
                return fail_and_quarantine(
                    "Modbus response boundary or timeout error", options.quarantine_duration);
            }
            retry_error = "Modbus response CRC or exception error";
        }
        if (!complete) {
            return fail_and_quarantine(
                retry_error.empty() ? "Modbus retry budget exhausted" : retry_error,
                options.quarantine_duration);
        }
    }

    const auto completed_at = std::chrono::steady_clock::now();
    if (completed_at > deadline) {
        return fail_and_quarantine(
            "poll cycle deadline reached after response", options.quarantine_duration);
    }
    snapshot_store_.publish(
        domain::parse_pd1000_registers(raw_registers), started_at, completed_at);
    return true;
}

void AcquisitionEngine::update_options(AcquisitionOptions options) {
    std::lock_guard<std::mutex> lock(options_mutex_);
    options_ = options;
}

AcquisitionOptions AcquisitionEngine::options() const {
    std::lock_guard<std::mutex> lock(options_mutex_);
    return options_;
}

const std::string& AcquisitionEngine::last_error() const noexcept {
    return last_error_;
}

}  // namespace uhf::acquisition
