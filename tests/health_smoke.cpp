// SPDX-License-Identifier: GPL-3.0-only
#include "health/health.hpp"

#include <chrono>
#include <cstdint>
#include <iostream>
#include <string>

namespace {

using Clock = std::chrono::steady_clock;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "health smoke failed: " << message << '\n';
        return false;
    }
    return true;
}

uhf::health::Input healthy_input(Clock::time_point last_success) {
    uhf::health::Input input;
    input.last_acquisition_success = last_success;
    input.acquisition_last_cycle_ok = true;
    input.storage_writable = true;
    input.modbus_tcp_listening = true;
    input.modbus_rtu_ready = true;
    input.iec61850_enabled = true;
    return input;
}

}  // namespace

int main() {
    const Clock::time_point now = Clock::time_point(std::chrono::seconds(100));
    const uhf::health::Aggregator aggregator;

    const uhf::health::Report no_snapshot = aggregator.evaluate({}, now);
    if (!expect(no_snapshot.overall == uhf::health::State::down, "no snapshot is down") ||
        !expect(no_snapshot.acquisition == uhf::health::State::down, "acquisition down") ||
        !expect(no_snapshot.iec61850 == uhf::health::State::disabled, "IEC disabled explicit") ||
        !expect(no_snapshot.to_json().find("\"status\":\"down\"") != std::string::npos,
            "down JSON")) {
        return 1;
    }

    const uhf::health::Report healthy = aggregator.evaluate(
        healthy_input(now - std::chrono::seconds(1)), now);
    if (!expect(healthy.overall == uhf::health::State::up, "healthy overall") ||
        !expect(healthy.acquisition == uhf::health::State::up, "healthy acquisition") ||
        !expect(healthy.acquisition_age_known && healthy.acquisition_age_ms == 1000U,
            "acquisition age")) {
        return 1;
    }

    uhf::health::Input degraded_input = healthy_input(now - std::chrono::seconds(7));
    degraded_input.storage_low_watermark = true;
    const uhf::health::Report degraded = aggregator.evaluate(degraded_input, now);
    if (!expect(degraded.overall == uhf::health::State::degraded, "stale overall degraded") ||
        !expect(degraded.acquisition == uhf::health::State::degraded, "stale acquisition") ||
        !expect(degraded.storage == uhf::health::State::degraded, "low storage degraded")) {
        return 1;
    }

    uhf::health::Input down_input = healthy_input(now - std::chrono::seconds(31));
    down_input.storage_writable = false;
    const uhf::health::Report down = aggregator.evaluate(down_input, now);
    if (!expect(down.overall == uhf::health::State::down, "old snapshot down") ||
        !expect(down.storage == uhf::health::State::down, "unwritable storage down") ||
        !expect(down.acquisition_age_known && down.acquisition_age_ms == 31'000U,
            "down acquisition age")) {
        return 1;
    }

    std::cout << "health smoke: OK\n";
    return 0;
}
