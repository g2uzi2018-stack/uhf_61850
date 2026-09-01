// SPDX-License-Identifier: GPL-3.0-only
#include "platform/network/linux_backend.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <fcntl.h>
#include <fstream>
#include <initializer_list>
#include <string>
#include <optional>
#include <sstream>
#include <string_view>
#include <sys/types.h>
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

bool run_ip(
    uhf::network::CommandRunner& runner,
    std::initializer_list<std::string> suffix) {
    std::vector<std::string> arguments;
    arguments.reserve(suffix.size() + 1U);
    arguments.emplace_back(kIpCommand);
    arguments.insert(arguments.end(), suffix.begin(), suffix.end());
    return runner.run(arguments);
}

bool run_ip_allow_missing(
    uhf::network::CommandRunner& runner,
    std::initializer_list<std::string> suffix) {
    std::vector<std::string> arguments;
    arguments.reserve(suffix.size() + 1U);
    arguments.emplace_back(kIpCommand);
    arguments.insert(arguments.end(), suffix.begin(), suffix.end());
    return runner.run_allow_missing(arguments);
}

bool is_missing_network_object(std::string_view error) noexcept {
    return error.find("Cannot assign requested address") != std::string_view::npos ||
        error.find("No such process") != std::string_view::npos ||
        error.find("Cannot find device") != std::string_view::npos;
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

std::string effective_address(
    const uhf::network::InterfaceConfig& config,
    const std::optional<uhf::network::DhcpLease>& lease) {
    if (config.mode == uhf::network::Mode::dhcp && lease) {
        return lease->address + "/" + std::to_string(lease->prefix);
    }
    return config.address + "/" + std::to_string(config.prefix);
}

std::string effective_gateway(
    const uhf::network::InterfaceConfig& config,
    const std::optional<uhf::network::DhcpLease>& lease) {
    return config.mode == uhf::network::Mode::dhcp && lease ? lease->gateway : config.gateway;
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

struct FileSnapshot {
    bool exists{false};
    std::string contents;
};

std::optional<FileSnapshot> snapshot_file(const std::filesystem::path& path) {
    std::error_code error;
    const std::filesystem::file_status status = std::filesystem::symlink_status(path, error);
    if (error == std::make_error_code(std::errc::no_such_file_or_directory) ||
        (!error && status.type() == std::filesystem::file_type::not_found)) {
        return FileSnapshot{};
    }
    if (error || status.type() != std::filesystem::file_type::regular) {
        return std::nullopt;
    }
    const std::optional<std::string> contents = read_file(path);
    if (!contents) {
        return std::nullopt;
    }
    return FileSnapshot{true, *contents};
}

bool sync_parent_directory(const std::filesystem::path& path) noexcept {
    const std::filesystem::path parent = path.parent_path().empty() ? "." : path.parent_path();
    const int descriptor = ::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (descriptor < 0) {
        return false;
    }
    const int result = ::fsync(descriptor);
    (void)::close(descriptor);
    return result == 0;
}

bool remove_file(const std::filesystem::path& path) noexcept {
    if (::unlink(path.c_str()) < 0 && errno != ENOENT) {
        return false;
    }
    return sync_parent_directory(path);
}

bool restore_file(
    const std::filesystem::path& path, const FileSnapshot& snapshot) noexcept {
    if (snapshot.exists) {
        return write_atomic(path, snapshot.contents);
    }
    return remove_file(path);
}

bool prefix_to_netmask(std::uint8_t prefix, std::string& output) noexcept {
    if (prefix == 0U || prefix > 32U) {
        return false;
    }
    const std::uint32_t host_mask =
        prefix == 32U ? 0xFFFFFFFFU : (0xFFFFFFFFU << (32U - prefix));
    in_addr address{htonl(host_mask)};
    char buffer[INET_ADDRSTRLEN]{};
    if (::inet_ntop(AF_INET, &address, buffer, sizeof(buffer)) == nullptr) {
        return false;
    }
    output = buffer;
    return true;
}

bool netmask_to_prefix(std::string_view value, std::uint8_t& output) noexcept {
    in_addr address{};
    if (::inet_pton(AF_INET, std::string(value).c_str(), &address) != 1) {
        return false;
    }
    const std::uint32_t mask = ntohl(address.s_addr);
    std::uint8_t prefix = 0U;
    bool zero_seen = false;
    for (int bit = 31; bit >= 0; --bit) {
        const bool one = (mask & (static_cast<std::uint32_t>(1U) << bit)) != 0U;
        if (one && zero_seen) {
            return false;
        }
        if (one) {
            ++prefix;
        } else {
            zero_seen = true;
        }
    }
    if (prefix == 0U) {
        return false;
    }
    output = prefix;
    return true;
}

std::string trim(std::string_view value) {
    std::size_t begin = 0U;
    while (begin < value.size() &&
           std::isspace(static_cast<unsigned char>(value[begin])) != 0) {
        ++begin;
    }
    std::size_t end = value.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1U])) != 0) {
        --end;
    }
    return std::string(value.substr(begin, end - begin));
}

