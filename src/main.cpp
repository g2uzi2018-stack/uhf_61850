// SPDX-License-Identifier: GPL-3.0-only
#include "app/build_info.hpp"
#include "app/gateway_runtime.hpp"
#include "config/config_store.hpp"
#include "logging/logger.hpp"
#include "web/http_server.hpp"

#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <iostream>
#include <limits>
#include <memory>
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
    bool start_acquisition{true};
    bool simulate{false};
    std::string acquisition_device{"/dev/ttyS1"};
    bool modbus_tcp_explicit{false};
    std::string modbus_tcp_bind{"127.0.0.1"};
    std::uint16_t modbus_tcp_port{502};
    bool modbus_rtu_explicit{false};
    bool start_modbus_rtu{true};
    std::string modbus_rtu_device{"/dev/ttyS4"};
    std::filesystem::path data_directory{"/var/lib/uhf-gateway/data"};
    bool data_directory_explicit{false};
    std::filesystem::path config_file{"/var/lib/uhf-gateway/config.json"};
    bool config_file_explicit{false};
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
                 "[--web-root PATH] [--state-dir PATH] [--listen IPV4:PORT] "
                 "[--simulate|--no-acquisition] [--acquisition-device PATH] "
                 "[--modbus-tcp-listen IPV4:PORT] [--modbus-rtu-device PATH] "
                 "[--no-modbus-rtu] [--data-dir PATH] [--config PATH]]\n";
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
            } else if (option == "--simulate") {
                options.simulate = true;
            } else if (option == "--no-acquisition") {
                options.start_acquisition = false;
            } else if (option == "--acquisition-device" && index + 1 < argc) {
                options.acquisition_device = argv[++index];
            } else if (option == "--modbus-tcp-listen" && index + 1 < argc) {
                if (!parse_listen(argv[++index], options.modbus_tcp_bind, options.modbus_tcp_port)) {
                    std::cerr << "invalid --modbus-tcp-listen value\n";
                    return 2;
                }
                options.modbus_tcp_explicit = true;
            } else if (option == "--modbus-rtu-device" && index + 1 < argc) {
                options.modbus_rtu_device = argv[++index];
                options.modbus_rtu_explicit = true;
                options.start_modbus_rtu = true;
            } else if (option == "--no-modbus-rtu") {
                options.start_modbus_rtu = false;
            } else if (option == "--data-dir" && index + 1 < argc) {
                options.data_directory = argv[++index];
                options.data_directory_explicit = true;
            } else if (option == "--config" && index + 1 < argc) {
                options.config_file = argv[++index];
                options.config_file_explicit = true;
            } else {
                print_usage();
                return 2;
            }
        }

        if (options.simulate && !options.modbus_tcp_explicit) {
            options.modbus_tcp_port = 15020U;
        }
        if (options.simulate && !options.modbus_rtu_explicit) {
            options.start_modbus_rtu = false;
        }
        if (options.simulate && !options.data_directory_explicit) {
            options.data_directory = options.state_directory / "data";
        }
        if (!options.config_file_explicit) {
            options.config_file = options.state_directory / "config.json";
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
            std::unique_ptr<uhf::config::ConfigStore> config_store =
                std::make_unique<uhf::config::ConfigStore>(options.config_file);
            std::unique_ptr<uhf::app::GatewayRuntime> runtime;
            if (options.start_acquisition) {
                uhf::app::GatewayRuntimeOptions runtime_options;
                runtime_options.simulate = options.simulate;
                runtime_options.acquisition_device = options.acquisition_device;
                runtime_options.poll_interval = std::chrono::seconds(6);
                runtime_options.start_modbus_tcp = true;
                runtime_options.modbus_tcp_bind = options.modbus_tcp_bind;
                runtime_options.modbus_tcp_port = options.modbus_tcp_port;
                runtime_options.start_modbus_rtu = options.start_modbus_rtu;
                runtime_options.modbus_rtu_device = options.modbus_rtu_device;
                runtime_options.persistence_options.data_root = options.data_directory;
                runtime = std::make_unique<uhf::app::GatewayRuntime>(
                    std::move(runtime_options), logger);
                runtime->start();
            }
            uhf::app::GatewayRuntime* runtime_pointer = runtime.get();
            uhf::web::HttpServer server(
                std::move(options.document_root),
                std::move(options.bind_address),
                options.port,
                std::move(options.state_directory),
                runtime_pointer == nullptr ? nullptr : &runtime_pointer->snapshot_store(),
                runtime_pointer == nullptr
                    ? uhf::web::HealthInputProvider{}
                    : uhf::web::HealthInputProvider{
                          [runtime_pointer] { return runtime_pointer->health_input(); }},
                config_store.get());
            const int result = server.run();
            if (runtime) {
                runtime->stop();
            }
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
