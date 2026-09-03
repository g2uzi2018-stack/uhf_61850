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
    ok = expect(definition.data_sets[1].entries.size() == 1U, "state FCDA entry is captured") && ok;
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
    const bool ok = check(argv[1], "UHFPD1", "PDMON") &&
        check(argv[2], "UHFPD12PD", "MON");
    if (ok) {
        std::cout << "IEC 61850 SCL model smoke: OK\n";
    }
    return ok ? 0 : 1;
}
