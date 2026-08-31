// SPDX-License-Identifier: GPL-3.0-only
#include "web/tls_context.hpp"

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
        std::cerr << "usage: uhf-tls-init [--state-dir PATH]\n";
        return 2;
    }
    try {
        const uhf::web::TlsFiles files{
            options.state_directory / "tls" / "server.crt",
            options.state_directory / "tls" / "server.key"};
        uhf::web::ensure_tls_files(files);
        uhf::web::TlsContext context(files);
        if (context.native() == nullptr) {
            std::cerr << "TLS context was not created\n";
            return 1;
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "unable to initialize TLS state: " << error.what() << '\n';
        return 1;
    }
}
