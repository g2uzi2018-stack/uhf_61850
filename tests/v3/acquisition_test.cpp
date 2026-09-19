// SPDX-License-Identifier: GPL-3.0-only
#define _XOPEN_SOURCE 600

#include "v3/acquisition.hpp"
#include "test_support.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <stdexcept>
#include <string>
#include <termios.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

using uhf::domain::modbus_crc16;
using uhf::v3::ISerialPort;

void make_raw(int descriptor) {
    termios attributes{};
    check(::tcgetattr(descriptor, &attributes) == 0, "tcgetattr");
    ::cfmakeraw(&attributes);
    check(::tcsetattr(descriptor, TCSANOW, &attributes) == 0, "tcsetattr");
}

class PtyPort final : public ISerialPort {
public:
    explicit PtyPort(const std::string& path) : descriptor_(::open(
        path.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC)) {
        check(descriptor_ >= 0, "open PTY slave");
        make_raw(descriptor_);
    }
    ~PtyPort() override { ::close(descriptor_); }
    bool write_all(const std::uint8_t* data, std::size_t size) override {
        std::size_t offset = 0U;
        while (offset < size) {
            const ssize_t written = ::write(descriptor_, data + offset, size - offset);
            if (written > 0) {
                offset += static_cast<std::size_t>(written);
            } else if (written < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) {
                continue;
            } else {
                return false;
            }
        }
        return true;
    }
    bool read_some(std::uint8_t* data, std::size_t capacity,
                   std::chrono::milliseconds timeout, std::size_t& received) override {
        received = 0U;
        pollfd descriptor{descriptor_, POLLIN, 0};
        const int wait = ::poll(&descriptor, 1, static_cast<int>(timeout.count()));
        if (wait <= 0) {
            return wait == 0 || errno == EINTR;
        }
        const ssize_t read_count = ::read(descriptor_, data, capacity);
        if (read_count > 0) {
            received = static_cast<std::size_t>(read_count);
            return true;
        }
        return read_count < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK);
    }
private:
    int descriptor_;
};

enum class DeviceKind { pd, current, temperature };
enum class ReplyFault { none, crc, short_frame, exception, timeout };

class PtyDevice {
public:
    explicit PtyDevice(DeviceKind kind, ReplyFault fault = ReplyFault::none)
        : kind_(kind), fault_(fault), master_(::posix_openpt(
              O_RDWR | O_NOCTTY | O_NONBLOCK)) {
        check(master_ >= 0, "posix_openpt");
        check(::grantpt(master_) == 0 && ::unlockpt(master_) == 0, "grant/unlock PTY");
        char* name = ::ptsname(master_);
        check(name != nullptr, "ptsname");
        slave_path_ = name;
        worker_ = std::thread(&PtyDevice::run, this);
    }
    ~PtyDevice() {
        stop_.store(true);
        if (worker_.joinable()) {
            worker_.join();
        }
        ::close(master_);
    }
    PtyDevice(const PtyDevice&) = delete;
    PtyDevice& operator=(const PtyDevice&) = delete;
    const std::string& slave_path() const noexcept { return slave_path_; }
    std::size_t request_count() const noexcept { return request_count_.load(); }

private:
    std::vector<std::uint16_t> registers(std::uint16_t start, std::uint16_t count) const {
        std::vector<std::uint16_t> result(count, 0U);
        if (kind_ == DeviceKind::pd) {
            const std::uint16_t base = start >= 20001U ? 20001U :
                start >= 15001U ? 15001U : 10001U;
            for (std::size_t i = 0U; i < result.size(); ++i) {
                result[i] = static_cast<std::uint16_t>(start - base + i + 1U);
            }
        } else if (kind_ == DeviceKind::current) {
            for (std::size_t i = 0U; i < result.size(); ++i) {
                result[i] = static_cast<std::uint16_t>(100U * (i + 1U));
            }
        } else {
            result = {10U, 0U, 20U, 0U, 30U, 0U};
        }
        return result;
    }
    void write_bytes(const std::vector<std::uint8_t>& bytes) {
        std::size_t offset = 0U;
        while (offset < bytes.size()) {
            const ssize_t written = ::write(master_, bytes.data() + offset, bytes.size() - offset);
            if (written > 0) {
                offset += static_cast<std::size_t>(written);
            } else if (written < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) {
                std::this_thread::yield();
            } else {
                return;
            }
        }
    }
    void send_reply(const std::array<std::uint8_t, 6>& request) {
        const std::uint16_t start = static_cast<std::uint16_t>(
            static_cast<std::uint16_t>(request[2]) << 8U | request[3]);
        const std::uint16_t count = static_cast<std::uint16_t>(
            static_cast<std::uint16_t>(request[4]) << 8U | request[5]);
        const auto words = registers(start, count);
        if (fault_ == ReplyFault::timeout && !fault_used_) {
            fault_used_ = true;
            return;
        }
        if (fault_ == ReplyFault::exception && !fault_used_) {
            fault_used_ = true;
            std::vector<std::uint8_t> response{request[0],
                                               static_cast<std::uint8_t>(request[1] | 0x80U), 2U};
            const std::uint16_t crc = modbus_crc16(response.data(), response.size());
            response.push_back(static_cast<std::uint8_t>(crc & 255U));
            response.push_back(static_cast<std::uint8_t>(crc >> 8U));
            write_bytes(response);
            return;
        }
        std::vector<std::uint8_t> response{request[0], request[1],
                                           static_cast<std::uint8_t>(count * 2U)};
        for (const std::uint16_t word : words) {
            response.push_back(static_cast<std::uint8_t>(word >> 8U));
            response.push_back(static_cast<std::uint8_t>(word & 255U));
        }
        const std::uint16_t crc = modbus_crc16(response.data(), response.size());
        response.push_back(static_cast<std::uint8_t>(crc & 255U));
        response.push_back(static_cast<std::uint8_t>(crc >> 8U));
        if (fault_ == ReplyFault::short_frame && !fault_used_) {
            fault_used_ = true;
            response.resize(3U);
            write_bytes(response);
            return;
        }
        if (fault_ == ReplyFault::crc && !fault_used_) {
            response.back() ^= 1U;
            fault_used_ = true;
        }
        std::size_t offset = 0U;
        while (offset < response.size()) {
            const ssize_t written = ::write(master_, response.data() + offset,
                                            response.size() - offset);
            if (written > 0) {
                offset += static_cast<std::size_t>(written);
            } else if (written < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) {
                std::this_thread::yield();
            } else {
                return;
            }
        }
    }
    void run() {
        std::array<std::uint8_t, 256> input{};
        std::size_t size = 0U;
        while (!stop_.load()) {
            pollfd descriptor{master_, POLLIN, 0};
            if (::poll(&descriptor, 1, 50) <= 0) {
                continue;
            }
            const ssize_t received = ::read(master_, input.data() + size, input.size() - size);
            if (received <= 0) {
                continue;
            }
            size += static_cast<std::size_t>(received);
            while (size >= 8U) {
                std::array<std::uint8_t, 6> request{};
                std::copy(input.begin(), input.begin() + 6, request.begin());
                std::memmove(input.data(), input.data() + 8U, size - 8U);
                size -= 8U;
                ++request_count_;
                send_reply(request);
            }
        }
    }

