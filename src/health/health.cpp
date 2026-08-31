// SPDX-License-Identifier: GPL-3.0-only
#include "health/health.hpp"

#include <algorithm>
#include <limits>

namespace {

int state_rank(uhf::health::State state) noexcept {
    switch (state) {
    case uhf::health::State::up:
        return 0;
    case uhf::health::State::disabled:
        return 1;
    case uhf::health::State::degraded:
        return 2;
    case uhf::health::State::down:
        return 3;
    }
    return 3;
}

uhf::health::State worst_state(
    uhf::health::State first, uhf::health::State second) noexcept {
    return state_rank(first) >= state_rank(second) ? first : second;
}

}  // namespace

namespace uhf::health {

std::string_view state_name(State state) noexcept {
    switch (state) {
    case State::up:
        return "up";
    case State::degraded:
        return "degraded";
    case State::down:
        return "down";
    case State::disabled:
        return "disabled";
    }
    return "down";
}

std::string Report::to_json() const {
    return "{\"status\":\"" + std::string(state_name(overall)) +
        "\",\"acquisition\":{\"status\":\"" +
        std::string(state_name(acquisition)) + "\",\"age_ms\":" +
        (acquisition_age_known ? std::to_string(acquisition_age_ms) : "null") +
        "},\"storage\":{\"status\":\"" + std::string(state_name(storage)) +
        "\"},\"modbus_tcp\":{\"status\":\"" +
        std::string(state_name(modbus_tcp)) + "\"},\"modbus_rtu\":{\"status\":\"" +
        std::string(state_name(modbus_rtu)) + "\"},\"iec61850\":{\"status\":\"" +
        std::string(state_name(iec61850)) + "\"}}\n";
}

Aggregator::Aggregator(std::chrono::seconds stale_after, std::chrono::seconds down_after)
    : stale_after_(stale_after), down_after_(down_after) {}

Report Aggregator::evaluate(const Input& input, std::chrono::steady_clock::time_point now) const {
    Report report;
    if (!input.last_acquisition_success) {
        report.acquisition = State::down;
    } else {
        const auto elapsed = now - *input.last_acquisition_success;
        const auto nonnegative_elapsed = std::max(elapsed, std::chrono::steady_clock::duration::zero());
        const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
            nonnegative_elapsed);
        report.acquisition_age_known = true;
        report.acquisition_age_ms = milliseconds.count() < 0
            ? 0U
            : static_cast<std::uint64_t>(milliseconds.count());
        if (nonnegative_elapsed > down_after_) {
            report.acquisition = State::down;
        } else if (nonnegative_elapsed > stale_after_ || !input.acquisition_last_cycle_ok) {
            report.acquisition = State::degraded;
        } else {
            report.acquisition = State::up;
        }
    }

    if (!input.storage_writable) {
        report.storage = State::down;
    } else if (input.storage_low_watermark) {
        report.storage = State::degraded;
    } else {
        report.storage = State::up;
    }
    report.modbus_tcp = input.modbus_tcp_listening ? State::up : State::down;
    report.modbus_rtu = input.modbus_rtu_ready ? State::up : State::down;
    report.iec61850 = input.iec61850_enabled ? State::up : State::disabled;

    report.overall = State::up;
    report.overall = worst_state(report.overall, report.acquisition);
    report.overall = worst_state(report.overall, report.storage);
    report.overall = worst_state(report.overall, report.modbus_tcp);
    report.overall = worst_state(report.overall, report.modbus_rtu);
    report.overall = worst_state(report.overall, report.iec61850);
    if (report.overall == State::disabled) {
        report.overall = State::degraded;
    }
    return report;
}

}  // namespace uhf::health
