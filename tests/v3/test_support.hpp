// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>
inline void check(bool condition, const char* message) {
    if (!condition) { throw std::runtime_error(message); }
}
inline void near(float actual, float expected, const char* message) {
    check(std::isfinite(actual) && std::fabs(actual - expected) <=
          0.0001F * (1.0F + std::fabs(expected)), message);
}
template<class Function> inline void rejects(Function function, const char* message) {
    bool rejected = false;
    try { function(); } catch (const std::exception&) { rejected = true; }
    check(rejected, message);
}
