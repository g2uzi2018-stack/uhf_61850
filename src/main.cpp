// SPDX-License-Identifier: GPL-3.0-only
#include "app/build_info.hpp"
#include "logging/logger.hpp"
#include "web/http_server.hpp"

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace {

struct WebOptions {
    std::filesystem::path document_root{"web"};
    std::filesystem::path state_directory{"/var/lib/uhf-gateway"};
    std::string bind_address{"127.0.0.1"};
    std::uint16_t port{8080};
};

bool parse_listen(std::string_view value, std::string& address, std::uint16_t& port) {
    const std::size_t separator = value.rfind(':');
    if (separator == std::string_view::npos || separator == 0 ||
        separator + 1 >= value.size()) {
        return false;
    }

    const std::string_view port_text = value.substr(separator + 1);
    std::uint32_t parsed_port = 0;
    const auto result = std::from_chars(
        port_text.data(), port_text.data() + port_text.size(), parsed_port);
    if (result.ec != std::errc{} || result.ptr != port_text.data() + port_text.size() ||
        parsed_port > std::numeric_limits<std::uint16_t>::max()) {
        return false;
    }

    address = std::string(value.substr(0, separator));
    port = static_cast<std::uint16_t>(parsed_port);
    return true;
}

void print_usage() {
    std::cerr << "usage: " << uhf::app::kProductName
              << " [--version|--self-test|--web "
                 "[--web-root PATH] [--state-dir PATH] [--listen IPV4:PORT]]\n";
}

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

    if (argument == "--web") {
        WebOptions options;
        for (int index = 2; index < argc; ++index) {
            const std::string_view option = argv[index];
            if (option == "--web-root" && index + 1 < argc) {
                options.document_root = argv[++index];
            } else if (option == "--state-dir" && index + 1 < argc) {
                options.state_directory = argv[++index];
            } else if (option == "--listen" && index + 1 < argc) {
                if (!parse_listen(argv[++index], options.bind_address, options.port)) {
                    std::cerr << "invalid --listen value\n";
                    return 2;
                }
            } else {
                print_usage();
                return 2;
            }
        }

        try {
            uhf::logging::Logger logger;
            logger.log(
                uhf::logging::Level::info,
                uhf::logging::Component::system,
                "process.start",
                "uhf-gatewayd web service starting",
                {uhf::logging::Field{"listen", options.bind_address + ":" +
                        std::to_string(options.port)}});
            uhf::web::HttpServer server(
                std::move(options.document_root),
                std::move(options.bind_address),
                options.port,
                std::move(options.state_directory));
            const int result = server.run();
            logger.log(
                uhf::logging::Level::info,
                uhf::logging::Component::system,
                "process.stop",
                "uhf-gatewayd web service stopped");
            return result;
        } catch (const std::exception& error) {
            std::cerr << "unable to start web server: " << error.what() << '\n';
            return 1;
        }
    }

    print_usage();
    return 2;
}
