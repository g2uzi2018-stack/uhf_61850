// SPDX-License-Identifier: GPL-3.0-only
#include "platform/network/linux_backend.hpp"

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <fcntl.h>
#include <fstream>
#include <initializer_list>
#include <optional>
#include <string_view>
#include <sys/wait.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace {

constexpr char kIpCommand[] = "/usr/sbin/ip";
constexpr std::size_t kMaxConfigBytes = 16U * 1024U;
constexpr mode_t kDirectoryMode = S_IRWXU;
constexpr mode_t kFileMode = S_IRUSR | S_IWUSR;
constexpr std::string_view kCandidateRouteMetric = "42700";

std::string address_with_prefix(const uhf::network::InterfaceConfig& config) {
    return config.address + "/" + std::to_string(config.prefix);
}

bool run_ip(
    uhf::network::CommandRunner& runner,
    std::initializer_list<std::string> suffix) {
    std::vector<std::string> arguments;
    arguments.reserve(suffix.size() + 1U);
    arguments.emplace_back(kIpCommand);
    arguments.insert(arguments.end(), suffix.begin(), suffix.end());
    return runner.run(arguments);
}

std::optional<std::string> read_file(const std::filesystem::path& path) {
    std::error_code error;
    if (!std::filesystem::exists(path, error)) {
        return error ? std::nullopt : std::optional<std::string>{};
    }
    if (error || !std::filesystem::is_regular_file(path, error) || error ||
        std::filesystem::file_size(path, error) > kMaxConfigBytes || error) {
        return std::nullopt;
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return std::nullopt;
    }
    std::string contents{
        std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    if (input.bad() || contents.size() > kMaxConfigBytes) {
        return std::nullopt;
    }
    return contents;
}

bool write_atomic(const std::filesystem::path& path, std::string_view contents) noexcept {
    const std::filesystem::path temporary = path.string() + ".tmp";
    const int file_descriptor = ::open(
        temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, kFileMode);
    if (file_descriptor < 0) {
        return false;
    }
    std::size_t written = 0U;
    while (written < contents.size()) {
        const ssize_t result = ::write(
            file_descriptor, contents.data() + written, contents.size() - written);
        if (result <= 0) {
            ::close(file_descriptor);
            ::unlink(temporary.c_str());
            return false;
        }
        written += static_cast<std::size_t>(result);
    }
    if (::fchmod(file_descriptor, kFileMode) < 0 || ::fsync(file_descriptor) < 0 ||
        ::close(file_descriptor) < 0 || ::rename(temporary.c_str(), path.c_str()) < 0) {
        ::close(file_descriptor);
        ::unlink(temporary.c_str());
        return false;
    }
    const std::filesystem::path parent = path.parent_path().empty() ? "." : path.parent_path();
    const int directory_descriptor = ::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (directory_descriptor < 0) {
        return false;
    }
    const int result = ::fsync(directory_descriptor);
    ::close(directory_descriptor);
    return result == 0;
}

}  // namespace

