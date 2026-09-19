// SPDX-License-Identifier: GPL-3.0-only
#include "v3/measurements.hpp"
#include "test_support.hpp"
using namespace uhf::v3;
namespace {
CurrentValues sample(float a, float b, float c, float A=10, float B=20, float C=30) {
    return {valid_value(a),valid_value(b),valid_value(c),valid_value(A),valid_value(B),
            valid_value(C),valid_value(7),valid_value(8)};
}
TemperatureValues temperatures(float a, float b, float c) {
    return {valid_value(a),valid_value(b),valid_value(c)};
}
}
int main() {
    try {
        MonitoringCalculator calc(WarmupPolicy::use_available, MeanPolicy::literal_signed);
        for (const auto& v : calc.snapshot()) { check(!v.valid(),"no invented startup measurements"); }
        check(calc.on_current(1,sample(1,2,3)),"accept first current sample");
        calc.on_temperature(temperatures(20,30,40));
        const std::array<float,35> expected{1,2,3,10,20,30,7,8,20,30,40,30,2,20,
            .1F,.1F,.1F,50,50,100.0F/3.0F,-10,0,10,1,1,1,1,2,3,1,2,3,-10,-10,20};
        auto result = calc.snapshot();
        for (std::size_t i=0;i<expected.size();++i) {
            check(result[i].valid(),"all normal values valid"); near(result[i].value,expected[i],kValueNames[i].data());
        }
        for (int i=0;i<100;++i) { (void)calc.snapshot(); calc.on_temperature(temperatures(20,30,40)); }
        check(calc.window_samples() == std::array<std::size_t,3>{1,1,1},"only acquisition advances windows");
        check(!calc.on_current(1,sample(99,99,99)) && !calc.on_current(0,sample(99,99,99)),"ignore duplicate/out-of-order samples");
        near(calc.snapshot()[0].value,1,"duplicate did not change raw value");
        MonitoringCalculator rolling(WarmupPolicy::require_50, MeanPolicy::literal_signed);
        for (std::uint64_t i=1;i<=49;++i) { rolling.on_current(i,sample(static_cast<float>(i),2.0F*static_cast<float>(i),3.0F*static_cast<float>(i))); }
        check(rolling.snapshot()[26].quality == Quality::insufficient_samples,"49 != 50 samples");
        rolling.on_current(50,sample(50,100,150));
        result = rolling.snapshot();
        near(result[26].value,50,"50-sample maximum"); near(result[29].value,1,"50-sample minimum");
        near(result[23].value,50,"50-sample ratio");
        rolling.on_current(51,sample(51,102,153));
        result = rolling.snapshot();
        near(result[26].value,51,"rolling maximum"); near(result[29].value,2,"oldest evicted");
        near(result[23].value,25.5F,"rolling ratio");
        auto partial = sample(52,104,156); partial[0] = {};
        rolling.on_current(52,partial);
        check(!rolling.snapshot()[26].valid(),"invalid current must not look fresh via history");
        rolling.on_current(53,sample(53,106,159));
        near(rolling.snapshot()[29].value,3,"invalid sample did not enter phase A window");
        calc.on_current(2,sample(0,0,0,0,0,0));
        result = calc.snapshot();
        check(!result[14].valid() && !result[17].valid() && !result[23].valid(),"division by zero is invalid, not zero");
        calc.on_temperature(temperatures(-10,-20,-30));
        near(calc.snapshot()[19].value,-50,"literal negative-mean formula");
        MonitoringCalculator absolute(WarmupPolicy::use_available,MeanPolicy::absolute_denominator);
        absolute.on_temperature(temperatures(-10,-20,-30));
        near(absolute.snapshot()[19].value,50,"explicit absolute denominator option");
        MonitoringCalculator reject(WarmupPolicy::use_available,MeanPolicy::reject_nonpositive);
        reject.on_temperature(temperatures(-10,-20,-30));
        check(!reject.snapshot()[19].valid(),"explicit nonpositive-mean rejection");
        calc.on_temperature(temperatures(-10,0,10));
        check(!calc.snapshot()[19].valid(),"zero temperature mean");
        calc.invalidate_current();
        result = calc.snapshot();
        check(!result[0].valid() && !result[12].valid() && !result[26].valid() && result[11].valid(),"port failure isolation");
        calc.invalidate_temperature(); check(!calc.snapshot()[8].valid(),"temperature failure invalidates");
        check(!valid_value(std::numeric_limits<float>::infinity()).valid(),"infinity rejected");
        check(!valid_value(std::numeric_limits<float>::quiet_NaN()).valid(),"NaN rejected as valid input");
        check(float32_registers(1) == std::array<std::uint16_t,2>{0x3f80,0},"float32 ABCD");
        check(float32_registers(-2.5F) == std::array<std::uint16_t,2>{0xc020,0},"negative float32");
        check(float32_registers(-0.0F) == std::array<std::uint16_t,2>{0x8000,0},"negative zero");
        check(float32_registers(std::numeric_limits<float>::quiet_NaN()) == std::array<std::uint16_t,2>{0x7fc0,0},"canonical quiet NaN");
        std::cout << "v3 measurements: all 35 values and boundary checks passed\n"; return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
