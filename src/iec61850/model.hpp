// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "iec61850_server.h"
#include "iec61850/scl_model.hpp"

#include <array>
#include <cstddef>
#include <string>

namespace uhf::iec61850 {

constexpr std::size_t kMeasurementCount = 5U;

class Model final {
public:
    explicit Model(std::string ied_name);
    explicit Model(SclModelDefinition definition);
    ~Model();

    Model(const Model&) = delete;
    Model& operator=(const Model&) = delete;

    IedModel* raw() const noexcept;
    DataAttribute* measurement_value(std::size_t index) const noexcept;
    DataAttribute* measurement_quality(std::size_t index) const noexcept;
    DataAttribute* measurement_time(std::size_t index) const noexcept;
    bool measurement_integer(std::size_t index) const noexcept;
    DataAttribute* peak_value() const noexcept;
    DataAttribute* peak_quality() const noexcept;
    DataAttribute* peak_time() const noexcept;
    DataAttribute* alarm_value() const noexcept;
    DataAttribute* alarm_quality() const noexcept;
    DataAttribute* alarm_time() const noexcept;
    const SclModelDefinition& definition() const noexcept;

private:
    IedModel* model_{nullptr};
    std::array<DataAttribute*, kMeasurementCount> measurement_values_{};
    std::array<DataAttribute*, kMeasurementCount> measurement_qualities_{};
    std::array<DataAttribute*, kMeasurementCount> measurement_times_{};
    std::array<bool, kMeasurementCount> measurement_integer_{};
    DataAttribute* peak_value_{nullptr};
    DataAttribute* peak_quality_{nullptr};
    DataAttribute* peak_time_{nullptr};
    DataAttribute* alarm_value_{nullptr};
    DataAttribute* alarm_quality_{nullptr};
    DataAttribute* alarm_time_{nullptr};
    SclModelDefinition definition_;
};

}  // namespace uhf::iec61850
