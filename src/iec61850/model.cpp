// SPDX-License-Identifier: GPL-3.0-only
#include "iec61850/model.hpp"

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <string>
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

void set_description(DataAttribute* attribute, const std::string& description) {
    if (description.empty()) {
        return;
    }
    MmsValue* value = MmsValue_newVisibleString(description.c_str());
    if (value == nullptr) {
        throw std::runtime_error("unable to create IEC 61850 description value");
    }
    DataAttribute_setValue(attribute, value);
    MmsValue_delete(value);
}

}  // namespace

namespace uhf::iec61850 {

std::string data_set_entry_reference(const uhf::iec61850::SclDataSetEntry& entry) {
    std::string reference = entry.prefix + entry.ln_class + entry.ln_inst;
    reference.append("$");
    reference.append(entry.fc);
    reference.append("$");
    reference.append(entry.do_name);
    reference.append("$");
    for (const char character : entry.da_name) {
        reference.push_back(character == '.' ? '$' : character);
    }
    return reference;
}

std::uint8_t report_triggers(const uhf::iec61850::SclReportControl& report) noexcept {
    std::uint8_t result = 0U;
    if (report.data_changed) {
        result = static_cast<std::uint8_t>(result | TRG_OPT_DATA_CHANGED);
    }
    if (report.quality_changed) {
        result = static_cast<std::uint8_t>(result | TRG_OPT_QUALITY_CHANGED);
    }
    if (report.integrity) {
        result = static_cast<std::uint8_t>(result | TRG_OPT_INTEGRITY);
    }
    return result;
}

Model::Model(std::string ied_name)
    : Model(default_model_definition(std::move(ied_name))) {}

Model::Model(SclModelDefinition definition)
    : definition_(std::move(definition)) {
    model_ = IedModel_create("UHFPD1");
    if (model_ == nullptr) {
        throw std::runtime_error("unable to create IEC 61850 model");
    }

    try {
        IedModel_setIedNameForDynamicModel(model_, definition_.ied_name.c_str());
        LogicalDevice* device = LogicalDevice_create(definition_.logical_device.c_str(), model_);
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

        set_description(child_description(health), definition_.lln0_description.empty()
            ? std::string{"设备健康状态"} : definition_.lln0_description);
        set_description(child_description(phy_health), definition_.phy_health_description);
        set_description(child_description(peak), definition_.peak_description);
        set_description(child_description(alarm), definition_.alarm_description);

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
            set_description(
                child_description(measurement),
                definition_.measurement_descriptions[index].empty()
                    ? std::string{kMeasurementDescriptions[index]}
                    : definition_.measurement_descriptions[index]);
            measurement_values_[index] = child_attribute(
                measurement, index == 1U ? "stVal" : "mag.f");
            measurement_integer_[index] = index == 1U;
            measurement_qualities_[index] = child_attribute(measurement, "q");
            measurement_times_[index] = child_attribute(measurement, "t");
        }

        constexpr std::uint8_t report_options = static_cast<std::uint8_t>(
            RPT_OPT_SEQ_NUM | RPT_OPT_TIME_STAMP | RPT_OPT_REASON_FOR_INCLUSION |
            RPT_OPT_DATA_SET | RPT_OPT_DATA_REFERENCE);
        for (const SclDataSet& definition_data_set : definition_.data_sets) {
            DataSet* data_set = DataSet_create(definition_data_set.name.c_str(), lln0);
            if (data_set == nullptr) {
                throw std::runtime_error("unable to create IEC 61850 data set");
            }
            for (const SclDataSetEntry& entry : definition_data_set.entries) {
                const std::string reference = data_set_entry_reference(entry);
                if (DataSetEntry_create(data_set, reference.c_str(), -1, nullptr) == nullptr) {
                    throw std::runtime_error("unable to create IEC 61850 data set member");
                }
            }
        }

        for (const SclReportControl& report : definition_.reports) {
            if (ReportControlBlock_create(
                    report.name.c_str(),
                    lln0,
                    report.rpt_id.empty() ? report.name.c_str() : report.rpt_id.c_str(),
                    report.buffered,
                    report.data_set.c_str(),
                    1U,
                    report_triggers(report),
                    report_options,
                    report.buffer_time_ms,
                    report.integrity_period_ms) == nullptr) {
                throw std::runtime_error("unable to create IEC 61850 report control block");
            }
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

const SclModelDefinition& Model::definition() const noexcept {
    return definition_;
}

}  // namespace uhf::iec61850
