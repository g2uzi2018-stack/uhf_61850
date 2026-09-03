// SPDX-License-Identifier: GPL-3.0-only
#include "iec61850/model.hpp"

#include <cstddef>
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

}  // namespace

int main(int argc, char* argv[]) {
    uhf::iec61850::Model model("TESTIED");
    bool ok = true;
    ok = expect(model.raw() != nullptr, "model exists") && ok;
    ok = expect(
        IedModel_getModelNodeByObjectReference(
            model.raw(), "TESTIEDPDMON/SPDC1.UhfPaDsch.mag.f") != nullptr,
        "standard peak reference exists") && ok;
    ok = expect(
        IedModel_getModelNodeByObjectReference(
            model.raw(), "TESTIEDPDMON/GGIO1.IntIn1.stVal") != nullptr,
        "frequency measurement reference exists") && ok;
    ok = expect(
        IedModel_getModelNodeByObjectReference(
            model.raw(), "TESTIEDPDMON/GGIO1.AnIn4.mag.f") != nullptr,
        "fourth analog measurement reference exists") && ok;
    ok = expect(
        IedModel_getModelNodeByObjectReference(
            model.raw(), "TESTIEDPDMON/SPDC1.PaDschAlm.stVal") != nullptr,
        "alarm reference exists") && ok;
    ok = expect(
        IedModel_getModelNodeByObjectReference(
            model.raw(), "TESTIEDPDMON/GGIO1.AnIn1.d") != nullptr,
        "average description exists") && ok;
    ok = expect(
        IedModel_getModelNodeByObjectReference(
            model.raw(), "TESTIEDPDMON/SPDC1.UhfPaDsch.d") != nullptr,
        "standard peak description exists") && ok;
    ok = expect(model.measurement_value(uhf::iec61850::kMeasurementCount) == nullptr,
                "measurement bounds") && ok;
    for (std::size_t index = 0; index < uhf::iec61850::kMeasurementCount; ++index) {
        ok = expect(model.measurement_value(index) != nullptr, "measurement value exists") && ok;
        ok = expect(model.measurement_quality(index) != nullptr, "measurement quality exists") && ok;
        ok = expect(model.measurement_time(index) != nullptr, "measurement time exists") && ok;
    }
    ok = expect(model.measurement_integer(1U), "frequency is integer") && ok;
    ok = expect(!model.measurement_integer(0U), "average is analog") && ok;
    ok = expect(model.peak_value() != nullptr, "peak handle exists") && ok;
    ok = expect(model.alarm_value() != nullptr, "alarm handle exists") && ok;
    ok = expect(model.alarm_quality() != nullptr, "alarm quality handle exists") && ok;
    ok = expect(model.alarm_time() != nullptr, "alarm time handle exists") && ok;
    if (argc == 2) {
        std::ifstream input(argv[1], std::ios::binary);
        const std::string contents(
            (std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
        uhf::iec61850::SclModelDefinition definition;
        std::string error;
        ok = expect(
            uhf::iec61850::parse_scl_model(contents, definition, error),
            "uploaded SCL parses for model generation") && ok;
        if (ok) {
            uhf::iec61850::Model uploaded_model(definition);
            const std::string prefix = definition.ied_name + definition.logical_device;
            ok = expect(
                IedModel_getModelNodeByObjectReference(
                    uploaded_model.raw(), (prefix + "/SPDC1.PaDschAlm.stVal").c_str()) != nullptr,
                "uploaded state point is generated") && ok;
            ok = expect(
                IedModel_lookupDataSet(
                    uploaded_model.raw(), (prefix + "/LLN0$DSState").c_str()) != nullptr,
                "uploaded state data set is generated") && ok;
            ok = expect(
                uploaded_model.definition().reports.size() == 2U &&
                    uploaded_model.definition().reports[1].name == "RPState",
                "uploaded state report control is generated") && ok;
        }
    }
    if (ok) {
        std::cout << "IEC 61850 dynamic model smoke: OK\n";
    }
    return ok ? 0 : 1;
}