std::string unquote(std::string value) {
    if (value.size() >= 2U &&
        ((value.front() == '"' && value.back() == '"') ||
         (value.front() == '\'' && value.back() == '\''))) {
        value = value.substr(1U, value.size() - 2U);
    }
    return value;
}

bool parse_vendor_file(
    const std::filesystem::path& path, uhf::network::InterfaceConfig& config) {
    const std::optional<std::string> contents = read_file(path);
    if (!contents || contents->empty()) {
        return false;
    }
    std::string method;
    std::string address;
    std::string netmask;
    std::string gateway;
    bool method_seen = false;
    bool address_seen = false;
    bool netmask_seen = false;
    bool gateway_seen = false;
    std::istringstream lines(*contents);
    std::string line;
    while (std::getline(lines, line)) {
        const std::string entry = trim(line);
        if (entry.empty() || entry.front() == '#') {
            continue;
        }
        const std::size_t separator = entry.find('=');
        if (separator == std::string::npos) {
            return false;
        }
        const std::string key = trim(entry.substr(0U, separator));
        const std::string value = unquote(trim(entry.substr(separator + 1U)));
        if (key == "METHOD") {
            if (method_seen) {
                return false;
            }
            method = value;
            method_seen = true;
        } else if (key == "IPADDR") {
            if (address_seen) {
                return false;
            }
            address = value;
            address_seen = true;
        } else if (key == "NETMASK") {
            if (netmask_seen) {
                return false;
            }
            netmask = value;
            netmask_seen = true;
        } else if (key == "GATEWAY") {
            if (gateway_seen) {
                return false;
            }
            gateway = value;
            gateway_seen = true;
        }
    }
    if (!method_seen || !netmask_seen) {
        return false;
    }
    std::uint8_t prefix = 0U;
    if (!netmask_to_prefix(netmask, prefix)) {
        return false;
    }
    config.prefix = prefix;
    config.dns = {};
    config.dns_count = 0U;
    config.hostname.clear();
    config.dhcp_timeout_seconds = 15U;
    if (method == "STATIC") {
        if (!address_seen) {
            return false;
        }
        config.mode = uhf::network::Mode::static_address;
        config.address = address;
        config.gateway = gateway;
    } else if (method == "DHCP" || method == "DHCP_DNS") {
        config.mode = uhf::network::Mode::dhcp;
        config.address.clear();
        config.gateway.clear();
    } else {
        return false;
    }
    return true;
}

std::string vendor_file_contents(const uhf::network::InterfaceConfig& config) {
    std::string netmask;
    if (!prefix_to_netmask(config.prefix, netmask)) {
        return {};
    }
    const std::string method = config.mode == uhf::network::Mode::dhcp
        ? (config.dns_count == 0U ? "DHCP" : "DHCP_DNS")
        : "STATIC";
    return "METHOD=" + method + "\nIPADDR=" + config.address +
        "\nNETMASK=" + netmask + "\nGATEWAY=" + config.gateway + "\n";
}

}  // namespace

