// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace uhf::iec61850 {

struct SclDataSetEntry {
    std::string ld_inst;
    std::string prefix;
    std::string ln_class;
    std::string ln_inst;
    std::string do_name;
    std::string da_name;
    std::string fc;
};

struct SclDataSet {
    std::string name;
    std::string description;
    std::vector<SclDataSetEntry> entries;
};

struct SclReportControl {
    std::string name;
    std::string description;
    std::string rpt_id;
    std::string data_set;
    bool buffered{false};
    std::uint32_t buffer_time_ms{0U};
    std::uint32_t integrity_period_ms{60000U};
    std::uint8_t max_clients{1U};
    bool data_changed{false};
    bool quality_changed{false};
    bool integrity{false};
};

struct SclModelDefinition {
    std::string ied_name;
    std::string ied_description;
    std::string logical_device;
    std::string logical_device_description;
    std::string lln0_description;
    std::string phy_health_description;
    std::array<std::string, 5U> measurement_descriptions{};
    std::string peak_description;
    std::string alarm_description;
    std::string communication_alarm_description;
    std::uint8_t max_report_controls{2U};
    bool v3_monitoring{false};
    std::vector<SclDataSet> data_sets;
    std::vector<SclReportControl> reports;
};

SclModelDefinition default_model_definition(std::string ied_name = "UHFPD1");
SclModelDefinition default_v3_model_definition(std::string ied_name = "UHFMON1");

bool parse_scl_model(
    std::string_view contents,
    SclModelDefinition& definition,
    std::string& error) noexcept;

}  // namespace uhf::iec61850
