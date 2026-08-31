// SPDX-License-Identifier: GPL-3.0-only
#include "acquisition/acquisition.hpp"

#include "domain/modbus.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <iostream>
#include <string>
#include <vector>

namespace {

enum class PortMode {
    timeout,
    short_response,
    crc_once,
    crc_always,
    late_byte,
};

class ScriptedPort final : public uhf::acquisition::ISerialPort {
public:
    explicit ScriptedPort(PortMode mode) : mode_(mode), started_at_(std::chrono::steady_clock::now()) {}

    bool write_all(const std::uint8_t* data, std::size_t size) override {
        if (size != 8U) {
            return false;
        }
        ++write_count_;
        if (mode_ == PortMode::timeout || mode_ == PortMode::late_byte) {
            return true;
        }

        const std::uint16_t register_count = static_cast<std::uint16_t>(
            static_cast<std::uint16_t>(data[4]) << 8U | static_cast<std::uint16_t>(data[5]));
        const std::size_t data_bytes = static_cast<std::size_t>(register_count) * 2U;
        std::vector<std::uint8_t> response(3U + data_bytes + 2U, 0U);
        response[0] = data[0];
        response[1] = data[1];
        response[2] = static_cast<std::uint8_t>(data_bytes);
        const std::uint16_t crc = uhf::domain::modbus_crc16(response.data(), response.size() - 2U);
        response[response.size() - 2U] = static_cast<std::uint8_t>(crc & 0x00FFU);
        response[response.size() - 1U] = static_cast<std::uint8_t>(crc >> 8U);
        if (mode_ == PortMode::short_response && write_count_ == 1U) {
            response.resize(3U);
        } else if (mode_ == PortMode::crc_once && write_count_ == 1U) {
            response.back() ^= 0x01U;
        } else if (mode_ == PortMode::crc_always) {
            response.back() ^= 0x01U;
        }
        for (const std::uint8_t byte : response) {
            pending_bytes_.push_back(byte);
        }
        return true;
    }

    bool read_some(
        std::uint8_t* data,
        std::size_t capacity,
        std::chrono::milliseconds /*timeout*/,
        std::size_t& received) override {
        received = 0;
        if (pending_bytes_.empty()) {
            if (mode_ == PortMode::late_byte && !late_byte_sent_ &&
                std::chrono::steady_clock::now() - started_at_ >= std::chrono::milliseconds(5)) {
                data[0] = 0U;
                received = 1U;
                late_byte_sent_ = true;
            }
            return true;
        }
        const std::size_t count = std::min(capacity, pending_bytes_.size());
        for (std::size_t index = 0; index < count; ++index) {
            data[index] = pending_bytes_.front();
            pending_bytes_.pop_front();
        }
        received = count;
        return true;
    }

    std::size_t write_count() const noexcept { return write_count_; }

private:
    PortMode mode_;
    std::chrono::steady_clock::time_point started_at_;
    std::deque<std::uint8_t> pending_bytes_;
    std::size_t write_count_{0};
    bool late_byte_sent_{false};
};

bool expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "acquisition fault smoke failed: " << message << '\n';
        return false;
    }
    return true;
}

bool run_fatal_case(PortMode mode, std::size_t expected_writes, bool expect_late_extension) {
    ScriptedPort port(mode);
    uhf::acquisition::SnapshotStore store;
    uhf::acquisition::AcquisitionOptions options;
    options.response_timeout = std::chrono::milliseconds(1);
    options.cycle_deadline = std::chrono::milliseconds(100);
    options.retry_delay = std::chrono::milliseconds(1);
    options.inter_frame_silence = std::chrono::microseconds(100);
    options.quarantine_duration = std::chrono::milliseconds(20);
    const auto started = std::chrono::steady_clock::now();
    uhf::acquisition::AcquisitionEngine engine(port, store, options);
    const bool result = engine.poll_once();
    const auto elapsed = std::chrono::steady_clock::now() - started;
    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed);
    return expect(!result, "fatal case unexpectedly published") &&
        expect(port.write_count() == expected_writes, "fatal case retried an unsafe response") &&
        expect(!engine.last_error().empty(), "fatal case did not retain an error") &&
        expect(!store.latest().has_value(), "fatal case published a partial snapshot") &&
        expect(elapsed_ms >= std::chrono::milliseconds(expect_late_extension ? 24 : 19),
            "quarantine window was not enforced");
}

}  // namespace

int main() {
    const uhf::acquisition::AcquisitionOptions defaults;
    if (!expect(defaults.max_retries == 3U, "default retry count") ||
        !expect(defaults.retry_delay == std::chrono::milliseconds(100), "default retry delay") ||
        !expect(defaults.quarantine_duration == std::chrono::milliseconds(6000),
            "default quarantine duration")) {
        return 1;
    }

    if (!run_fatal_case(PortMode::timeout, 1U, false) ||
        !run_fatal_case(PortMode::short_response, 1U, false) ||
        !run_fatal_case(PortMode::late_byte, 1U, true)) {
        return 1;
    }

    {
        ScriptedPort port(PortMode::crc_once);
        uhf::acquisition::SnapshotStore store;
        uhf::acquisition::AcquisitionOptions options;
        options.retry_delay = std::chrono::milliseconds(1);
        options.inter_frame_silence = std::chrono::microseconds(100);
        uhf::acquisition::AcquisitionEngine engine(port, store, options);
        if (!expect(engine.poll_once(), "CRC error was not retried") ||
            !expect(port.write_count() == 32U, "CRC retry did not preserve request order") ||
            !expect(store.latest().has_value(), "CRC retry did not publish complete snapshot")) {
            return 1;
        }
    }

    if (!run_fatal_case(PortMode::crc_always, 4U, false)) {
        return 1;
    }

    std::cout << "acquisition fault smoke: OK\n";
    return 0;
}
