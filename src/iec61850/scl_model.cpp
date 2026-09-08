// SPDX-License-Identifier: GPL-3.0-only
#include "iec61850/scl_model.hpp"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

constexpr std::size_t kMaxSclBytes = 48U * 1024U;
constexpr std::string_view kSclNamespace = "http://www.iec.ch/61850/2003/SCL";

struct Attribute {
    std::string name;
    std::string value;
};

struct Element {
    std::string name;
    std::vector<Attribute> attributes;
    std::string text;
};

std::string local_name(std::string_view name) {
    const std::size_t separator = name.rfind(':');
    return std::string(separator == std::string_view::npos ? name : name.substr(separator + 1U));
}

bool name_start(char value) noexcept {
    return (value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z') ||
        value == '_' || value == ':';
}

bool name_character(char value) noexcept {
    return name_start(value) || (value >= '0' && value <= '9') || value == '-' || value == '.';
}

void skip_space(std::string_view input, std::size_t& position) noexcept {
    while (position < input.size() &&
           std::isspace(static_cast<unsigned char>(input[position])) != 0) {
        ++position;
    }
}

bool parse_name(std::string_view input, std::size_t& position, std::string& name) {
    if (position >= input.size() || !name_start(input[position])) {
        return false;
    }
    const std::size_t begin = position++;
    while (position < input.size() && name_character(input[position])) {
        ++position;
    }
    name.assign(input.substr(begin, position - begin));
    return true;
}

bool parse_tag(
    std::string_view input,
    std::size_t& position,
    Element& element,
    bool& closing,
    bool& self_closing,
    std::string& error) {
    if (position >= input.size() || input[position] != '<') {
        error = "SCL tag expected";
        return false;
    }
    ++position;
    closing = false;
    self_closing = false;
    if (position < input.size() && input[position] == '/') {
        closing = true;
        ++position;
    }
    skip_space(input, position);
    if (!parse_name(input, position, element.name)) {
        error = "invalid SCL element name";
        return false;
    }
    element.attributes.clear();
    element.text.clear();
    if (closing) {
        skip_space(input, position);
        if (position >= input.size() || input[position] != '>') {
            error = "invalid SCL closing element";
            return false;
        }
        ++position;
        return true;
    }
    while (true) {
        skip_space(input, position);
        if (position >= input.size()) {
            error = "unterminated SCL element";
            return false;
        }
        if (input[position] == '>') {
            ++position;
            return true;
        }
        if (input[position] == '/') {
            ++position;
            skip_space(input, position);
            if (position >= input.size() || input[position] != '>') {
                error = "invalid SCL self-closing element";
                return false;
            }
            ++position;
            self_closing = true;
            return true;
        }
        Attribute attribute;
        if (!parse_name(input, position, attribute.name)) {
            error = "invalid SCL attribute name";
            return false;
        }
        skip_space(input, position);
        if (position >= input.size() || input[position++] != '=') {
            error = "SCL attribute assignment expected";
            return false;
        }
        skip_space(input, position);
        if (position >= input.size() || (input[position] != '\'' && input[position] != '"')) {
            error = "SCL quoted attribute expected";
            return false;
        }
        const char quote = input[position++];
        const std::size_t begin = position;
        while (position < input.size() && input[position] != quote) {
            if (input[position] == '<' || input[position] == '\0') {
                error = "invalid SCL attribute value";
                return false;
            }
            ++position;
        }
        if (position >= input.size()) {
            error = "unterminated SCL attribute value";
            return false;
        }
        attribute.value.assign(input.substr(begin, position - begin));
        ++position;
        element.attributes.push_back(std::move(attribute));
    }
}

const std::string* attribute(const Element& element, std::string_view name) {
    for (const Attribute& item : element.attributes) {
        if (local_name(item.name) == name) {
            return &item.value;
        }
    }
    return nullptr;
}

std::string trim(std::string_view input) {
    std::size_t begin = 0U;
    while (begin < input.size() && std::isspace(static_cast<unsigned char>(input[begin])) != 0) {
        ++begin;
    }
    std::size_t end = input.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(input[end - 1U])) != 0) {
        --end;
    }
    return std::string(input.substr(begin, end - begin));
}

