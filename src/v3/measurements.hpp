// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>

namespace uhf::v3 {
constexpr std::size_t kValueCount = 35;
constexpr std::size_t kWindowSize = 50;
constexpr std::array<std::string_view, kValueCount> kValueNames{
    "Ia","Ib","Ic","IA","IB","IC","Id","Ie","TA","TB","TC",
    "TAVG","Iavg","IAVG","Qa","Qb","Qc","In","IN","TN","TAD","TBD","TCD",
    "Ma","Mb","Mc","MXa","MXb","MXc","MNa","MNb","MNc","TAG","TBG","TCG"};
enum class Quality { good, unavailable, undefined, insufficient_samples };
struct Value {
    float value{std::numeric_limits<float>::quiet_NaN()};
    Quality quality{Quality::unavailable};
    bool valid() const noexcept;
};
Value valid_value(float value) noexcept;
using ValueTable = std::array<Value, kValueCount>;
using CurrentValues = std::array<Value, 8>;
using TemperatureValues = std::array<Value, 3>;

// Explicit integration decisions: v3.0 does not settle startup warm-up or
// negative-temperature denominators. No production default is assumed here.
enum class WarmupPolicy { require_50, use_available };
enum class MeanPolicy { literal_signed, absolute_denominator, reject_nonpositive };
class MonitoringCalculator {
public:
    MonitoringCalculator(WarmupPolicy warmup, MeanPolicy mean_policy) noexcept;
    // Sequence numbers are monotonic within this object's lifetime. Duplicate
    // or out-of-order samples are rejected without changing values/history.
    bool on_current(std::uint64_t sequence, const CurrentValues& values) noexcept;
    void on_temperature(const TemperatureValues& values) noexcept;
    void invalidate_current() noexcept;
    void invalidate_temperature() noexcept;
    // Read-only: UI/TCP refreshes and temperature updates cannot advance history.
    ValueTable snapshot() const noexcept;
    std::array<std::size_t, 3> window_samples() const noexcept;
private:
    struct Window {
        std::array<float, kWindowSize> values{};
        std::size_t next{0};
        std::size_t count{0};
        void push(float value) noexcept;
    };
    CurrentValues current_{};
    TemperatureValues temperature_{};
    std::array<Window, 3> windows_{};
    WarmupPolicy warmup_;
    MeanPolicy mean_policy_;
    bool have_sequence_{false};
    std::uint64_t last_sequence_{0};
};
std::array<std::uint16_t, 2> float32_registers(float value) noexcept;
}  // namespace uhf::v3