namespace uhf::network {

bool execute_ip_command(
    const std::vector<std::string>& arguments, bool allow_missing) {
    if (arguments.empty() || arguments.front() != kIpCommand) {
        return false;
    }
    std::vector<char*> argv;
    argv.reserve(arguments.size() + 1U);
    for (const std::string& argument : arguments) {
        argv.push_back(const_cast<char*>(argument.c_str()));
    }
    argv.push_back(nullptr);

    int error_pipe[2] = {-1, -1};
    if (allow_missing && ::pipe2(error_pipe, O_CLOEXEC) < 0) {
        return false;
    }
    const pid_t child = ::fork();
    if (child < 0) {
        if (error_pipe[0] >= 0) {
            ::close(error_pipe[0]);
            ::close(error_pipe[1]);
        }
        return false;
    }
    if (child == 0) {
        if (allow_missing) {
            ::close(error_pipe[0]);
            (void)::dup2(error_pipe[1], STDERR_FILENO);
            if (error_pipe[1] > STDERR_FILENO) {
                ::close(error_pipe[1]);
            }
        } else {
            const int null_device = ::open("/dev/null", O_RDWR | O_CLOEXEC);
            if (null_device >= 0) {
                (void)::dup2(null_device, STDOUT_FILENO);
                (void)::dup2(null_device, STDERR_FILENO);
                if (null_device > STDERR_FILENO) {
                    ::close(null_device);
                }
            }
        }
        ::execv(kIpCommand, argv.data());
        _exit(127);
    }

    if (allow_missing) {
        ::close(error_pipe[1]);
    }
    std::string error_output;
    if (allow_missing) {
        char buffer[256];
        ssize_t bytes_read = 0;
        while ((bytes_read = ::read(error_pipe[0], buffer, sizeof(buffer))) > 0) {
            error_output.append(buffer, static_cast<std::size_t>(bytes_read));
            if (error_output.size() > 4096U) {
                error_output.resize(4096U);
                break;
            }
        }
        ::close(error_pipe[0]);
    }

    int status = 0;
    while (::waitpid(child, &status, 0) < 0) {
        if (errno != EINTR) {
            return false;
        }
    }
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        return true;
    }
    return allow_missing && is_missing_network_object(error_output);
}

bool ExecCommandRunner::run(const std::vector<std::string>& arguments) {
    return execute_ip_command(arguments, false);
}

bool ExecCommandRunner::run_allow_missing(const std::vector<std::string>& arguments) {
    return execute_ip_command(arguments, true);
}

LinuxNetworkBackend::LinuxNetworkBackend(
    std::filesystem::path persistent_file,
    CommandRunner& command_runner,
    DhcpClient* dhcp_client,
    VendorNetworkPaths vendor_paths)
    : persistent_file_(std::move(persistent_file)),
      command_runner_(command_runner),
      dhcp_client_(dhcp_client),
      vendor_paths_(std::move(vendor_paths)),
      lease_store_(persistent_file_.string() + ".leases"),
      staged_lease_store_(persistent_file_.string() + ".staged-leases"),
      previous_lease_store_(persistent_file_.string() + ".previous-leases") {}

