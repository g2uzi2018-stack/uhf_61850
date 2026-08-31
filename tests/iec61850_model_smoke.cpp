// SPDX-License-Identifier: GPL-3.0-only
#include "iec61850/model.hpp"

#include <cstddef>
#include <iostream>
#include <string>

namespace {

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
    }
    return condition;
}

}  // namespace

int main() {
    uhf::iec61850::Model model("TESTIED");
    bool ok = true;
    ok = expect(model.raw() != nullptr, "model exists") && ok;
    ok = expect(
        IedModel_getModelNodeByObjectReference(
            model.raw(), "TESTIEDPDMON/SPDC1.UhfPaDsch.mag.f") != nullptr,
        "standard peak reference exists") && ok;
    ok = expect(
        IedModel_getModelNodeByObjectReference(
            model.raw(), "TESTIEDPDMON/GGIO1.AnIn5.mag.f") != nullptr,
        "fifth measurement reference exists") && ok;
    ok = expect(
        IedModel_getModelNodeByObjectReference(
            model.raw(), "TESTIEDPDMON/SPDC1.PaDschAlm.stVal") != nullptr,
        "alarm reference exists") && ok;
    ok = expect(model.measurement_value(uhf::iec61850::kMeasurementCount) == nullptr,
                "measurement bounds") && ok;
    for (std::size_t index = 0; index < uhf::iec61850::kMeasurementCount; ++index) {
        ok = expect(model.measurement_value(index) != nullptr, "measurement value exists") && ok;
        ok = expect(model.measurement_quality(index) != nullptr, "measurement quality exists") && ok;
        ok = expect(model.measurement_time(index) != nullptr, "measurement time exists") && ok;
    }
    ok = expect(model.peak_value() != nullptr, "peak handle exists") && ok;
    ok = expect(model.alarm_value() != nullptr, "alarm handle exists") && ok;
    ok = expect(model.alarm_quality() != nullptr, "alarm quality handle exists") && ok;
    ok = expect(model.alarm_time() != nullptr, "alarm time handle exists") && ok;
    if (ok) {
        std::cout << "IEC 61850 dynamic model smoke: OK\n";
    }
    return ok ? 0 : 1;
}
