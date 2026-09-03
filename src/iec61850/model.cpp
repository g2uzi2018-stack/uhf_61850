// SPDX-License-Identifier: GPL-3.0-only
#include "iec61850/model.hpp"

#include <stdexcept>
#include <utility>

namespace {

ModelNode* as_model_node(LogicalNode* node) noexcept {
    return reinterpret_cast<ModelNode*>(node);
}

ModelNode* as_model_node(DataObject* node) noexcept {
    return reinterpret_cast<ModelNode*>(node);
}

DataAttribute* child_attribute(DataObject* object, const char* path) {
    auto* child = ModelNode_getChild(as_model_node(object), path);
    if (child == nullptr || ModelNode_getType(child) != DataAttributeModelType) {
        throw std::runtime_error("IEC 61850 dynamic model attribute is missing");
    }
    return reinterpret_cast<DataAttribute*>(child);
}

DataAttribute* child_description(DataObject* object) {
    return child_attribute(object, "d");
}

void set_description(DataAttribute* attribute, const char* description) {
    MmsValue* value = MmsValue_newVisibleString(description);
    if (value == nullptr) {
        throw std::runtime_error("unable to create IEC 61850 description value");
    }
    DataAttribute_setValue(attribute, value);
    MmsValue_delete(value);
}

}  // namespace