bool LinuxNetworkBackend::read_current(NetworkConfig& config) {
    if (load(config)) {
        if (config.eth0.mode == Mode::dhcp || config.eth1.mode == Mode::dhcp) {
            std::array<std::optional<DhcpLease>, 2U> leases;
            if (!load_leases(lease_store_, leases) ||
                (config.eth0.mode == Mode::dhcp && !leases[0U]) ||
                (config.eth1.mode == Mode::dhcp && !leases[1U])) {
                return false;
            }
        }
        if (!vendor_paths_.eth0_config.empty() || !vendor_paths_.eth1_config.empty()) {
            if (vendor_paths_.eth0_config.empty() || vendor_paths_.eth1_config.empty()) {
                return false;
            }
            std::error_code vendor_error;
            const bool eth0_exists = std::filesystem::exists(vendor_paths_.eth0_config, vendor_error);
            const bool eth1_exists = std::filesystem::exists(vendor_paths_.eth1_config, vendor_error);
            if (vendor_error) {
                return false;
            }
            if (!eth0_exists || !eth1_exists) {
                return save(config);
            }
        }
        return true;
    }
    std::error_code error;
    const std::filesystem::file_status status =
        std::filesystem::symlink_status(persistent_file_, error);
    if ((error && error != std::make_error_code(std::errc::no_such_file_or_directory)) ||
        (!error && status.type() != std::filesystem::file_type::not_found)) {
        return false;
    }
    if (!vendor_paths_.eth0_config.empty() || !vendor_paths_.eth1_config.empty()) {
        if (vendor_paths_.eth0_config.empty() || vendor_paths_.eth1_config.empty()) {
            return false;
        }
        std::error_code vendor_error;
        const bool eth0_exists = std::filesystem::exists(vendor_paths_.eth0_config, vendor_error);
        const bool eth1_exists = std::filesystem::exists(vendor_paths_.eth1_config, vendor_error);
        if (vendor_error || eth0_exists != eth1_exists) {
            return false;
        }
        if (eth0_exists) {
            if (!load_vendor(config)) {
                return false;
            }
            if (config.eth0.mode == Mode::dhcp || config.eth1.mode == Mode::dhcp) {
                std::array<std::optional<DhcpLease>, 2U> leases;
                if (!load_leases(lease_store_, leases) ||
                    (config.eth0.mode == Mode::dhcp && !leases[0U]) ||
                    (config.eth1.mode == Mode::dhcp && !leases[1U])) {
                    return false;
                }
            }
            return save(config);
        }
    }
    config = NetworkConfig{};
    return save(config);
}

bool LinuxNetworkBackend::apply_stage(
    const NetworkConfig& previous, const NetworkConfig& candidate) {
    if (staged_ || !validate(previous).valid || !validate(candidate).valid) {
        return false;
    }
    std::array<std::optional<DhcpLease>, 2U> previous_leases;
    if (!load_leases(lease_store_, previous_leases) ||
        (previous.eth0.mode == Mode::dhcp && !previous_leases[0U]) ||
        (previous.eth1.mode == Mode::dhcp && !previous_leases[1U])) {
        return false;
    }
    if (!previous_lease_store_.clear() || !previous_lease_store_.save(previous_leases)) {
        return false;
    }

    std::array<std::optional<DhcpLease>, 2U> candidate_leases;
    const auto release_candidate_leases = [&]() noexcept {
        bool success = true;
        if (dhcp_client_ == nullptr) {
            return true;
        }
        for (std::size_t index = 0U; index < candidate_leases.size(); ++index) {
            if (candidate_leases[index]) {
                const InterfaceConfig& config = index == 0U ? candidate.eth0 : candidate.eth1;
                success = dhcp_client_->release(config, *candidate_leases[index]) && success;
            }
        }
        return success;
    };
    for (std::size_t index = 0U; index < candidate_leases.size(); ++index) {
        const InterfaceConfig& config = index == 0U ? candidate.eth0 : candidate.eth1;
        if (config.mode != Mode::dhcp) {
            continue;
        }
        if (dhcp_client_ == nullptr) {
            return false;
        }
        DhcpLease lease;
        if (!dhcp_client_->acquire(config, lease) || !validate_lease(lease)) {
            (void)release_candidate_leases();
            return false;
        }
        candidate_leases[index] = std::move(lease);
    }
    if (!staged_lease_store_.save(candidate_leases) ||
        !apply_address_additions(previous, candidate, previous_leases, candidate_leases)) {
        (void)remove_candidate_state(previous, candidate, previous_leases, candidate_leases);
        (void)release_candidate_leases();
        (void)staged_lease_store_.clear();
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
        (staged_ && (to_flat_json(staged_previous_) != to_flat_json(previous) ||
                     to_flat_json(staged_candidate_) != to_flat_json(candidate)))) {
        return false;
    }
    std::array<std::optional<DhcpLease>, 2U> previous_leases;
    std::array<std::optional<DhcpLease>, 2U> candidate_leases;
    if (!load_leases(lease_store_, previous_leases)) {
        return false;
    }
    const LeaseLoadResult staged = staged_lease_store_.load();
    if (staged.status == LeaseLoadStatus::valid) {
        candidate_leases = staged.leases;
    } else if (staged.status == LeaseLoadStatus::none) {
        candidate_leases = {};
    } else {
        return false;
    }
    if ((candidate.eth0.mode == Mode::dhcp && !candidate_leases[0U]) ||
        (candidate.eth1.mode == Mode::dhcp && !candidate_leases[1U]) ||
        (previous.eth0.mode == Mode::dhcp && !previous_leases[0U]) ||
        (previous.eth1.mode == Mode::dhcp && !previous_leases[1U])) {
        return false;
    }

    std::array<bool, 2U> stopped_previous{{false, false}};
    const auto restore_previous_clients = [&]() noexcept {
        bool success = true;
        if (dhcp_client_ == nullptr) {
            return true;
        }
        for (std::size_t index = 0U; index < stopped_previous.size(); ++index) {
            if (!stopped_previous[index]) {
                continue;
            }
            const InterfaceConfig& config = index == 0U ? previous.eth0 : previous.eth1;
            success = dhcp_client_->start(config, *previous_leases[index]) && success;
        }
        return success;
    };
    for (std::size_t index = 0U; index < stopped_previous.size(); ++index) {
        const InterfaceConfig& config = index == 0U ? previous.eth0 : previous.eth1;
        if (config.mode != Mode::dhcp) {
            continue;
        }
        if (dhcp_client_ == nullptr || !dhcp_client_->stop(config, *previous_leases[index])) {
            (void)restore_previous_clients();
            return false;
        }
        stopped_previous[index] = true;
    }
    if (!remove_previous_state(previous, candidate, previous_leases, candidate_leases)) {
        (void)restore_previous_state(previous, candidate, previous_leases, candidate_leases);
        (void)restore_previous_clients();
        return false;
    }
    if (!save(candidate)) {
        (void)restore_previous_state(previous, candidate, previous_leases, candidate_leases);
        (void)restore_previous_clients();
        return false;
    }
    if (!lease_store_.save(candidate_leases) || !staged_lease_store_.clear()) {
        (void)save(previous);
        (void)restore_previous_state(previous, candidate, previous_leases, candidate_leases);
        (void)restore_previous_clients();
        return false;
    }
    staged_ = false;
    return true;
}