std::string unescape(std::string value) {
    struct Replacement { std::string_view from; std::string_view to; };
    static constexpr Replacement replacements[] = {
        {"&quot;", "\""}, {"&apos;", "'"}, {"&lt;", "<"},
        {"&gt;", ">"}, {"&amp;", "&"}};
    for (const Replacement replacement : replacements) {
        std::size_t position = 0U;
        while ((position = value.find(replacement.from, position)) != std::string::npos) {
            value.replace(position, replacement.from.size(), replacement.to);
            position += replacement.to.size();
        }
    }
    return value;
}

bool parse_uint(std::string_view value, std::uint32_t& result) {
    if (value.empty()) {
        return false;
    }
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    return parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size();
}

const uhf::iec61850::SclDataSet* find_dataset(
    const uhf::iec61850::SclModelDefinition& definition, std::string_view name) {
    for (const uhf::iec61850::SclDataSet& data_set : definition.data_sets) {
        if (data_set.name == name) {
            return &data_set;
        }
    }
    return nullptr;
}

bool approved_entry(const uhf::iec61850::SclDataSetEntry& entry) {
    if (entry.prefix != "" || entry.ln_inst != "1" ||
        (entry.ln_class != "GGIO" && entry.ln_class != "SPDC")) {
        return false;
    }
    if (entry.ln_class == "GGIO") {
        if (entry.do_name == "Ind1") {
            return entry.da_name == "stVal" && entry.fc == "ST";
        }
        if (entry.do_name == "IntIn1") {
            return entry.da_name == "stVal" && entry.fc == "ST";
        }
        return (entry.do_name == "AnIn1" || entry.do_name == "AnIn2" ||
                entry.do_name == "AnIn3" || entry.do_name == "AnIn4") &&
            entry.da_name == "mag.f" && entry.fc == "MX";
    }
    return entry.do_name == "PaDschAlm" && entry.da_name == "stVal" && entry.fc == "ST";
}

}  // namespace

