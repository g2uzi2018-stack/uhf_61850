// SPDX-License-Identifier: GPL-3.0-only
#include "iec61850/scl_model.hpp"

#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

namespace {

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
    }
    return condition;
}

std::string read_file(const char* path) {
    std::ifstream input(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
}

bool check(const char* path, const char* ied_name, const char* logical_device) {
    const std::string contents = read_file(path);
    uhf::iec61850::SclModelDefinition definition;
    std::string error;
    bool ok = expect(
        uhf::iec61850::parse_scl_model(contents, definition, error),
        "SCL model parses");
    if (!ok) {
        std::cerr << "parser error: " << error << '\n';
        return false;
    }
    ok = expect(definition.ied_name == ied_name, "IED name is captured") && ok;
    ok = expect(definition.logical_device == logical_device, "logical device is captured") && ok;
    ok = expect(definition.data_sets.size() == 2U, "two approved data sets are captured") && ok;
    ok = expect(definition.reports.size() == 2U, "two report controls are captured") && ok;
    ok = expect(definition.data_sets[0].entries.size() == 6U, "telemetry FCDA entries are captured") && ok;
    ok = expect(definition.data_sets[1].entries.size() == 2U, "state FCDA entries are captured") && ok;
    ok = expect(
        definition.communication_alarm_description == "下位机通讯异常",
        "communication alarm description is captured") && ok;
    ok = expect(definition.reports[1].buffered, "state report buffering is captured") && ok;
    ok = expect(definition.reports[1].buffer_time_ms == 4000U, "state report buffer time is captured") && ok;
    ok = expect(definition.measurement_descriptions[0] == "放电均值", "measurement description is captured") && ok;
    return ok;
}

}  // namespace

int main(int argc, char* argv[]) {
    if (argc != 3) {
        std::cerr << "usage: " << argv[0] << " CONFIG_ICD UPLOADED_ICD\n";
        return 2;
    }
    bool ok = check(argv[1], "UHFPD1", "PDMON") &&
        check(argv[2], "UHFPD12PD", "MON");
    std::string legacy = read_file(argv[1]);
    const std::size_t ind1 = legacy.find("<FCDA", legacy.find("DSState"));
    const std::size_t ind1_end = ind1 == std::string::npos ? std::string::npos : legacy.find('>', ind1);
    if (ind1 != std::string::npos && ind1_end != std::string::npos &&
        legacy.substr(ind1, ind1_end - ind1).find("Ind1") == std::string::npos) {
        const std::size_t next = legacy.find("<FCDA", ind1_end);
        const std::size_t next_end = next == std::string::npos ? std::string::npos : legacy.find('>', next);
        if (next != std::string::npos && next_end != std::string::npos) {
            legacy.erase(next, next_end - next + 1U);
        }
    }
    uhf::iec61850::SclModelDefinition legacy_definition;
    std::string legacy_error;
    ok = expect(
        uhf::iec61850::parse_scl_model(legacy, legacy_definition, legacy_error),
        "legacy SCL without communication FCDA remains compatible") && ok;
    ok = expect(
        legacy_definition.data_sets.size() == 2U &&
            legacy_definition.data_sets[1].entries.size() == 2U,
        "legacy SCL is augmented with communication alarm") && ok;
    if (ok) {
        std::cout << "IEC 61850 SCL model smoke: OK\n";
    }
    return ok ? 0 : 1;
}