bool LinuxNetworkBackend::rollback(
    const NetworkConfig& previous, const NetworkConfig& candidate) {
    if (!validate(previous).valid || !validate(candidate).valid ||
        (staged_ && (to_flat_json(staged_previous_) != to_flat_json(previous) ||
                     to_flat_json(staged_candidate_) != to_flat_json(candidate)))) {
        return false;
    }
    std::array<std::optional<DhcpLease>, 2U> current_leases;
    if (!load_leases(lease_store_, current_leases)) {
        return false;
    }
    std::array<std::optional<DhcpLease>, 2U> previous_leases = current_leases;
    const LeaseLoadResult previous_backup = previous_lease_store_.load();
    if (previous_backup.status == LeaseLoadStatus::valid) {
        previous_leases = previous_backup.leases;
    } else if (previous_backup.status != LeaseLoadStatus::none) {
        return false;
    }
    std::array<std::optional<DhcpLease>, 2U> candidate_leases;
    const LeaseLoadResult staged = staged_lease_store_.load();
    if (staged.status == LeaseLoadStatus::valid) {
        candidate_leases = staged.leases;
    } else if (staged.status == LeaseLoadStatus::none) {
        candidate_leases = current_leases;
    } else {
        return false;
    }
    if ((candidate.eth0.mode == Mode::dhcp && !candidate_leases[0U]) ||
        (candidate.eth1.mode == Mode::dhcp && !candidate_leases[1U])) {
        return false;
    }
    if (!save(previous) || !lease_store_.save(previous_leases)) {
        return false;
    }
    const bool state_removed = remove_candidate_state(
        previous, candidate, previous_leases, candidate_leases);
    bool leases_released = true;
    if (dhcp_client_ != nullptr) {
        for (std::size_t index = 0U; index < candidate_leases.size(); ++index) {
            if (candidate_leases[index]) {
                const InterfaceConfig& config = index == 0U ? candidate.eth0 : candidate.eth1;
                leases_released = dhcp_client_->release(config, *candidate_leases[index]) &&
                    leases_released;
            }
        }
    }
    if (!state_removed || !leases_released || !staged_lease_store_.clear() ||
        !previous_lease_store_.clear()) {
        return false;
    }
    staged_ = false;
    return true;
}