namespace uhf::network {

bool ExecCommandRunner::run(const std::vector<std::string>& arguments) {
    if (arguments.empty() || arguments.front() != kIpCommand) {
        return false;
    }
    std::vector<char*> argv;
    argv.reserve(arguments.size() + 1U);
    for (const std::string& argument : arguments) {
        argv.push_back(const_cast<char*>(argument.c_str()));
    }
    argv.push_back(nullptr);

    const pid_t child = ::fork();
    if (child < 0) {
        return false;
    }
    if (child == 0) {
        const int null_device = ::open("/dev/null", O_RDWR | O_CLOEXEC);
        if (null_device >= 0) {
            (void)::dup2(null_device, STDOUT_FILENO);
            (void)::dup2(null_device, STDERR_FILENO);
            if (null_device > STDERR_FILENO) {
                ::close(null_device);
            }
        }
        ::execv(kIpCommand, argv.data());
        _exit(127);
    }

    int status = 0;
    while (::waitpid(child, &status, 0) < 0) {
        if (errno != EINTR) {
            return false;
        }
    }
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

LinuxNetworkBackend::LinuxNetworkBackend(
    std::filesystem::path persistent_file, CommandRunner& command_runner)
    : persistent_file_(std::move(persistent_file)), command_runner_(command_runner) {}

bool LinuxNetworkBackend::read_current(NetworkConfig& config) {
    if (load(config)) {
        return true;
    }
    config = NetworkConfig{};
    return save(config);
}

bool LinuxNetworkBackend::apply_stage(
    const NetworkConfig& previous, const NetworkConfig& candidate) {
    if (staged_ || !validate(previous).valid || !validate(candidate).valid) {
        return false;
    }
    if (candidate.eth0.mode == Mode::dhcp || candidate.eth1.mode == Mode::dhcp) {
        return false;
    }
    if (!apply_address_additions(previous, candidate)) {
        return false;
    }
    staged_previous_ = previous;
    staged_candidate_ = candidate;
    staged_ = true;
    return true;
}

bool LinuxNetworkBackend::confirm(
    const NetworkConfig& previous, const NetworkConfig& candidate) {
    if (!validate(previous).valid || !validate(candidate).valid ||
        (staged_ && (staged_previous_.eth0.address != previous.eth0.address ||
                     staged_candidate_.eth0.address != candidate.eth0.address))) {
        return false;
    }
    if (!remove_previous_state(previous, candidate)) {
        (void)restore_previous_state(previous, candidate);
        return false;
    }
    if (!save(candidate)) {
        (void)restore_previous_state(previous, candidate);
        return false;
    }
    staged_ = false;
    return true;
}

bool LinuxNetworkBackend::rollback(
    const NetworkConfig& previous, const NetworkConfig& candidate) {
    if (!validate(previous).valid || !validate(candidate).valid ||
        (staged_ && (previous.eth0.address != staged_previous_.eth0.address ||
                     candidate.eth0.address != staged_candidate_.eth0.address))) {
        return false;
    }
    if (!remove_candidate_state(previous, candidate)) {
        return false;
    }
    staged_ = false;
    return true;
}

bool LinuxNetworkBackend::load(NetworkConfig& config) const {
    const std::optional<std::string> contents = read_file(persistent_file_);
    if (!contents) {
        return false;
    }
    if (contents->empty()) {
        return false;
    }
    return parse_flat_json(*contents, config);
}

bool LinuxNetworkBackend::save(const NetworkConfig& config) const noexcept {
    const std::filesystem::path directory =
        persistent_file_.parent_path().empty() ? "." : persistent_file_.parent_path();
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error || !std::filesystem::is_directory(directory, error) || error ||
        ::chmod(directory.c_str(), kDirectoryMode) < 0) {
        return false;
    }
    return write_atomic(persistent_file_, to_flat_json(config) + "\n");
}

bool LinuxNetworkBackend::apply_address_additions(
    const NetworkConfig& previous, const NetworkConfig& candidate) {
    bool eth0_added = false;
    bool eth1_added = false;
    const auto add_address = [this](
        const InterfaceConfig& old_config, const InterfaceConfig& new_config) {
        return old_config.address == new_config.address && old_config.prefix == new_config.prefix
            ? true
            : run_ip(command_runner_, {
                  "address", "add", address_with_prefix(new_config), "dev", new_config.name});
    };
    if (previous.eth0.address != candidate.eth0.address || previous.eth0.prefix != candidate.eth0.prefix) {
        if (!add_address(previous.eth0, candidate.eth0)) {
            return false;
        }
        eth0_added = true;
    }
    if (previous.eth1.address != candidate.eth1.address || previous.eth1.prefix != candidate.eth1.prefix) {
        if (!add_address(previous.eth1, candidate.eth1)) {
            if (eth0_added) {
                (void)run_ip(command_runner_, {
                    "address", "del", address_with_prefix(candidate.eth0), "dev", candidate.eth0.name});
            }
            return false;
        }
        eth1_added = true;
    }
    const auto add_route = [this](
        const InterfaceConfig& old_config, const InterfaceConfig& new_config) {
        if (old_config.gateway == new_config.gateway) {
            return true;
        }
        if (new_config.gateway.empty()) {
            return true;
        }
        return run_ip(command_runner_, {
            "route", "add", "default", "via", new_config.gateway, "dev", new_config.name,
            "metric", std::string(kCandidateRouteMetric)});
    };
    if (!add_route(previous.eth0, candidate.eth0) || !add_route(previous.eth1, candidate.eth1)) {
        if (eth1_added) {
            (void)run_ip(command_runner_, {
                "address", "del", address_with_prefix(candidate.eth1), "dev", candidate.eth1.name});
        }
        if (eth0_added) {
            (void)run_ip(command_runner_, {
                "address", "del", address_with_prefix(candidate.eth0), "dev", candidate.eth0.name});
        }
        return false;
    }
    return true;
}

bool LinuxNetworkBackend::remove_candidate_state(
    const NetworkConfig& previous, const NetworkConfig& candidate) {
    bool success = true;
    const auto remove_address = [this, &success](
        const InterfaceConfig& old_config, const InterfaceConfig& new_config) {
        if (old_config.address != new_config.address || old_config.prefix != new_config.prefix) {
            success = run_ip(command_runner_, {
                "address", "del", address_with_prefix(new_config), "dev", new_config.name}) && success;
        }
    };
    remove_address(previous.eth0, candidate.eth0);
    remove_address(previous.eth1, candidate.eth1);
    if (previous.eth0.gateway != candidate.eth0.gateway && !candidate.eth0.gateway.empty()) {
        success = run_ip(command_runner_, {
            "route", "del", "default", "via", candidate.eth0.gateway, "dev", candidate.eth0.name,
            "metric", std::string(kCandidateRouteMetric)}) && success;
    }
    if (previous.eth1.gateway != candidate.eth1.gateway && !candidate.eth1.gateway.empty()) {
        success = run_ip(command_runner_, {
            "route", "del", "default", "via", candidate.eth1.gateway, "dev", candidate.eth1.name,
            "metric", std::string(kCandidateRouteMetric)}) && success;
    }
    return success;
}

bool LinuxNetworkBackend::remove_previous_state(
    const NetworkConfig& previous, const NetworkConfig& candidate) {
    bool success = true;
    if (previous.eth0.gateway != candidate.eth0.gateway && !previous.eth0.gateway.empty()) {
        success = run_ip(command_runner_, {
            "route", "del", "default", "via", previous.eth0.gateway, "dev", previous.eth0.name}) && success;
    }
    if (previous.eth1.gateway != candidate.eth1.gateway && !previous.eth1.gateway.empty()) {
        success = run_ip(command_runner_, {
            "route", "del", "default", "via", previous.eth1.gateway, "dev", previous.eth1.name}) && success;
    }
    if (previous.eth0.address != candidate.eth0.address || previous.eth0.prefix != candidate.eth0.prefix) {
        success = run_ip(command_runner_, {
            "address", "del", address_with_prefix(previous.eth0), "dev", previous.eth0.name}) && success;
    }
    if (previous.eth1.address != candidate.eth1.address || previous.eth1.prefix != candidate.eth1.prefix) {
        success = run_ip(command_runner_, {
            "address", "del", address_with_prefix(previous.eth1), "dev", previous.eth1.name}) && success;
    }
    return success;
}

bool LinuxNetworkBackend::restore_previous_state(
    const NetworkConfig& previous, const NetworkConfig& candidate) {
    bool success = true;
    if (previous.eth0.address != candidate.eth0.address || previous.eth0.prefix != candidate.eth0.prefix) {
        success = run_ip(command_runner_, {
            "address", "add", address_with_prefix(previous.eth0), "dev", previous.eth0.name}) && success;
    }
    if (previous.eth1.address != candidate.eth1.address || previous.eth1.prefix != candidate.eth1.prefix) {
        success = run_ip(command_runner_, {
            "address", "add", address_with_prefix(previous.eth1), "dev", previous.eth1.name}) && success;
    }
    if (previous.eth0.gateway != candidate.eth0.gateway && !previous.eth0.gateway.empty()) {
        success = run_ip(command_runner_, {
            "route", "add", "default", "via", previous.eth0.gateway, "dev", previous.eth0.name}) && success;
    }
    if (previous.eth1.gateway != candidate.eth1.gateway && !previous.eth1.gateway.empty()) {
        success = run_ip(command_runner_, {
            "route", "add", "default", "via", previous.eth1.gateway, "dev", previous.eth1.name}) && success;
    }
    return success;
}

}  // namespace uhf::network
