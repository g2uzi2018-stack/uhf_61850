// SPDX-License-Identifier: GPL-3.0-only
#include "app/build_info.hpp"

#include <iostream>
#include <string_view>

namespace {

int run_self_test() {
    if (uhf::app::kProductName != "uhf-gatewayd" ||
        uhf::app::kVersion.empty()) {
        return 1;
    }

    std::cout << uhf::app::kProductName << " self-test: OK\n";
    return 0;
}

}  // namespace

int main(int argc, char* argv[]) {
    const std::string_view argument = argc > 1 ? argv[1] : "";

    if (argument == "--version") {
        std::cout << uhf::app::kProductName << " " << uhf::app::kVersion << '\n';
        return 0;
    }

    if (argument == "--self-test") {
        return run_self_test();
    }

    std::cerr << "usage: " << uhf::app::kProductName
              << " [--version|--self-test]\n";
    return 2;
}
