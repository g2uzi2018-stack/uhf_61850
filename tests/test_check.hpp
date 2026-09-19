// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <stdexcept>
#include <string>

namespace uhf::test {

[[noreturn]] inline void fail_check(
    const char* expression, const char* file, int line) {
    throw std::runtime_error(
        std::string(file) + ":" + std::to_string(line) +
        ": test check failed: " + expression);
}

}  // namespace uhf::test

#define UHF_TEST_CHECK(expression)                                                   \
    do {                                                                             \
        if (!(expression)) {                                                         \
            ::uhf::test::fail_check(#expression, __FILE__, __LINE__);                \
        }                                                                            \
    } while (false)