namespace uhf::iec61850 {

SclModelDefinition default_model_definition(std::string ied_name) {
    SclModelDefinition definition;
    definition.ied_name = std::move(ied_name);
    definition.ied_description = "局部放电在线监测主IED";
    definition.logical_device = "PDMON";
    definition.logical_device_description = "局部放电在线监测设备";
    definition.lln0_description = "设备公共信息";
    definition.phy_health_description = "物理设备健康状态";
    definition.measurement_descriptions = {"放电均值", "脉冲次数", "放电峰值", "峰值相位", "背景噪声"};
    definition.peak_description = "标准 UHF 局放峰值";
    definition.alarm_description = "局部放电告警";
    definition.communication_alarm_description = "下位机通讯异常";

    SclDataSet measurements;
    measurements.name = "DSMeasurements";
    measurements.description = "局部放电遥测数据集";
    const auto add = [&measurements](
                         std::string ln_class,
                         std::string do_name,
                         std::string da_name,
                         std::string fc) {
        measurements.entries.push_back(SclDataSetEntry{
            "PDMON", "", std::move(ln_class), "1", std::move(do_name),
            std::move(da_name), std::move(fc)});
    };
    add("GGIO", "AnIn1", "mag.f", "MX");
    add("GGIO", "IntIn1", "stVal", "ST");
    add("GGIO", "AnIn2", "mag.f", "MX");
    add("GGIO", "AnIn3", "mag.f", "MX");
    add("GGIO", "AnIn4", "mag.f", "MX");
    add("SPDC", "PaDschAlm", "stVal", "ST");
    definition.data_sets.push_back(std::move(measurements));
    definition.data_sets.push_back(SclDataSet{
        "DSState", "局部放电遥信数据集",
        {SclDataSetEntry{"PDMON", "", "SPDC", "1", "PaDschAlm", "stVal", "ST"},
         SclDataSetEntry{"PDMON", "", "GGIO", "1", "Ind1", "stVal", "ST"}}});
    definition.reports.push_back(SclReportControl{
        "RPMeasurements", "局放遥测数据报告控制块", "RPMeasurements", "DSMeasurements", false,
        0U, 60000U, 1U, true, true, true});
    definition.reports.push_back(SclReportControl{
        "RPState", "局放遥信数据报告控制块", "RPState", "DSState", true,
        4000U, 60000U, 2U, true, true, true});
    return definition;
}

bool parse_scl_model(
    std::string_view contents, SclModelDefinition& definition, std::string& error) noexcept {
    definition = {};
    error.clear();
    if (contents.empty() || contents.size() > kMaxSclBytes ||
        contents.find('\0') != std::string_view::npos ||
        contents.find("<!DOCTYPE") != std::string_view::npos ||
        contents.find("<!doctype") != std::string_view::npos ||
        contents.find("<!ENTITY") != std::string_view::npos ||
        contents.find("<!entity") != std::string_view::npos ||
        contents.find("<![CDATA[") != std::string_view::npos) {
        error = "unsupported or oversized SCL document";
        return false;
    }

    std::vector<Element> stack;
    stack.reserve(32U);
    std::optional<std::size_t> current_data_set;
    std::optional<std::size_t> current_report;
    std::optional<std::size_t> current_doi;
    std::string current_doi_name;
    std::string current_dai_name;
    bool root_seen = false;
    bool root_namespace_ok = false;
    bool ied_seen = false;
    bool logical_device_seen = false;
    std::size_t position = 0U;

    const auto in_ied = [&stack] {
        return std::any_of(stack.begin(), stack.end(), [](const Element& item) {
            return local_name(item.name) == "IED";
        });
    };
    while (position < contents.size()) {
        const std::size_t open = contents.find('<', position);
        if (open == std::string_view::npos) {
            if (!stack.empty()) {
                stack.back().text.append(contents.substr(position));
            }
            break;
        }
        if (!stack.empty()) {
            stack.back().text.append(contents.substr(position, open - position));
        }
        position = open;
        if (contents.compare(position, 4U, "<!--") == 0) {
            const std::size_t end = contents.find("-->", position + 4U);
            if (end == std::string_view::npos) {
                error = "unterminated SCL comment";
                return false;
            }
            position = end + 3U;
            continue;
        }
        if (contents.compare(position, 2U, "<?") == 0) {
            const std::size_t end = contents.find("?>", position + 2U);
            if (end == std::string_view::npos) {
                error = "unterminated SCL declaration";
                return false;
            }
            position = end + 2U;
            continue;
        }
        Element element;
        bool closing = false;
        bool self_closing = false;
        if (!parse_tag(contents, position, element, closing, self_closing, error)) {
            return false;
        }
        const std::string name = local_name(element.name);
        if (closing) {
            if (stack.empty() || local_name(stack.back().name) != name) {
                error = "unbalanced SCL element";
                return false;
            }
            const Element completed = std::move(stack.back());
            stack.pop_back();
            const std::string parent = stack.empty() ? std::string{} : local_name(stack.back().name);
            if (name == "Val" && current_doi && current_dai_name == "d" &&
                in_ied()) {
                const std::string value = unescape(trim(completed.text));
                if (current_doi_name == "Health") {
                    definition.lln0_description = value;
                } else if (current_doi_name == "PhyHealth") {
                    definition.phy_health_description = value;
                } else if (current_doi_name == "UhfPaDsch") {
                    definition.peak_description = value;
                } else if (current_doi_name == "PaDschAlm") {
                    definition.alarm_description = value;
                } else if (current_doi_name == "Ind1") {
                    definition.communication_alarm_description = value;
                } else if (current_doi_name == "AnIn1") {
                    definition.measurement_descriptions[0] = value;
                } else if (current_doi_name == "IntIn1") {
                    definition.measurement_descriptions[1] = value;
                } else if (current_doi_name == "AnIn2") {
                    definition.measurement_descriptions[2] = value;
                } else if (current_doi_name == "AnIn3") {
                    definition.measurement_descriptions[3] = value;
                } else if (current_doi_name == "AnIn4") {
                    definition.measurement_descriptions[4] = value;
                }
            }
            if (name == "DAI") {
                current_dai_name.clear();
            } else if (name == "DOI") {
                current_doi.reset();
                current_doi_name.clear();
            } else if (name == "DataSet") {
                current_data_set.reset();
            } else if (name == "ReportControl") {
                current_report.reset();
            }
            (void)parent;
            continue;
        }

        if (!root_seen) {
            root_seen = true;
            root_namespace_ok = name == "SCL" && attribute(element, "xmlns") != nullptr &&
                *attribute(element, "xmlns") == kSclNamespace;
        } else if (stack.empty()) {
            error = "SCL contains more than one root element";
            return false;
        }
        if (name == "IED" && in_ied()) {
            error = "nested IED element";
            return false;
        }
        if (name == "IED") {
            if (ied_seen) {
                error = "SCL contains multiple IED elements";
                return false;
            }
            ied_seen = true;
            if (const std::string* value = attribute(element, "name")) {
                definition.ied_name = *value;
            }
            if (const std::string* value = attribute(element, "desc")) {
                definition.ied_description = unescape(*value);
            }
        } else if (name == "LDevice" && in_ied()) {
            if (logical_device_seen) {
                error = "SCL contains multiple logical devices";
                return false;
            }
            logical_device_seen = true;
            if (definition.logical_device.empty()) {
                if (const std::string* value = attribute(element, "inst")) {
                    definition.logical_device = *value;
                }
                if (const std::string* value = attribute(element, "desc")) {
                    definition.logical_device_description = unescape(*value);
                }
            }
        } else if (name == "LN0" && in_ied()) {
            if (const std::string* value = attribute(element, "desc")) {
                definition.lln0_description = unescape(*value);
            }
        } else if (name == "DataSet" && in_ied()) {
            const std::string* value = attribute(element, "name");
            if (value == nullptr) {
                error = "SCL data set has no name";
                return false;
            }
            if (find_dataset(definition, *value) != nullptr) {
                error = "SCL contains duplicate data set names";
                return false;
            }
            definition.data_sets.push_back(SclDataSet{*value, {}, {}});
            current_data_set = definition.data_sets.size() - 1U;
            if (const std::string* desc = attribute(element, "desc")) {
                definition.data_sets.back().description = unescape(*desc);
            }
        } else if (name == "FCDA" && current_data_set && in_ied()) {
            SclDataSetEntry entry;
            const auto set_value = [&element, &entry](std::string_view key, std::string& output) {
                if (const std::string* value = attribute(element, key)) {
                    output = *value;
                }
            };
            set_value("ldInst", entry.ld_inst);
            set_value("prefix", entry.prefix);
            set_value("lnClass", entry.ln_class);
            set_value("lnInst", entry.ln_inst);
            set_value("doName", entry.do_name);
            set_value("daName", entry.da_name);
            set_value("fc", entry.fc);
            definition.data_sets[*current_data_set].entries.push_back(std::move(entry));
        } else if (name == "ReportControl" && in_ied()) {
            const std::string* value = attribute(element, "name");
            const std::string* data_set = attribute(element, "datSet");
            if (value == nullptr || data_set == nullptr) {
                error = "SCL report control is missing name or data set";
                return false;
            }
            if (std::any_of(
                    definition.reports.begin(), definition.reports.end(),
                    [value](const SclReportControl& report) { return report.name == *value; })) {
                error = "SCL contains duplicate report control names";
                return false;
            }
            SclReportControl report;
            report.name = *value;
            report.data_set = *data_set;
            if (const std::string* desc = attribute(element, "desc")) {
                report.description = unescape(*desc);
            }
            if (const std::string* rpt_id = attribute(element, "rptID")) {
                report.rpt_id = *rpt_id;
            } else {
                report.rpt_id = report.name;
            }
            if (const std::string* buffered = attribute(element, "buffered")) {
                report.buffered = *buffered == "true";
            }
            std::uint32_t parsed = 0U;
            if (const std::string* value_ms = attribute(element, "bufTime")) {
                if (!parse_uint(*value_ms, parsed)) {
                    error = "invalid report buffer time";
                    return false;
                }
                report.buffer_time_ms = parsed;
            }
            if (const std::string* value_ms = attribute(element, "intgPd")) {
                if (!parse_uint(*value_ms, parsed)) {
                    error = "invalid report integrity period";
                    return false;
                }
                report.integrity_period_ms = parsed;
            }
            definition.reports.push_back(std::move(report));
            current_report = definition.reports.size() - 1U;
        } else if (name == "RptEnabled" && current_report) {
            std::uint32_t parsed = 0U;
            if (const std::string* max = attribute(element, "max")) {
                if (!parse_uint(*max, parsed) || parsed == 0U || parsed > 255U) {
                    error = "invalid report client limit";
                    return false;
                }
                definition.reports[*current_report].max_clients = static_cast<std::uint8_t>(parsed);
            }
        } else if (name == "ConfReportControl" && in_ied()) {
            std::uint32_t parsed = 0U;
            if (const std::string* max = attribute(element, "max")) {
                if (!parse_uint(*max, parsed) || parsed == 0U || parsed > 255U) {
                    error = "invalid configured report control limit";
                    return false;
                }
                definition.max_report_controls = static_cast<std::uint8_t>(parsed);
            }
        } else if (name == "TrgOps" && current_report) {
            const auto enabled = [&element](std::string_view key) {
                const std::string* value = attribute(element, key);
                return value != nullptr && (*value == "true" || *value == "1");
            };
            definition.reports[*current_report].data_changed = enabled("dchg");
            definition.reports[*current_report].quality_changed = enabled("qchg");
            definition.reports[*current_report].integrity =
                enabled("intg") || enabled("period");
        } else if (name == "DO") {
            if (const std::string* do_name = attribute(element, "name")) {
                const std::string* desc = attribute(element, "desc");
                if (desc != nullptr) {
                    const std::string value = unescape(*desc);
                    if (*do_name == "Health") {
                        definition.lln0_description = value;
                    } else if (*do_name == "PhyHealth") {
                        definition.phy_health_description = value;
                    } else if (*do_name == "UhfPaDsch") {
                        definition.peak_description = value;
                    } else if (*do_name == "PaDschAlm") {
                        definition.alarm_description = value;
                    } else if (*do_name == "AnIn1") {
                        definition.measurement_descriptions[0] = value;
                    } else if (*do_name == "IntIn1") {
                        definition.measurement_descriptions[1] = value;
                    } else if (*do_name == "AnIn2") {
                        definition.measurement_descriptions[2] = value;
                    } else if (*do_name == "AnIn3") {
                        definition.measurement_descriptions[3] = value;
                    } else if (*do_name == "AnIn4") {
                        definition.measurement_descriptions[4] = value;
                    } else if (*do_name == "Ind1") {
                        definition.communication_alarm_description = value;
                    }
                }
            }
        } else if (name == "DOI" && in_ied()) {
            if (const std::string* value = attribute(element, "name")) {
                current_doi_name = *value;
                current_doi = 0U;
            }
        } else if (name == "DAI" && current_doi && in_ied()) {
            if (const std::string* value = attribute(element, "name")) {
                current_dai_name = *value;
            }
        }
        if (!self_closing) {
            if (stack.size() >= 64U) {
                error = "SCL nesting limit exceeded";
                return false;
            }
            stack.push_back(std::move(element));
        }
    }
    if (!root_seen || !root_namespace_ok || !stack.empty()) {
        error = "invalid SCL root or unbalanced document";
        return false;
    }
    if (definition.ied_name.empty() || definition.logical_device.empty()) {
        error = "SCL is missing IED or logical device";
        return false;
    }
    const SclDataSet* measurements = find_dataset(definition, "DSMeasurements");
    const SclDataSet* state = find_dataset(definition, "DSState");
    if (definition.data_sets.size() != 2U || measurements == nullptr || state == nullptr ||
        measurements->entries.size() != 6U || state->entries.empty() ||
        state->entries.size() > 2U) {
        error = "SCL must contain the approved telemetry and state data sets";
        return false;
    }
    SclDataSet* mutable_state = nullptr;
    for (SclDataSet& data_set : definition.data_sets) {
        if (data_set.name == "DSState") {
            mutable_state = &data_set;
            break;
        }
    }
    const bool communication_entry_present = mutable_state != nullptr &&
        std::any_of(
            mutable_state->entries.begin(),
            mutable_state->entries.end(),
            [](const SclDataSetEntry& entry) {
                return entry.ln_class == "GGIO" && entry.ln_inst == "1" &&
                    entry.do_name == "Ind1" && entry.da_name == "stVal" && entry.fc == "ST";
            });
    if (mutable_state != nullptr && !communication_entry_present) {
        mutable_state->entries.push_back(SclDataSetEntry{
            definition.logical_device, "", "GGIO", "1", "Ind1", "stVal", "ST"});
    }
    for (const SclDataSet& data_set : definition.data_sets) {
        if (data_set.name != "DSMeasurements" && data_set.name != "DSState") {
            continue;
        }
        for (const SclDataSetEntry& entry : data_set.entries) {
            if (entry.ld_inst != definition.logical_device || !approved_entry(entry)) {
                error = "SCL data set contains an unsupported FCDA reference";
                return false;
            }
        }
    }
    const SclReportControl* measurements_report = nullptr;
    const SclReportControl* state_report = nullptr;
    for (const SclReportControl& report : definition.reports) {
        if (report.name == "RPMeasurements") {
            measurements_report = &report;
        } else if (report.name == "RPState") {
            state_report = &report;
        }
    }
    if (definition.reports.size() != 2U || measurements_report == nullptr || state_report == nullptr ||
        measurements_report->data_set != "DSMeasurements" || state_report->data_set != "DSState") {
        error = "SCL must contain telemetry and state report controls";
        return false;
    }
    if (definition.max_report_controls == 0U) {
        definition.max_report_controls = 2U;
    }
    return true;
}

}  // namespace uhf::iec61850