    DeviceKind kind_;
    ReplyFault fault_;
    bool fault_used_{false};
    int master_;
    std::string slave_path_;
    std::atomic<bool> stop_{false};
    std::atomic<std::size_t> request_count_{0U};
    std::thread worker_;
};

}  // namespace

int main() {
    try {
        uhf::v3::SnapshotStore snapshots;
        uhf::v3::SteadyClock clock;
        uhf::v3::CollectorOptions options;
        options.response_timeout = std::chrono::milliseconds(100);
        options.retry_delay = std::chrono::milliseconds(1);
        options.quarantine_duration = std::chrono::milliseconds(1);
        options.current_scale = {1.0F, 0.0F};
        options.temperature_scale = {0.1F, 0.0F};

        PtyDevice pd_device(DeviceKind::pd);
        PtyDevice current_device(DeviceKind::current, ReplyFault::crc);
        PtyDevice temperature_device(DeviceKind::temperature);
        PtyPort pd_port(pd_device.slave_path());
        PtyPort current_port(current_device.slave_path());
        PtyPort temperature_port(temperature_device.slave_path());
        {
            uhf::v3::SchedulerOptions scheduler_options;
            scheduler_options.pd_slave_id = 2U;
            scheduler_options.pd_channel_interval = std::chrono::milliseconds(4000);
            scheduler_options.current_period = std::chrono::milliseconds(1100);
            scheduler_options.temperature_period = std::chrono::milliseconds(1200);
            scheduler_options.collector.response_timeout = std::chrono::milliseconds(160);
            scheduler_options.collector.retry_delay = std::chrono::milliseconds(30);
            scheduler_options.collector.quarantine_duration = std::chrono::milliseconds(40);
            scheduler_options.collector.max_retries = 2U;
            uhf::v3::AcquisitionScheduler scheduler(
                pd_port, current_port, temperature_port,
                snapshots, clock, scheduler_options);
            const uhf::v3::UnifiedSnapshot configured = snapshots.snapshot();
            check(configured.pd_status.acquisition.unit_id == 2U &&
                  configured.pd_status.acquisition.poll_interval_ms == 4000U &&
                  configured.current_status.acquisition.poll_interval_ms == 1100U &&
                  configured.temperature_status.acquisition.poll_interval_ms == 1200U &&
                  configured.current_status.acquisition.response_timeout_ms == 160U &&
                  configured.current_status.acquisition.retry_delay_ms == 30U &&
                  configured.current_status.acquisition.late_frame_quarantine_ms == 40U &&
                  configured.current_status.acquisition.max_retries == 2U,
                  "scheduler publishes active acquisition settings");
            scheduler_options.pd_slave_id = 1U;
            scheduler_options.pd_channel_interval = std::chrono::milliseconds(3000);
            scheduler_options.current_period = std::chrono::milliseconds(1300);
            scheduler.update_options(scheduler_options);
            const uhf::v3::SchedulerOptions updated = scheduler.options();
            const uhf::v3::UnifiedSnapshot reconfigured = snapshots.snapshot();
            check(updated.current_period == std::chrono::milliseconds(1300) &&
                  reconfigured.pd_status.acquisition.unit_id == 1U &&
                  reconfigured.pd_status.acquisition.poll_interval_ms == 3000U &&
                  reconfigured.current_status.acquisition.poll_interval_ms == 1300U,
                  "scheduler hot reloads acquisition settings");
        }
        uhf::v3::PortCollector pd(pd_port, snapshots, clock, options);
        uhf::v3::PortCollector current(current_port, snapshots, clock, options);
        uhf::v3::PortCollector temperature(temperature_port, snapshots, clock, options);

        check(pd.poll_pd_all(), "PTY PD three-channel poll");
        check(current.poll_current(), "PTY current poll with CRC retry");
        check(temperature.poll_temperature(), "PTY temperature poll");
        const uhf::v3::UnifiedSnapshot result = snapshots.snapshot();
        check(result.pd_valid[0] && result.pd_valid[1] && result.pd_valid[2] &&
              result.pd[0].received.all(), "complete PD round published");
        near(result.pd[0].features[0].value, 1.0F, "PD feature from PTY");
        check(result.pd[0].spectrum_raw[0] == 16, "PD spectrum from PTY");
        near(result.current[0].value, 100.0F, "current from PTY");
        near(result.current[7].value, 800.0F, "all current channels from PTY");
        near(result.temperature[0].value, 1.0F, "temperature scale from PTY");
        near(result.temperature[2].value, 3.0F, "temperature register selection from PTY");
        check(current_device.request_count() == 2U, "CRC error caused one bounded retry");
        check(result.pd_status.online && result.current_status.online &&
              result.temperature_status.online, "all source statuses recovered");

        snapshots.reset_engineering_values(true, false);
        const uhf::v3::UnifiedSnapshot current_reset = snapshots.snapshot();
        check(!current_reset.current[0].valid() &&
              !current_reset.measurements[0].valid(),
              "current conversion reset invalidates current and derived values");
        check(current_reset.temperature[0].valid() &&
              current_reset.measurements[8].valid(),
              "current conversion reset preserves temperature values");
        check(current.poll_current(), "current republished after conversion reset");
        snapshots.reset_engineering_values(false, true);
        const uhf::v3::UnifiedSnapshot temperature_reset = snapshots.snapshot();
        check(temperature_reset.current[0].valid() &&
              temperature_reset.measurements[0].valid(),
              "temperature conversion reset preserves current values");
        check(!temperature_reset.temperature[0].valid() &&
              !temperature_reset.measurements[8].valid(),
              "temperature conversion reset invalidates temperature values");

        uhf::v3::CollectorOptions invalid_scale = current.options();
        invalid_scale.current_scale = {0.0F, 0.0F};
        current.update_options(invalid_scale);
        check(current.poll_current(), "invalid scale does not terminate collection");
        check(!snapshots.snapshot().current[0].valid(),
              "invalid scale never publishes an engineering value");

        const auto fault_check = [](ReplyFault fault) {
            uhf::v3::SnapshotStore fault_snapshots;
            uhf::v3::SteadyClock fault_clock;
            uhf::v3::CollectorOptions fault_options;
            fault_options.response_timeout = std::chrono::milliseconds(20);
            fault_options.retry_delay = std::chrono::milliseconds(1);
            fault_options.quarantine_duration = std::chrono::milliseconds(1);
            fault_options.max_retries = 1U;
            PtyDevice device(DeviceKind::current, fault);
            PtyPort port(device.slave_path());
            uhf::v3::PortCollector collector(port, fault_snapshots, fault_clock, fault_options);
            check(collector.poll_current(), "collector recovers from one injected frame fault");
            check(device.request_count() == 2U, "one frame fault consumes one retry");
        };
        fault_check(ReplyFault::short_frame);
        fault_check(ReplyFault::exception);
        fault_check(ReplyFault::timeout);
        std::cout << "v3 acquisition: PTY three-port collection and CRC retry passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