bool LinuxNetworkBackend::start_runtime() {
    NetworkConfig current;
    if (!read_current(current)) {
        return false;
    }
    if (!apply_static_runtime_state(current)) {
        return false;
    }
    std::array<std::optional<DhcpLease>, 2U> leases;
    if (!load_leases(lease_store_, leases)) {
        return false;
    }
    for (std::size_t index = 0U; index < leases.size(); ++index) {
        const InterfaceConfig& config = index == 0U ? current.eth0 : current.eth1;
        if (config.mode != Mode::dhcp) {
            continue;
        }
        if (dhcp_client_ == nullptr || !leases[index] ||
            !validate_lease(*leases[index]) || !dhcp_client_->start(config, *leases[index])) {
            return false;
        }
    }
    return true;
}

bool LinuxNetworkBackend::apply_static_runtime_state(const NetworkConfig& current) {
    for (const InterfaceConfig* config : {&current.eth0, &current.eth1}) {
        if (config->mode != Mode::static_address) {
            continue;
        }
        if (!run_ip(command_runner_, {
                "address", "replace", config->address + "/" + std::to_string(config->prefix),
                "dev", config->name})) {
            return false;
        }
        if (!config->gateway.empty() &&
            !run_ip(command_runner_, {
                "route", "replace", "default", "via", config->gateway, "dev", config->name})) {
            return false;
        }
    }
    return true;
}

