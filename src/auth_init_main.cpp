// SPDX-License-Identifier: GPL-3.0-only
#include "web/auth_store.hpp"

#include <filesystem>
#include <iostream>
#include <string_view>

namespace {

struct Options {
    std::filesystem::path state_directory{"/var/lib/uhf-gateway"};
};

bool parse_options(int argc, char* argv[], Options& options) {
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument = argv[index];
        if (argument == "--state-dir" && index + 1 < argc) {
            options.state_directory = argv[++index];
        } else {
            return false;
        }
    }
    return true;
}

}  // namespace

int main(int argc, char* argv[]) {
    Options options;
    if (!parse_options(argc, argv, options)) {
        std::cerr << "usage: uhf-auth-init [--state-dir PATH]\n";
        return 2;
    }
    try {
        uhf::web::AuthStore store(options.state_directory);
        std::error_code error;
        if (!std::filesystem::is_regular_file(store.bootstrap_password_path(), error) || error) {
            std::cerr << "authentication bootstrap file was not created\n";
            return 1;
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "unable to initialize authentication state: " << error.what() << '\n';
        return 1;
    }
}
