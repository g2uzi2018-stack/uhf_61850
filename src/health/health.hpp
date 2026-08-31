// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace uhf::health {

enum class State {
    up,
    degraded,
    down,
    disabled,
};

struct Input {
    std::optional<std::chrono::steady_clock::time_point> last_acquisition_success;
    bool acquisition_last_cycle_ok{false};
    bool storage_writable{false};
    bool storage_low_watermark{false};
    bool modbus_tcp_listening{false};
    bool modbus_rtu_ready{false};
    bool iec61850_enabled{false};
};

struct Report {
    State overall{State::down};
    State acquisition{State::down};
    State storage{State::down};
    State modbus_tcp{State::down};
    State modbus_rtu{State::down};
    State iec61850{State::disabled};
    bool acquisition_age_known{false};
    std::uint64_t acquisition_age_ms{0U};

    std::string to_json() const;
};

std::string_view state_name(State state) noexcept;

class Aggregator {
public:
    explicit Aggregator(
        std::chrono::seconds stale_after = std::chrono::seconds(6),
        std::chrono::seconds down_after = std::chrono::seconds(30));

    Report evaluate(const Input& input, std::chrono::steady_clock::time_point now) const;

private:
    std::chrono::seconds stale_after_;
    std::chrono::seconds down_after_;
};

}  // namespace uhf::health
