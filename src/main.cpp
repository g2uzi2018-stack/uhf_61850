// SPDX-License-Identifier: GPL-3.0-only
#include "app/build_info.hpp"
#include "app/gateway_runtime.hpp"
#include "activation/credential_file.hpp"
#include "config/config_store.hpp"
#include "logging/logger.hpp"
#include "platform/privileged/unix_socket.hpp"
#include "web/http_server.hpp"

#include <charconv>
#include <cmath>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace {

struct WebOptions {
    std::filesystem::path document_root{"web"};
    std::filesystem::path state_directory{"/var/lib/uhf-gateway"};
    std::string bind_address{"0.0.0.0"};
    std::uint16_t port{8080};
    bool listen_explicit{false};
    bool http_recovery{false};
    bool tls_enabled{true};
    bool tls_certificate_explicit{false};
    bool tls_private_key_explicit{false};
    std::filesystem::path tls_certificate;
    std::filesystem::path tls_private_key;
    bool start_acquisition{true};
    bool simulate{false};
    bool v3_enabled{false};
    std::string acquisition_device{"/dev/ttyS1"};
    bool acquisition_device_explicit{false};
    std::string v3_pd_device{"/dev/ttyS1"};
    std::string v3_current_device{"/dev/ttyS2"};
    std::string v3_temperature_device{"/dev/ttyS3"};
    std::optional<uhf::acquisition::SerialSettings> v3_current_serial_settings;
    std::optional<uhf::acquisition::SerialSettings> v3_temperature_serial_settings;
    bool v3_current_serial_explicit{false};
    bool v3_temperature_serial_explicit{false};
    std::string v3_device_id;
    std::string v3_activation_key;
    std::optional<std::string> v3_activation_code;
    bool v3_device_id_explicit{false};
    bool v3_activation_key_explicit{false};
    bool v3_activation_code_explicit{false};
    std::optional<std::filesystem::path> v3_device_id_file;
    std::optional<std::filesystem::path> v3_activation_key_file;
    std::optional<std::filesystem::path> v3_activation_code_file;
    std::filesystem::path activation_state;
    float v3_current_multiplier{std::numeric_limits<float>::quiet_NaN()};
    float v3_current_offset{std::numeric_limits<float>::quiet_NaN()};
    float v3_temperature_multiplier{std::numeric_limits<float>::quiet_NaN()};
    float v3_temperature_offset{std::numeric_limits<float>::quiet_NaN()};
    bool v3_current_multiplier_explicit{false};
    bool v3_current_offset_explicit{false};
    bool v3_temperature_multiplier_explicit{false};
    bool v3_temperature_offset_explicit{false};
    bool modbus_tcp_explicit{false};
    std::string modbus_tcp_bind{"192.168.3.230"};
    std::uint16_t modbus_tcp_port{502};
    bool modbus_rtu_explicit{false};
    bool start_modbus_rtu{true};
    std::string modbus_rtu_device{"/dev/ttyS4"};
    bool iec61850_explicit{false};
    bool start_iec61850{true};
    std::string iec61850_bind{"192.168.3.230"};
    std::uint16_t iec61850_port{102U};
    std::filesystem::path data_directory{"/var/lib/uhf-gateway/data"};
    bool data_directory_explicit{false};
    std::filesystem::path config_file{"/var/lib/uhf-gateway/config.json"};
    bool config_file_explicit{false};
    std::filesystem::path defaults_file{"/etc/uhf-gateway/defaults.json"};
    std::filesystem::path privileged_socket{"/run/uhf-gateway/privileged.sock"};
    bool privileged_socket_explicit{false};
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

bool parse_float(std::string_view value, float& output) {
    std::string text(value);
    char* end = nullptr;
    errno = 0;
    const float parsed = std::strtof(text.c_str(), &end);
    if (errno != 0 || end == text.c_str() || end != text.c_str() + text.size() ||
        !std::isfinite(parsed)) {
        return false;
    }
    output = parsed;
    return true;
}

void print_usage() {
    std::cerr << "usage: " << uhf::app::kProductName
              << " [--version|--self-test|--web "
                 "[--web-root PATH] [--state-dir PATH] [--listen IPV4:PORT] "
                 "[--http-recovery] [--tls-cert PATH] [--tls-key PATH] "
                 "[--simulate|--v3|--no-acquisition] [--acquisition-device PATH] "
                 "[--v3-pd-device PATH] [--v3-current-device PATH] [--v3-temperature-device PATH] "
                 "[--v3-current-serial BAUD/8N1] [--v3-temperature-serial BAUD/8N1] "
                 "[--v3-device-id ID|--v3-device-id-file PATH] "
                 "[--v3-activation-key KEY|--v3-activation-key-file PATH] "
                 "[--v3-activation-code CODE|--v3-activation-code-file PATH] "
                 "[--activation-state PATH] "
                 "[--v3-current-multiplier N] [--v3-current-offset N] "
                 "[--v3-temperature-multiplier N] [--v3-temperature-offset N] "
                 "[--modbus-tcp-listen IPV4:PORT] [--modbus-rtu-device PATH] "
                 "[--no-modbus-rtu] [--iec61850-listen IPV4:PORT] [--no-iec61850] "
                 "[--data-dir PATH] [--config PATH] [--privileged-socket PATH]]\n";
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
                options.listen_explicit = true;
            } else if (option == "--http-recovery") {
                options.http_recovery = true;
            } else if (option == "--tls-cert" && index + 1 < argc) {
                options.tls_certificate = argv[++index];
                options.tls_certificate_explicit = true;
            } else if (option == "--tls-key" && index + 1 < argc) {
                options.tls_private_key = argv[++index];
                options.tls_private_key_explicit = true;
            } else if (option == "--simulate") {
                options.simulate = true;
            } else if (option == "--v3") {
                options.v3_enabled = true;
                options.simulate = false;
            } else if (option == "--no-acquisition") {
                options.start_acquisition = false;
            } else if (option == "--acquisition-device" && index + 1 < argc) {
                options.acquisition_device = argv[++index];
                options.acquisition_device_explicit = true;
            } else if (option == "--v3-pd-device" && index + 1 < argc) {
                options.v3_pd_device = argv[++index];
            } else if (option == "--v3-current-device" && index + 1 < argc) {
                options.v3_current_device = argv[++index];
            } else if (option == "--v3-temperature-device" && index + 1 < argc) {
                options.v3_temperature_device = argv[++index];
            } else if (option == "--v3-current-serial" && index + 1 < argc) {
                bool valid = false;
                options.v3_current_serial_settings =
                    uhf::acquisition::parse_serial_profile(argv[++index], valid);
                if (!valid || !options.v3_current_serial_settings) {
                    std::cerr << "invalid --v3-current-serial value\n";
                    return 2;
                }
                options.v3_current_serial_explicit = true;
            } else if (option == "--v3-temperature-serial" && index + 1 < argc) {
                bool valid = false;
                options.v3_temperature_serial_settings =
                    uhf::acquisition::parse_serial_profile(argv[++index], valid);
                if (!valid || !options.v3_temperature_serial_settings) {
                    std::cerr << "invalid --v3-temperature-serial value\n";
                    return 2;
                }
                options.v3_temperature_serial_explicit = true;
            } else if (option == "--v3-device-id" && index + 1 < argc) {
                options.v3_device_id = argv[++index];
                options.v3_device_id_explicit = true;
            } else if (option == "--v3-device-id-file" && index + 1 < argc) {
                options.v3_device_id_file = std::filesystem::path(argv[++index]);
            } else if (option == "--v3-activation-key" && index + 1 < argc) {
                options.v3_activation_key = argv[++index];
                options.v3_activation_key_explicit = true;
            } else if (option == "--v3-activation-key-file" && index + 1 < argc) {
                options.v3_activation_key_file = std::filesystem::path(argv[++index]);
            } else if (option == "--v3-activation-code" && index + 1 < argc) {
                options.v3_activation_code = std::string(argv[++index]);
                options.v3_activation_code_explicit = true;
            } else if (option == "--v3-activation-code-file" && index + 1 < argc) {
                options.v3_activation_code_file = std::filesystem::path(argv[++index]);
            } else if (option == "--activation-state" && index + 1 < argc) {
                options.activation_state = argv[++index];
            } else if (option == "--v3-current-multiplier" && index + 1 < argc) {
                if (!parse_float(argv[++index], options.v3_current_multiplier)) {
                    std::cerr << "invalid --v3-current-multiplier value\n";
                    return 2;
                }
                options.v3_current_multiplier_explicit = true;
            } else if (option == "--v3-current-offset" && index + 1 < argc) {
                if (!parse_float(argv[++index], options.v3_current_offset)) {
                    std::cerr << "invalid --v3-current-offset value\n";
                    return 2;
                }
                options.v3_current_offset_explicit = true;
            } else if (option == "--v3-temperature-multiplier" && index + 1 < argc) {
                if (!parse_float(argv[++index], options.v3_temperature_multiplier)) {
                    std::cerr << "invalid --v3-temperature-multiplier value\n";
                    return 2;
                }
                options.v3_temperature_multiplier_explicit = true;
            } else if (option == "--v3-temperature-offset" && index + 1 < argc) {
                if (!parse_float(argv[++index], options.v3_temperature_offset)) {
                    std::cerr << "invalid --v3-temperature-offset value\n";
                    return 2;
                }
                options.v3_temperature_offset_explicit = true;
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
            } else if (option == "--iec61850-listen" && index + 1 < argc) {
                if (!parse_listen(argv[++index], options.iec61850_bind, options.iec61850_port)) {
                    std::cerr << "invalid --iec61850-listen value\n";
                    return 2;
                }
                options.iec61850_explicit = true;
                options.start_iec61850 = true;
            } else if (option == "--no-iec61850") {
                options.iec61850_explicit = true;
                options.start_iec61850 = false;
            } else if (option == "--data-dir" && index + 1 < argc) {
                options.data_directory = argv[++index];
                options.data_directory_explicit = true;
            } else if (option == "--config" && index + 1 < argc) {
                options.config_file = argv[++index];
                options.config_file_explicit = true;
            } else if (option == "--privileged-socket" && index + 1 < argc) {
                options.privileged_socket = argv[++index];
                options.privileged_socket_explicit = true;
            } else {
                print_usage();
                return 2;
            }
        }

        const auto load_credential = [](const std::optional<std::filesystem::path>& path,
                                         std::string_view option_name,
                                         std::string& destination) {
            if (!path) return true;
            std::string error;
            const std::optional<std::string> value =
                uhf::activation::read_credential_file(*path, error);
            if (!value) {
                std::cerr << "invalid " << option_name << ": " << error << '\n';
                return false;
            }
            destination = *value;
            return true;
        };
        if ((options.v3_device_id_explicit && options.v3_device_id_file) ||
            (options.v3_activation_key_explicit && options.v3_activation_key_file) ||
            (options.v3_activation_code_explicit && options.v3_activation_code_file)) {
            std::cerr << "v3 provisioning values must use either inline or file input, not both\n";
            return 2;
        }
        std::string activation_code;
        if (!load_credential(
                options.v3_device_id_file, "--v3-device-id-file", options.v3_device_id) ||
            !load_credential(
                options.v3_activation_key_file, "--v3-activation-key-file",
                options.v3_activation_key) ||
            !load_credential(
                options.v3_activation_code_file, "--v3-activation-code-file",
                activation_code)) {
            return 2;
        }
        if (options.v3_activation_code_file) {
            options.v3_activation_code = std::move(activation_code);
        }

        if (options.v3_current_multiplier_explicit !=
            options.v3_current_offset_explicit) {
            std::cerr << "v3 current multiplier and offset must be provided together\n";
            return 2;
        }
        if (options.v3_temperature_multiplier_explicit !=
            options.v3_temperature_offset_explicit) {
            std::cerr << "v3 temperature multiplier and offset must be provided together\n";
            return 2;
        }
        if ((options.v3_current_multiplier_explicit &&
             options.v3_current_multiplier <= 0.0F) ||
            (options.v3_temperature_multiplier_explicit &&
             options.v3_temperature_multiplier <= 0.0F)) {
            std::cerr << "v3 conversion multipliers must be positive\n";
            return 2;
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
        if (options.simulate && !options.privileged_socket_explicit) {
            options.privileged_socket = options.state_directory / "privileged.sock";
        }
        if (!options.config_file_explicit) {
            options.config_file = options.state_directory / "config.json";
        }
        if (options.activation_state.empty()) {
            options.activation_state = options.state_directory / "activation.state";
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
                std::make_unique<uhf::config::ConfigStore>(options.config_file, options.defaults_file);
            const uhf::config::Snapshot configured = config_store->snapshot();
            if (!options.listen_explicit) {
                options.port = configured.values.web_port;
            }
            if (!options.modbus_tcp_explicit) {
                options.modbus_tcp_bind = "0.0.0.0";
                options.modbus_tcp_port = options.simulate && !options.config_file_explicit
                    ? 15020U
                    : configured.values.modbus_tcp_port;
            }
            if (!options.acquisition_device_explicit) {
                options.acquisition_device = configured.values.acquisition_device;
            }
            if (!options.v3_current_serial_explicit) {
                bool valid = false;
                options.v3_current_serial_settings =
                    uhf::acquisition::parse_serial_profile(
                        configured.values.v3_current_serial, valid);
            }
            if (!options.v3_temperature_serial_explicit) {
                bool valid = false;
                options.v3_temperature_serial_settings =
                    uhf::acquisition::parse_serial_profile(
                        configured.values.v3_temperature_serial, valid);
            }
            if (!options.modbus_rtu_explicit) {
                options.modbus_rtu_device = configured.values.rtu_device;
            }
            if (!options.iec61850_explicit) {
                options.iec61850_bind = "0.0.0.0";
                options.start_iec61850 = configured.values.iec_enabled;
                options.iec61850_port = configured.values.iec_port;
                if (options.simulate && !options.config_file_explicit) {
                    options.iec61850_port = 15102U;
                }
            }
            options.tls_enabled = configured.values.tls_enabled;
            if (options.http_recovery) {
                options.tls_enabled = false;
            } else if (!options.tls_enabled) {
                throw std::runtime_error(
                    "HTTP is disabled by configuration; use --http-recovery only for local recovery");
            }
            if (!options.tls_certificate_explicit) {
                options.tls_certificate = options.state_directory / "tls" / "server.crt";
            }
            if (!options.tls_private_key_explicit) {
                options.tls_private_key = options.state_directory / "tls" / "server.key";
            }
            std::unique_ptr<uhf::app::GatewayRuntime> runtime;
            if (options.start_acquisition) {
                uhf::app::GatewayRuntimeOptions runtime_options;
                runtime_options.simulate = options.simulate;
                runtime_options.v3_enabled = options.v3_enabled;
                runtime_options.config_store = config_store.get();
                runtime_options.acquisition_device = options.acquisition_device;
                runtime_options.v3_pd_device = options.v3_pd_device;
                runtime_options.v3_current_device = options.v3_current_device;
                runtime_options.v3_temperature_device = options.v3_temperature_device;
                runtime_options.v3_current_serial_settings =
                    options.v3_current_serial_settings;
                runtime_options.v3_temperature_serial_settings =
                    options.v3_temperature_serial_settings;
                runtime_options.activation_options.state_file = options.activation_state;
                runtime_options.activation_options.device_id = options.v3_device_id;
                runtime_options.activation_options.manufacturer_key = options.v3_activation_key;
                runtime_options.activation_options.requested_code = options.v3_activation_code;
                runtime_options.v3_scheduler_options.collector.current_scale = {
                    options.v3_current_multiplier, options.v3_current_offset};
                runtime_options.v3_scheduler_options.collector.temperature_scale = {
                    options.v3_temperature_multiplier, options.v3_temperature_offset};
                runtime_options.reload_v3_current_conversion =
                    !options.v3_current_multiplier_explicit;
                runtime_options.reload_v3_temperature_conversion =
                    !options.v3_temperature_multiplier_explicit;
                runtime_options.acquisition_options.slave_id = configured.values.acquisition_slave_id;
                runtime_options.acquisition_options.response_timeout = std::chrono::milliseconds(
                    configured.values.acquisition_response_timeout_ms);
                runtime_options.acquisition_options.max_retries = configured.values.acquisition_max_retries;
                runtime_options.poll_interval = std::chrono::milliseconds(
                    configured.values.acquisition_period_ms);
                runtime_options.start_modbus_tcp = true;
                runtime_options.reload_modbus_tcp_endpoint =
                    !options.modbus_tcp_explicit &&
                    options.modbus_tcp_port == configured.values.modbus_tcp_port;
                runtime_options.modbus_tcp_bind_all = !options.modbus_tcp_explicit;
                runtime_options.modbus_tcp_bind = options.modbus_tcp_bind;
                runtime_options.modbus_tcp_port = options.modbus_tcp_port;
                runtime_options.modbus_tcp_unit_id = configured.values.modbus_tcp_unit_id;
                runtime_options.start_modbus_rtu = options.start_modbus_rtu;
                runtime_options.modbus_rtu_device = options.modbus_rtu_device;
                runtime_options.modbus_rtu_options.unit_id = configured.values.rtu_unit_id;
                runtime_options.start_iec61850 = options.start_iec61850;
                runtime_options.reload_iec61850_endpoint =
                    !options.iec61850_explicit &&
                    options.iec61850_port == configured.values.iec_port;
                runtime_options.iec61850_bind_all = !options.iec61850_explicit;
                runtime_options.iec61850_bind = options.iec61850_bind;
                runtime_options.iec61850_port = options.iec61850_port;
                runtime_options.iec61850_ied_name = configured.values.iec_ied_name;
                runtime_options.iec61850_icd_override = options.state_directory / "UHFPD1.icd";
                runtime_options.iec61850_icd_packaged = "/etc/uhf-gateway/UHFPD1.icd";
                runtime_options.persistence_options.config_store = config_store.get();
                runtime_options.persistence_options.data_root = options.data_directory;
                runtime_options.persistence_options.periodic_period = std::chrono::seconds(
                    configured.values.storage_period_seconds);
                runtime_options.persistence_options.cleaner_options.retention =
                    std::chrono::hours(24U * configured.values.storage_retention_days);
                runtime_options.persistence_options.cleaner_options.min_free_bytes =
                    configured.values.storage_min_free_bytes;
                runtime = std::make_unique<uhf::app::GatewayRuntime>(
                    std::move(runtime_options), logger);
                runtime->start();
            }
            uhf::app::GatewayRuntime* runtime_pointer = runtime.get();
            uhf::privileged::UnixSocketClient network_client(options.privileged_socket);
            if (!options.simulate) {
                const uhf::privileged::Reply time_reply = configured.values.time_sync_enabled
                    ? network_client.time_sync(configured.values.sntp_server)
                    : network_client.time_disable();
                if (!time_reply.ok) {
                    logger.log(
                        uhf::logging::Level::warning,
                        uhf::logging::Component::config,
                        "time.configuration_apply_failed",
                        "Configured time synchronization could not be applied");
                }
            }
            uhf::web::HttpServer server(
                std::move(options.document_root),
                std::move(options.bind_address),
                options.port,
                std::move(options.state_directory),
                runtime_pointer == nullptr || options.v3_enabled
                    ? nullptr : &runtime_pointer->snapshot_store(),
                runtime_pointer == nullptr
                    ? uhf::web::HealthInputProvider{}
                    : uhf::web::HealthInputProvider{
                          [runtime_pointer] { return runtime_pointer->health_input(); }},
                config_store.get(),
                options.tls_enabled,
                uhf::web::TlsFiles{options.tls_certificate, options.tls_private_key},
                &logger,
                options.data_directory,
                &network_client,
                options.port == configured.values.web_port,
                runtime_pointer == nullptr
                    ? uhf::web::Iec61850StatsProvider{}
                    : uhf::web::Iec61850StatsProvider{
                          [runtime_pointer] { return runtime_pointer->iec61850_stats(); }},
                runtime_pointer == nullptr
                    ? uhf::web::Iec61850EndpointProvider{}
                    : uhf::web::Iec61850EndpointProvider{
                          [runtime_pointer] { return runtime_pointer->iec61850_endpoint(); }},
                runtime_pointer == nullptr
                    ? uhf::web::Iec61850ModelProvider{}
                    : uhf::web::Iec61850ModelProvider{
                          [runtime_pointer] { return runtime_pointer->iec61850_model_definition(); }},
                runtime_pointer == nullptr
                    ? uhf::web::Iec61850ReloadHandler{}
                    : uhf::web::Iec61850ReloadHandler{
                          [runtime_pointer] { return runtime_pointer->reload_iec61850_model(); }},
                runtime_pointer == nullptr ? nullptr : runtime_pointer->v3_snapshot_store(),
                runtime_pointer == nullptr ? nullptr : runtime_pointer->v3_packet_trace());
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
