// SPDX-License-Identifier: GPL-3.0-only
#include "v3/measurements.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace uhf::v3 {
namespace {
Value invalid(Quality quality) noexcept {
    return {std::numeric_limits<float>::quiet_NaN(), quality};
}
Value number(double value) noexcept {
    if (!std::isfinite(value) || std::fabs(value) > std::numeric_limits<float>::max()) {
        return invalid(Quality::undefined);
    }
    return {static_cast<float>(value), Quality::good};
}
Value mean(Value a, Value b, Value c) noexcept {
    if (!a.valid() || !b.valid() || !c.valid()) { return {}; }
    return number((static_cast<double>(a.value) + b.value + c.value) / 3.0);
}
Value difference(Value a, Value b) noexcept {
    if (!a.valid() || !b.valid()) { return {}; }
    return number(static_cast<double>(a.value) - b.value);
}
Value ratio(Value numerator, Value denominator) noexcept {
    if (!numerator.valid() || !denominator.valid()) { return {}; }
    if (denominator.value == 0) { return invalid(Quality::undefined); }
    return number(static_cast<double>(numerator.value) / denominator.value);
}
Value imbalance(Value a, Value b, Value c, Value average, MeanPolicy policy) noexcept {
    if (!a.valid() || !b.valid() || !c.valid() || !average.valid()) { return {}; }
    double denominator = average.value;
    if (denominator == 0 || (policy == MeanPolicy::reject_nonpositive && denominator <= 0)) {
        return invalid(Quality::undefined);
    }
    if (policy == MeanPolicy::absolute_denominator) { denominator = std::fabs(denominator); }
    const double deviation = std::max({std::fabs(static_cast<double>(a.value) - average.value),
        std::fabs(static_cast<double>(b.value) - average.value),
        std::fabs(static_cast<double>(c.value) - average.value)});
    // Percent points: 25 means 25%, while Qa/Qb/Qc remain plain ratios.
    return number(deviation / denominator * 100.0);
}
}  // namespace
bool Value::valid() const noexcept { return quality == Quality::good && std::isfinite(value); }
Value valid_value(float value) noexcept { return number(value); }
MonitoringCalculator::MonitoringCalculator(WarmupPolicy warmup, MeanPolicy mean_policy) noexcept
    : warmup_(warmup), mean_policy_(mean_policy) {}
void MonitoringCalculator::Window::push(float value) noexcept {
    values[next] = value;
    next = (next + 1) % kWindowSize;
    count = std::min(count + 1, kWindowSize);
}
bool MonitoringCalculator::on_current(std::uint64_t sequence, const CurrentValues& values) noexcept {
    if (have_sequence_ && sequence <= last_sequence_) { return false; }
    have_sequence_ = true;
    last_sequence_ = sequence;
    current_ = values;
    // Last 50 valid readings PER PHASE, not 50 UI requests or wall-clock seconds.
    // Invalid readings do not enter or clear the window; integration must define
    // whether reconnect/reconfiguration should construct a fresh calculator.
    for (std::size_t i = 0; i < 3; ++i) {
        if (values[i].valid()) { windows_[i].push(values[i].value); }
    }
    return true;
}
void MonitoringCalculator::on_temperature(const TemperatureValues& values) noexcept { temperature_ = values; }
void MonitoringCalculator::invalidate_current() noexcept { current_ = {}; }
void MonitoringCalculator::invalidate_temperature() noexcept { temperature_ = {}; }
std::array<std::size_t, 3> MonitoringCalculator::window_samples() const noexcept {
    return {windows_[0].count, windows_[1].count, windows_[2].count};
}
ValueTable MonitoringCalculator::snapshot() const noexcept {
    ValueTable out{};
    std::copy(current_.begin(), current_.end(), out.begin());
    std::copy(temperature_.begin(), temperature_.end(), out.begin() + 8);
    out[11] = mean(out[8], out[9], out[10]);
    out[12] = mean(out[0], out[1], out[2]);
    out[13] = mean(out[3], out[4], out[5]);
    for (std::size_t i = 0; i < 3; ++i) {
        out[14 + i] = ratio(out[i], out[3 + i]);
        out[20 + i] = difference(out[8 + i], out[11]);
        out[32 + i] = difference(out[8 + i], out[8 + (i + 1) % 3]);
        const auto& window = windows_[i];
        if (!current_[i].valid() || window.count == 0) { continue; }
        if (warmup_ == WarmupPolicy::require_50 && window.count < kWindowSize) {
            out[23 + i] = out[26 + i] = out[29 + i] = invalid(Quality::insufficient_samples);
            continue;
        }
        float minimum = window.values[0];
        float maximum = window.values[0];
        for (std::size_t j = 1; j < window.count; ++j) {
            minimum = std::min(minimum, window.values[j]);
            maximum = std::max(maximum, window.values[j]);
        }
        out[26 + i] = valid_value(maximum);
        out[29 + i] = valid_value(minimum);
        out[23 + i] = ratio(out[26 + i], out[29 + i]);
    }
    out[17] = imbalance(out[0], out[1], out[2], out[12], mean_policy_);
    out[18] = imbalance(out[3], out[4], out[5], out[13], mean_policy_);
    out[19] = imbalance(out[8], out[9], out[10], out[11], mean_policy_);
    return out;
}
std::array<std::uint16_t, 2> float32_registers(float value) noexcept {
    static_assert(sizeof(float) == 4 && std::numeric_limits<float>::is_iec559,
                  "v3 wire format requires IEEE 754 binary32");
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    if (std::isnan(value)) { bits = 0x7fc00000U; }
    return {static_cast<std::uint16_t>(bits >> 16U), static_cast<std::uint16_t>(bits & 0xffffU)};
}
}  // namespace uhf::v3