namespace uhf::iec61850 {

Model::Model(std::string ied_name) {
    model_ = IedModel_create("UHFPD1");
    if (model_ == nullptr) {
        throw std::runtime_error("unable to create IEC 61850 model");
    }

    try {
        IedModel_setIedNameForDynamicModel(model_, ied_name.c_str());
        LogicalDevice* device = LogicalDevice_create("PDMON", model_);
        if (device == nullptr) {
            throw std::runtime_error("unable to create IEC 61850 logical device");
        }

        LogicalNode* lln0 = LogicalNode_create("LLN0", device);
        LogicalNode* lphd1 = LogicalNode_create("LPHD1", device);
        LogicalNode* spdc1 = LogicalNode_create("SPDC1", device);
        LogicalNode* ggio1 = LogicalNode_create("GGIO1", device);
        if (lln0 == nullptr || lphd1 == nullptr || spdc1 == nullptr || ggio1 == nullptr) {
            throw std::runtime_error("unable to create IEC 61850 logical nodes");
        }

        DataObject* health = CDC_ENS_create("Health", as_model_node(lln0), CDC_OPTION_DESC);
        DataObject* phy_health = CDC_SPS_create(
            "PhyHealth", as_model_node(lphd1), CDC_OPTION_DESC);
        DataObject* peak = CDC_MV_create(
            "UhfPaDsch", as_model_node(spdc1), CDC_OPTION_DESC, false);
        DataObject* alarm = CDC_SPS_create(
            "PaDschAlm", as_model_node(spdc1), CDC_OPTION_DESC);
        if (health == nullptr || phy_health == nullptr || peak == nullptr || alarm == nullptr) {
            throw std::runtime_error("unable to create IEC 61850 common data objects");
        }

        set_description(child_description(health), "设备健康状态");
        set_description(child_description(phy_health), "物理设备健康状态");
        set_description(child_description(peak), "标准 UHF 局放峰值");
        set_description(child_description(alarm), "局部放电告警");

        peak_value_ = child_attribute(peak, "mag.f");
        peak_quality_ = child_attribute(peak, "q");
        peak_time_ = child_attribute(peak, "t");
        alarm_value_ = child_attribute(alarm, "stVal");
        alarm_quality_ = child_attribute(alarm, "q");
        alarm_time_ = child_attribute(alarm, "t");

        static constexpr const char* kMeasurementNames[kMeasurementCount] = {
            "AnIn1", "IntIn1", "AnIn2", "AnIn3", "AnIn4"};
        static constexpr const char* kMeasurementDescriptions[kMeasurementCount] = {
            "放电均值", "脉冲次数", "放电峰值", "峰值相位", "背景噪声"};
        for (std::size_t index = 0; index < kMeasurementCount; ++index) {
            DataObject* measurement = index == 1U
                ? CDC_INS_create(
                      kMeasurementNames[index], as_model_node(ggio1), CDC_OPTION_DESC)
                : CDC_MV_create(
                      kMeasurementNames[index], as_model_node(ggio1), CDC_OPTION_DESC, false);
            if (measurement == nullptr) {
                throw std::runtime_error("unable to create IEC 61850 measurement object");
            }
            set_description(child_description(measurement), kMeasurementDescriptions[index]);
            measurement_values_[index] = child_attribute(
                measurement, index == 1U ? "stVal" : "mag.f");
            measurement_integer_[index] = index == 1U;
            measurement_qualities_[index] = child_attribute(measurement, "q");
            measurement_times_[index] = child_attribute(measurement, "t");
        }

        DataSet* data_set = DataSet_create("DSMeasurements", lln0);
        if (data_set == nullptr) {
            throw std::runtime_error("unable to create IEC 61850 data set");
        }
        for (std::size_t index = 0; index < kMeasurementCount; ++index) {
            const std::string reference = index == 1U
                ? "GGIO1$ST$IntIn1$stVal"
                : std::string("GGIO1$MX$AnIn") +
                    std::to_string(index == 0U ? 1U : index) + "$mag$f";
            if (DataSetEntry_create(data_set, reference.c_str(), -1, nullptr) == nullptr) {
                throw std::runtime_error("unable to create IEC 61850 data set member");
            }
        }
        if (DataSetEntry_create(data_set, "SPDC1$ST$PaDschAlm$stVal", -1, nullptr) == nullptr) {
            throw std::runtime_error("unable to create IEC 61850 alarm data set member");
        }

        constexpr std::uint8_t report_options = static_cast<std::uint8_t>(
            RPT_OPT_SEQ_NUM | RPT_OPT_TIME_STAMP | RPT_OPT_REASON_FOR_INCLUSION |
            RPT_OPT_DATA_SET | RPT_OPT_DATA_REFERENCE);
        if (ReportControlBlock_create(
                "RPMeasurements",
                lln0,
                "RPMeasurements",
                false,
                "DSMeasurements",
                1U,
                static_cast<std::uint8_t>(
                    TRG_OPT_DATA_CHANGED | TRG_OPT_QUALITY_CHANGED | TRG_OPT_INTEGRITY),
                report_options,
                0U,
                60000U) == nullptr) {
            throw std::runtime_error("unable to create IEC 61850 report control block");
        }

        (void)health;
        (void)phy_health;
    } catch (...) {
        IedModel_destroy(model_);
        model_ = nullptr;
        throw;
    }
}

Model::~Model() {
    if (model_ != nullptr) {
        IedModel_destroy(model_);
    }
}

IedModel* Model::raw() const noexcept {
    return model_;
}

DataAttribute* Model::measurement_value(std::size_t index) const noexcept {
    return index < measurement_values_.size() ? measurement_values_[index] : nullptr;
}

DataAttribute* Model::measurement_quality(std::size_t index) const noexcept {
    return index < measurement_qualities_.size() ? measurement_qualities_[index] : nullptr;
}

DataAttribute* Model::measurement_time(std::size_t index) const noexcept {
    return index < measurement_times_.size() ? measurement_times_[index] : nullptr;
}

bool Model::measurement_integer(std::size_t index) const noexcept {
    return index < measurement_integer_.size() && measurement_integer_[index];
}

DataAttribute* Model::peak_value() const noexcept {
    return peak_value_;
}

DataAttribute* Model::peak_quality() const noexcept {
    return peak_quality_;
}

DataAttribute* Model::peak_time() const noexcept {
    return peak_time_;
}

DataAttribute* Model::alarm_value() const noexcept {
    return alarm_value_;
}

DataAttribute* Model::alarm_quality() const noexcept {
    return alarm_quality_;
}

DataAttribute* Model::alarm_time() const noexcept {
    return alarm_time_;
}

}  // namespace uhf::iec61850