bool LinuxNetworkBackend::refresh_runtime() {
    NetworkConfig current;
    if (!read_current(current)) {
        return false;
    }
    std::array<std::optional<DhcpLease>, 2U> previous_leases;
    if (!load_leases(lease_store_, previous_leases)) {
        return false;
    }

    std::array<std::optional<DhcpLease>, 2U> refreshed_leases = previous_leases;
    bool changed = false;
    for (std::size_t index = 0U; index < refreshed_leases.size(); ++index) {
        const InterfaceConfig& config = index == 0U ? current.eth0 : current.eth1;
        if (config.mode != Mode::dhcp) {
            continue;
        }
        if (dhcp_client_ == nullptr || !previous_leases[index]) {
            return false;
        }
        DhcpLease lease;
        if (!dhcp_client_->current_lease(config, lease)) {
            continue;
        }
        if (!validate_lease(lease)) {
            return false;
        }
        if (!refreshed_leases[index] ||
            refreshed_leases[index]->address != lease.address ||
            refreshed_leases[index]->prefix != lease.prefix ||
            refreshed_leases[index]->gateway != lease.gateway ||
            refreshed_leases[index]->dns_count != lease.dns_count ||
            refreshed_leases[index]->dns != lease.dns) {
            refreshed_leases[index] = std::move(lease);
            changed = true;
        }
    }
    if (!changed) {
        return true;
    }

    if (!apply_address_additions(
            current, current, previous_leases, refreshed_leases)) {
        return false;
    }
    if (!remove_previous_state(
            current, current, previous_leases, refreshed_leases)) {
        (void)remove_candidate_state(current, current, previous_leases, refreshed_leases);
        (void)restore_previous_state(current, current, previous_leases, refreshed_leases);
        return false;
    }
    if (!lease_store_.save(refreshed_leases)) {
        (void)remove_candidate_state(current, current, previous_leases, refreshed_leases);
        (void)restore_previous_state(current, current, previous_leases, refreshed_leases);
        return false;
    }
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

bool LinuxNetworkBackend::load_vendor(NetworkConfig& config) const {
    if (vendor_paths_.eth0_config.empty() || vendor_paths_.eth1_config.empty()) {
        return false;
    }
    NetworkConfig parsed;
    if (!parse_vendor_file(vendor_paths_.eth0_config, parsed.eth0) ||
        !parse_vendor_file(vendor_paths_.eth1_config, parsed.eth1) ||
        !validate(parsed).valid) {
        return false;
    }
    config = std::move(parsed);
    return true;
}

bool LinuxNetworkBackend::load_leases(
    const LeaseStore& store,
    std::array<std::optional<DhcpLease>, 2U>& leases) const {
    const LeaseLoadResult result = store.load();
    if (result.status == LeaseLoadStatus::none) {
        leases = {};
        return true;
    }
    if (result.status != LeaseLoadStatus::valid) {
        return false;
    }
    leases = result.leases;
    return true;
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
    if (!validate(config).valid) {
        return false;
    }
    const std::optional<FileSnapshot> previous = snapshot_file(persistent_file_);
    if (!previous || !write_atomic(persistent_file_, to_flat_json(config) + "\n")) {
        if (previous) {
            (void)restore_file(persistent_file_, *previous);
        }
        return false;
    }
    if (save_vendor(config)) {
        return true;
    }
    (void)restore_file(persistent_file_, *previous);
    return false;
}

bool LinuxNetworkBackend::save_vendor(const NetworkConfig& config) const noexcept {
    const bool eth0_configured = !vendor_paths_.eth0_config.empty();
    const bool eth1_configured = !vendor_paths_.eth1_config.empty();
    if (!eth0_configured && !eth1_configured) {
        return true;
    }
    if (!eth0_configured || !eth1_configured) {
        return false;
    }
    const std::optional<FileSnapshot> previous_eth0 = snapshot_file(vendor_paths_.eth0_config);
    const std::optional<FileSnapshot> previous_eth1 = snapshot_file(vendor_paths_.eth1_config);
    if (!previous_eth0 || !previous_eth1) {
        return false;
    }
    const std::string eth0_contents = vendor_file_contents(config.eth0);
    const std::string eth1_contents = vendor_file_contents(config.eth1);
    if (eth0_contents.empty() || eth1_contents.empty() ||
        !write_atomic(vendor_paths_.eth0_config, eth0_contents) ||
        !write_atomic(vendor_paths_.eth1_config, eth1_contents)) {
        (void)restore_file(vendor_paths_.eth0_config, *previous_eth0);
        (void)restore_file(vendor_paths_.eth1_config, *previous_eth1);
        return false;
    }
    return true;
}

bool LinuxNetworkBackend::apply_address_additions(
    const NetworkConfig& previous,
    const NetworkConfig& candidate,
    const std::array<std::optional<DhcpLease>, 2U>& previous_leases,
    const std::array<std::optional<DhcpLease>, 2U>& candidate_leases) {
    const InterfaceConfig* old_configs[] = {&previous.eth0, &previous.eth1};
    const InterfaceConfig* new_configs[] = {&candidate.eth0, &candidate.eth1};
    for (std::size_t index = 0U; index < 2U; ++index) {
        const std::string old_address = effective_address(*old_configs[index], previous_leases[index]);
        const std::string new_address = effective_address(*new_configs[index], candidate_leases[index]);
        if (old_address == new_address) {
            continue;
        }
        if (!run_ip(command_runner_, {
                "address", "add", new_address, "dev", new_configs[index]->name})) {
            return false;
        }
    }
    for (std::size_t index = 0U; index < 2U; ++index) {
        const std::string old_gateway = effective_gateway(*old_configs[index], previous_leases[index]);
        const std::string new_gateway = effective_gateway(*new_configs[index], candidate_leases[index]);
        if (old_gateway == new_gateway || new_gateway.empty()) {
            continue;
        }
        if (!run_ip(command_runner_, {
                "route", "add", "default", "via", new_gateway, "dev", new_configs[index]->name,
                "metric", std::string(kCandidateRouteMetric)})) {
            (void)remove_candidate_state(previous, candidate, previous_leases, candidate_leases);
            return false;
        }
    }
    return true;
}

bool LinuxNetworkBackend::remove_candidate_state(
    const NetworkConfig& previous,
    const NetworkConfig& candidate,
    const std::array<std::optional<DhcpLease>, 2U>& previous_leases,
    const std::array<std::optional<DhcpLease>, 2U>& candidate_leases) {
    bool success = true;
    const InterfaceConfig* old_configs[] = {&previous.eth0, &previous.eth1};
    const InterfaceConfig* new_configs[] = {&candidate.eth0, &candidate.eth1};
    for (std::size_t index = 0U; index < 2U; ++index) {
        const std::string old_address = effective_address(*old_configs[index], previous_leases[index]);
        const std::string new_address = effective_address(*new_configs[index], candidate_leases[index]);
        if (old_address != new_address) {
            success = run_ip_allow_missing(command_runner_, {
                "address", "del", new_address, "dev", new_configs[index]->name}) && success;
        }
        const std::string old_gateway = effective_gateway(*old_configs[index], previous_leases[index]);
        const std::string new_gateway = effective_gateway(*new_configs[index], candidate_leases[index]);
        if (old_gateway != new_gateway && !new_gateway.empty()) {
            success = run_ip_allow_missing(command_runner_, {
                "route", "del", "default", "via", new_gateway, "dev", new_configs[index]->name,
                "metric", std::string(kCandidateRouteMetric)}) && success;
        }
    }
    return success;
}

bool LinuxNetworkBackend::remove_previous_state(
    const NetworkConfig& previous,
    const NetworkConfig& candidate,
    const std::array<std::optional<DhcpLease>, 2U>& previous_leases,
    const std::array<std::optional<DhcpLease>, 2U>& candidate_leases) {
    bool success = true;
    const InterfaceConfig* old_configs[] = {&previous.eth0, &previous.eth1};
    const InterfaceConfig* new_configs[] = {&candidate.eth0, &candidate.eth1};
    for (std::size_t index = 0U; index < 2U; ++index) {
        const std::string old_address = effective_address(*old_configs[index], previous_leases[index]);
        const std::string new_address = effective_address(*new_configs[index], candidate_leases[index]);
        const std::string old_gateway = effective_gateway(*old_configs[index], previous_leases[index]);
        const std::string new_gateway = effective_gateway(*new_configs[index], candidate_leases[index]);
        if (old_gateway != new_gateway && !old_gateway.empty()) {
            success = run_ip_allow_missing(command_runner_, {
                "route", "del", "default", "via", old_gateway, "dev", old_configs[index]->name}) && success;
        }
        if (old_address != new_address) {
            success = run_ip_allow_missing(command_runner_, {
                "address", "del", old_address, "dev", old_configs[index]->name}) && success;
        }
    }
    return success;
}

bool LinuxNetworkBackend::restore_previous_state(
    const NetworkConfig& previous,
    const NetworkConfig& candidate,
    const std::array<std::optional<DhcpLease>, 2U>& previous_leases,
    const std::array<std::optional<DhcpLease>, 2U>& candidate_leases) {
    bool success = true;
    const InterfaceConfig* old_configs[] = {&previous.eth0, &previous.eth1};
    const InterfaceConfig* new_configs[] = {&candidate.eth0, &candidate.eth1};
    for (std::size_t index = 0U; index < 2U; ++index) {
        const std::string old_address = effective_address(*old_configs[index], previous_leases[index]);
        const std::string new_address = effective_address(*new_configs[index], candidate_leases[index]);
        const std::string old_gateway = effective_gateway(*old_configs[index], previous_leases[index]);
        const std::string new_gateway = effective_gateway(*new_configs[index], candidate_leases[index]);
        if (old_address != new_address) {
            success = run_ip(command_runner_, {
                "address", "add", old_address, "dev", old_configs[index]->name}) && success;
        }
        if (old_gateway != new_gateway && !old_gateway.empty()) {
            success = run_ip(command_runner_, {
                "route", "add", "default", "via", old_gateway, "dev", old_configs[index]->name}) && success;
        }
    }
    return success;
}

}  // namespace uhf::network
