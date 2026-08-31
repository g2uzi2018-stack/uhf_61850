// SPDX-License-Identifier: GPL-3.0-only
#include "platform/network/dhcp_client.hpp"

#include <arpa/inet.h>
#include <algorithm>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <fcntl.h>
#include <fstream>
#include <limits>
#include <poll.h>
#include <csignal>
#include <sstream>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

constexpr std::size_t kMaxLeaseBytes = 4096U;
constexpr mode_t kDirectoryMode = S_IRWXU;
constexpr mode_t kFileMode = S_IRUSR | S_IWUSR;

bool valid_ipv4(std::string_view value) noexcept {
    in_addr address{};
    return !value.empty() && ::inet_pton(AF_INET, std::string(value).c_str(), &address) == 1;
}

bool same_subnet(
    std::string_view address, std::uint8_t prefix, std::string_view gateway) noexcept {
    in_addr parsed_address{};
    in_addr parsed_gateway{};
    if (::inet_pton(AF_INET, std::string(address).c_str(), &parsed_address) != 1 ||
        ::inet_pton(AF_INET, std::string(gateway).c_str(), &parsed_gateway) != 1 ||
        prefix == 0U || prefix > 32U) {
        return false;
    }
    const std::uint32_t address_host = ntohl(parsed_address.s_addr);
    const std::uint32_t gateway_host = ntohl(parsed_gateway.s_addr);
    const std::uint32_t mask = static_cast<std::uint32_t>(0xFFFFFFFFU << (32U - prefix));
    return (address_host & mask) == (gateway_host & mask) && address_host != gateway_host;
}

std::string token_or_dash(std::string_view value) {
    return value.empty() ? "-" : std::string(value);
}

bool dash_or_token(const std::string& value, std::string& output) {
    if (value == "-") {
        output.clear();
        return true;
    }
    output = value;
    return true;
}

bool write_atomic(const std::filesystem::path& path, std::string_view contents) noexcept {
    const std::filesystem::path directory = path.parent_path().empty() ? "." : path.parent_path();
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error || !std::filesystem::is_directory(directory, error) || error ||
        ::chmod(directory.c_str(), kDirectoryMode) < 0) {
        return false;
    }
    const std::filesystem::path temporary = path.string() + ".tmp";
    const int descriptor = ::open(
        temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, kFileMode);
    if (descriptor < 0) {
        return false;
    }
    std::size_t written = 0U;
    while (written < contents.size()) {
        const ssize_t result = ::write(
            descriptor, contents.data() + written, contents.size() - written);
        if (result <= 0) {
            ::close(descriptor);
            (void)::unlink(temporary.c_str());
            return false;
        }
        written += static_cast<std::size_t>(result);
    }
    const bool ready = ::fchmod(descriptor, kFileMode) == 0 && ::fsync(descriptor) == 0;
    const bool closed = ::close(descriptor) == 0;
    if (!ready || !closed || ::rename(temporary.c_str(), path.c_str()) < 0) {
        (void)::unlink(temporary.c_str());
        return false;
    }
    const int directory_descriptor = ::open(
        directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (directory_descriptor < 0) {
        return false;
    }
    const int result = ::fsync(directory_descriptor);
    (void)::close(directory_descriptor);
    return result == 0;
}

std::optional<std::string> read_file(const std::filesystem::path& path) {
    std::error_code error;
    if (!std::filesystem::exists(path, error)) {
        return error ? std::nullopt : std::optional<std::string>{};
    }
    if (error || !std::filesystem::is_regular_file(path, error) || error ||
        std::filesystem::file_size(path, error) > kMaxLeaseBytes || error) {
        return std::nullopt;
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return std::nullopt;
    }
    std::string contents{
        std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    if (input.bad() || contents.size() > kMaxLeaseBytes) {
        return std::nullopt;
    }
    return contents;
}

enum class ReadStatus {
    none,
    valid,
    invalid,
    io_error,
};

ReadStatus read_lines(
    const std::filesystem::path& path,
    std::array<std::optional<uhf::network::DhcpLease>, 2U>& leases) {
    std::error_code error;
    if (!std::filesystem::exists(path, error)) {
        return error ? ReadStatus::io_error : ReadStatus::none;
    }
    if (error || !std::filesystem::is_regular_file(path, error) || error ||
        std::filesystem::file_size(path, error) > kMaxLeaseBytes || error) {
        return ReadStatus::io_error;
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return ReadStatus::io_error;
    }
    std::string contents{
        std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    if (input.bad() || contents.size() > kMaxLeaseBytes) {
        return ReadStatus::io_error;
    }

    std::istringstream lines(contents);
    std::string version;
    if (!std::getline(lines, version) || version != "version=1") {
        return ReadStatus::invalid;
    }
    for (std::size_t index = 0U; index < leases.size(); ++index) {
        std::string line;
        if (!std::getline(lines, line)) {
            return ReadStatus::invalid;
        }
        std::istringstream fields(line);
        std::string label;
        std::string address;
        if (!(fields >> label >> address) || label != (index == 0U ? "eth0" : "eth1")) {
            return ReadStatus::invalid;
        }
        if (address == "-") {
            leases[index] = std::nullopt;
            continue;
        }
        std::uint32_t prefix = 0U;
        std::string gateway;
        std::string dns1;
        std::string dns2;
        if (!(fields >> prefix >> gateway >> dns1 >> dns2) || prefix > 32U) {
            return ReadStatus::invalid;
        }
        uhf::network::DhcpLease lease;
        lease.address = address;
        lease.prefix = static_cast<std::uint8_t>(prefix);
        if (!dash_or_token(gateway, lease.gateway) || !dash_or_token(dns1, lease.dns[0U]) ||
            !dash_or_token(dns2, lease.dns[1U])) {
            return ReadStatus::invalid;
        }
        lease.dns_count = 0U;
        for (const std::string& dns : lease.dns) {
            if (!dns.empty()) {
                if (lease.dns_count >= uhf::network::kMaxDnsServers) {
                    return ReadStatus::invalid;
                }
                lease.dns[lease.dns_count++] = dns;
            }
        }
        if (!uhf::network::validate_lease(lease)) {
            return ReadStatus::invalid;
        }
        leases[index] = std::move(lease);
    }
    std::string extra;
    if (std::getline(lines, extra)) {
        return ReadStatus::invalid;
    }
    return ReadStatus::valid;
}

std::string serialize(
    const std::array<std::optional<uhf::network::DhcpLease>, 2U>& leases) {
    std::ostringstream output;
    output << "version=1\n";
    for (std::size_t index = 0U; index < leases.size(); ++index) {
        output << (index == 0U ? "eth0" : "eth1") << ' ';
        if (!leases[index]) {
            output << "-\n";
            continue;
        }
        const uhf::network::DhcpLease& lease = *leases[index];
        output << lease.address << ' ' << static_cast<unsigned int>(lease.prefix) << ' '
               << token_or_dash(lease.gateway) << ' '
               << token_or_dash(lease.dns_count > 0U ? lease.dns[0U] : "") << ' '
               << token_or_dash(lease.dns_count > 1U ? lease.dns[1U] : "") << '\n';
    }
    return output.str();
}

bool remove_file(const std::filesystem::path& path) noexcept {
    return ::unlink(path.c_str()) == 0 || errno == ENOENT;
}

bool read_key_line(
    std::istringstream& input, std::string_view key, std::string& value) {
    std::string line;
    if (!std::getline(input, line) || line.rfind(key, 0U) != 0U) {
        return false;
    }
    value = line.substr(key.size());
    return value.find_first_of("\r\n") == std::string::npos && value.size() <= 256U;
}

std::optional<std::string> read_process_cmdline(pid_t pid) {
    if (pid <= 0) {
        return std::nullopt;
    }
    const std::filesystem::path path =
        std::filesystem::path{"/proc"} / std::to_string(pid) / "cmdline";
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error) || error ||
        std::filesystem::file_size(path, error) > 4096U || error) {
        return std::nullopt;
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return std::nullopt;
    }
    std::string contents{
        std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    if (input.bad() || contents.empty() || contents.size() > 4096U) {
        return std::nullopt;
    }
    return contents;
}

bool has_cmdline_token(std::string_view cmdline, std::string_view token) {
    std::size_t begin = 0U;
    while (begin < cmdline.size()) {
        const std::size_t end = cmdline.find('\0', begin);
        const std::size_t token_end = end == std::string_view::npos ? cmdline.size() : end;
        if (cmdline.substr(begin, token_end - begin) == token) {
            return true;
        }
        if (end == std::string_view::npos) {
            break;
        }
        begin = end + 1U;
    }
    return false;
}

int open_pidfd(pid_t pid) noexcept {
#if defined(SYS_pidfd_open)
    return static_cast<int>(::syscall(SYS_pidfd_open, pid, 0U));
#else
    static_cast<void>(pid);
    return -1;
#endif
}

bool send_pidfd_signal(int pidfd, int signal_number) noexcept {
#if defined(SYS_pidfd_send_signal)
    if (::syscall(SYS_pidfd_send_signal, pidfd, signal_number, nullptr, 0U) == 0) {
        return true;
    }
    return errno == ESRCH;
#else
    static_cast<void>(pidfd);
    static_cast<void>(signal_number);
    return false;
#endif
}

bool wait_pidfd_exit(int pidfd, std::chrono::steady_clock::time_point deadline) noexcept {
    while (std::chrono::steady_clock::now() < deadline) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        const int timeout = static_cast<int>(std::max<std::int64_t>(
            1, std::min<std::int64_t>(remaining.count(), 50)));
        pollfd descriptor{pidfd, POLLIN, 0};
        const int result = ::poll(&descriptor, 1, timeout);
        if (result > 0 && (descriptor.revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL)) != 0) {
            return true;
        }
        if (result < 0 && errno != EINTR) {
            return false;
        }
    }
    return false;
}

bool parse_prefix(std::string_view mask_text, std::uint8_t& prefix) noexcept {
    in_addr parsed{};
    if (::inet_pton(AF_INET, std::string(mask_text).c_str(), &parsed) != 1) {
        return false;
    }
    const std::uint32_t mask = ntohl(parsed.s_addr);
    std::uint8_t count = 0U;
    bool zero_seen = false;
    for (int bit = 31; bit >= 0; --bit) {
        const bool one = (mask & (static_cast<std::uint32_t>(1U) << bit)) != 0U;
        if (one && zero_seen) {
            return false;
        }
        if (one) {
            ++count;
        } else {
            zero_seen = true;
        }
    }
    if (count == 0U) {
        return false;
    }
    prefix = count;
    return true;
}

std::vector<std::string> dhclient_arguments(
    const std::filesystem::path& executable,
    const std::filesystem::path& hook,
    const std::filesystem::path& pid,
    const std::filesystem::path& lease,
    const uhf::network::InterfaceConfig& config,
    bool one_shot) {
    std::vector<std::string> arguments = {
        executable.string(), one_shot ? "-1" : "-nw", "-sf", hook.string(), "-pf", pid.string(),
        "-lf", lease.string()};
    if (!config.hostname.empty()) {
        arguments.emplace_back("-H");
        arguments.push_back(config.hostname);
    }
    arguments.push_back(config.name);
    return arguments;
}

pid_t spawn_dhclient(
    std::vector<std::string>& arguments,
    const std::filesystem::path& result,
    const uhf::network::InterfaceConfig& config) noexcept {
    std::vector<char*> argv;
    argv.reserve(arguments.size() + 1U);
    for (std::string& argument : arguments) {
        argv.push_back(argument.data());
    }
    argv.push_back(nullptr);

    const pid_t child = ::fork();
    if (child < 0) {
        return -1;
    }
    if (child == 0) {
        if (::setenv("UHF_DHCP_RESULT_FILE", result.c_str(), 1) != 0 ||
            ::setenv("UHF_DHCP_INTERFACE", config.name.c_str(), 1) != 0) {
            _exit(126);
        }
        const int null_device = ::open("/dev/null", O_RDWR | O_CLOEXEC);
        if (null_device >= 0) {
            (void)::dup2(null_device, STDOUT_FILENO);
            (void)::dup2(null_device, STDERR_FILENO);
            if (null_device > STDERR_FILENO) {
                ::close(null_device);
            }
        }
        ::execv(arguments.front().c_str(), argv.data());
        _exit(127);
    }
    return child;
}

bool run_dhclient(
    const std::filesystem::path& executable,
    const std::filesystem::path& hook,
    const std::filesystem::path& result,
    const std::filesystem::path& pid,
    const std::filesystem::path& lease,
    const uhf::network::InterfaceConfig& config) noexcept {
    std::vector<std::string> arguments = dhclient_arguments(
        executable, hook, pid, lease, config, true);
    const pid_t child = spawn_dhclient(arguments, result, config);
    if (child < 0) {
        return false;
    }

    int status = 0;
    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::seconds(config.dhcp_timeout_seconds);
    while (std::chrono::steady_clock::now() < deadline) {
        const pid_t result_pid = ::waitpid(child, &status, WNOHANG);
        if (result_pid == child) {
            return WIFEXITED(status) && WEXITSTATUS(status) == 0;
        }
        if (result_pid < 0 && errno != EINTR) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    (void)::kill(child, SIGTERM);
    const auto terminate_deadline = std::chrono::steady_clock::now() +
        std::chrono::milliseconds(250);
    while (std::chrono::steady_clock::now() < terminate_deadline) {
        const pid_t result_pid = ::waitpid(child, &status, WNOHANG);
        if (result_pid == child || (result_pid < 0 && errno == ECHILD)) {
            return false;
        }
        if (result_pid < 0 && errno != EINTR) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    (void)::kill(child, SIGKILL);
    while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {
    }
    return false;
}

}  // namespace

namespace uhf::network {

bool validate_lease(const DhcpLease& lease) noexcept {
    if (!valid_ipv4(lease.address) || lease.prefix == 0U || lease.prefix > 32U ||
        lease.dns_count > kMaxDnsServers) {
        return false;
    }
    if (!lease.gateway.empty() && !same_subnet(lease.address, lease.prefix, lease.gateway)) {
        return false;
    }
    for (std::size_t index = 0U; index < lease.dns_count; ++index) {
        if (!valid_ipv4(lease.dns[index])) {
            return false;
        }
    }
    return true;
}

LeaseStore::LeaseStore(std::filesystem::path file) : file_(std::move(file)) {}

LeaseLoadResult LeaseStore::load() const {
    std::array<std::optional<DhcpLease>, 2U> leases;
    const ReadStatus status = read_lines(file_, leases);
    switch (status) {
    case ReadStatus::none:
        return {LeaseLoadStatus::none, {}};
    case ReadStatus::valid:
        return {LeaseLoadStatus::valid, std::move(leases)};
    case ReadStatus::invalid:
        return {LeaseLoadStatus::corrupt, {}};
    case ReadStatus::io_error:
        return {LeaseLoadStatus::io_error, {}};
    }
    return {LeaseLoadStatus::io_error, {}};
}

bool LeaseStore::save(const std::array<std::optional<DhcpLease>, 2U>& leases) const noexcept {
    for (const std::optional<DhcpLease>& lease : leases) {
        if (lease && !validate_lease(*lease)) {
            return false;
        }
    }
    return write_atomic(file_, serialize(leases));
}

bool LeaseStore::clear() const noexcept {
    if (::unlink(file_.c_str()) < 0 && errno != ENOENT) {
        return false;
    }
    const std::filesystem::path directory = file_.parent_path().empty() ? "." : file_.parent_path();
    const int descriptor = ::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (descriptor < 0) {
        return false;
    }
    const int result = ::fsync(descriptor);
    (void)::close(descriptor);
    return result == 0;
}

ExecDhcpClient::ExecDhcpClient(
    std::filesystem::path state_directory,
    std::filesystem::path dhclient_path,
    std::filesystem::path hook_path)
    : state_directory_(std::move(state_directory)),
      dhclient_path_(std::move(dhclient_path)),
      hook_path_(std::move(hook_path)) {}

int ExecDhcpClient::interface_index(const InterfaceConfig& config) const noexcept {
    if (config.name == "eth0") {
        return 0;
    }
    if (config.name == "eth1") {
        return 1;
    }
    return -1;
}

std::filesystem::path ExecDhcpClient::result_path(
    const InterfaceConfig& config) const {
    return state_directory_ / (config.name + ".result");
}

std::filesystem::path ExecDhcpClient::pid_path(
    const InterfaceConfig& config) const {
    return state_directory_ / (config.name + ".pid");
}

std::filesystem::path ExecDhcpClient::lease_path(
    const InterfaceConfig& config) const {
    return state_directory_ / (config.name + ".lease");
}

std::filesystem::path ExecDhcpClient::probe_result_path(
    const InterfaceConfig& config) const {
    return state_directory_ / (config.name + ".probe.result");
}

std::filesystem::path ExecDhcpClient::probe_pid_path(
    const InterfaceConfig& config) const {
    return state_directory_ / (config.name + ".probe.pid");
}

std::filesystem::path ExecDhcpClient::probe_lease_path(
    const InterfaceConfig& config) const {
    return state_directory_ / (config.name + ".probe.lease");
}

bool ExecDhcpClient::read_result(
    const std::filesystem::path& result,
    const InterfaceConfig& config,
    DhcpLease& lease) const {
    const std::optional<std::string> contents = read_file(result);
    if (!contents) {
        return false;
    }
    std::istringstream input(*contents);
    std::string value;
    std::string mask;
    std::string gateway;
    std::string dns1;
    std::string dns2;
    if (!read_key_line(input, "version=", value) || value != "1" ||
        !read_key_line(input, "interface=", value) || value != config.name ||
        !read_key_line(input, "address=", lease.address) ||
        !read_key_line(input, "mask=", mask) ||
        !read_key_line(input, "gateway=", gateway) ||
        !read_key_line(input, "dns1=", dns1) ||
        !read_key_line(input, "dns2=", dns2)) {
        return false;
    }
    if (!parse_prefix(mask, lease.prefix)) {
        return false;
    }
    lease.gateway = gateway == "-" ? "" : gateway;
    lease.dns_count = 0U;
    for (const std::string& dns : {dns1, dns2}) {
        if (dns == "-") {
            continue;
        }
        if (lease.dns_count >= kMaxDnsServers) {
            return false;
        }
        lease.dns[lease.dns_count++] = dns;
    }
    std::string extra;
    if (std::getline(input, extra) || !validate_lease(lease)) {
        return false;
    }
    return true;
}

bool ExecDhcpClient::acquire(const InterfaceConfig& config, DhcpLease& lease) {
    if (config.mode != Mode::dhcp || (config.name != "eth0" && config.name != "eth1") ||
        state_directory_.empty() || dhclient_path_.empty() || hook_path_.empty()) {
        return false;
    }
    std::error_code error;
    std::filesystem::create_directories(state_directory_, error);
    if (error || !std::filesystem::is_directory(state_directory_, error) || error ||
        ::chmod(state_directory_.c_str(), kDirectoryMode) < 0) {
        return false;
    }
    const std::filesystem::path result = probe_result_path(config);
    const std::filesystem::path pid = probe_pid_path(config);
    const std::filesystem::path lease_file = probe_lease_path(config);
    if (!remove_file(result) || !remove_file(pid)) {
        return false;
    }
    if (!run_dhclient(
            dhclient_path_, hook_path_, result, pid, lease_file, config) ||
        !read_result(result, config, lease)) {
        (void)remove_file(result);
        (void)remove_file(pid);
        (void)remove_file(lease_file);
        return false;
    }
    (void)remove_file(result);
    (void)remove_file(pid);
    (void)remove_file(lease_file);
    return true;
}

pid_t ExecDhcpClient::stored_pid(const InterfaceConfig& config) const noexcept {
    const std::optional<std::string> contents = read_file(pid_path(config));
    if (!contents || contents->empty()) {
        return -1;
    }
    std::string_view value_text = *contents;
    while (!value_text.empty() &&
           std::isspace(static_cast<unsigned char>(value_text.back())) != 0) {
        value_text.remove_suffix(1U);
    }
    if (value_text.empty()) {
        return -1;
    }
    std::uint64_t value = 0U;
    const auto result = std::from_chars(
        value_text.data(), value_text.data() + value_text.size(), value);
    if (result.ec != std::errc{} || value == 0U ||
        result.ptr != value_text.data() + value_text.size() ||
        value > static_cast<std::uint64_t>(std::numeric_limits<pid_t>::max())) {
        return -1;
    }
    return static_cast<pid_t>(value);
}

bool ExecDhcpClient::process_matches(const InterfaceConfig& config, pid_t pid) const noexcept {
    const std::optional<std::string> cmdline = read_process_cmdline(pid);
    if (!cmdline) {
        return false;
    }
    const std::string executable = dhclient_path_.string();
    const std::string executable_name = dhclient_path_.filename().string();
    return (has_cmdline_token(*cmdline, executable) ||
            (!executable_name.empty() && has_cmdline_token(*cmdline, executable_name))) &&
        has_cmdline_token(*cmdline, config.name) &&
        has_cmdline_token(*cmdline, pid_path(config).string());
}

bool ExecDhcpClient::terminate_pid(const InterfaceConfig& config, pid_t pid) const noexcept {
    if (pid <= 0) {
        return true;
    }
    const int pidfd = open_pidfd(pid);
    if (pidfd >= 0) {
        if (!process_matches(config, pid)) {
            (void)::close(pidfd);
            return true;
        }
        if (!send_pidfd_signal(pidfd, SIGTERM)) {
            (void)::close(pidfd);
            return false;
        }
        const auto deadline = std::chrono::steady_clock::now() +
            std::chrono::milliseconds(250);
        if (wait_pidfd_exit(pidfd, deadline)) {
            (void)::close(pidfd);
            return true;
        }
        if (!send_pidfd_signal(pidfd, SIGKILL)) {
            (void)::close(pidfd);
            return false;
        }
        const auto kill_deadline = std::chrono::steady_clock::now() +
            std::chrono::milliseconds(250);
        const bool exited = wait_pidfd_exit(pidfd, kill_deadline);
        (void)::close(pidfd);
        return exited;
    }
    if (!process_matches(config, pid)) {
        return true;
    }
    if (::kill(pid, SIGTERM) < 0 && errno != ESRCH) {
        return false;
    }
    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::milliseconds(250);
    while (std::chrono::steady_clock::now() < deadline) {
        if (!process_matches(config, pid) || (::kill(pid, 0) < 0 && errno == ESRCH)) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    if (!process_matches(config, pid)) {
        return true;
    }
    if (::kill(pid, SIGKILL) < 0 && errno != ESRCH) {
        return false;
    }
    const auto kill_deadline = std::chrono::steady_clock::now() +
        std::chrono::milliseconds(250);
    while (std::chrono::steady_clock::now() < kill_deadline) {
        if (!process_matches(config, pid) || (::kill(pid, 0) < 0 && errno == ESRCH)) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return !process_matches(config, pid) || (::kill(pid, 0) < 0 && errno == ESRCH);
}

bool ExecDhcpClient::start(
    const InterfaceConfig& config, const DhcpLease& lease) {
    const int index = interface_index(config);
    if (config.mode != Mode::dhcp || index < 0 || !validate_lease(lease) ||
        state_directory_.empty() || dhclient_path_.empty() || hook_path_.empty()) {
        return false;
    }
    std::error_code error;
    std::filesystem::create_directories(state_directory_, error);
    if (error || !std::filesystem::is_directory(state_directory_, error) || error ||
        ::chmod(state_directory_.c_str(), kDirectoryMode) < 0) {
        return false;
    }

    if (running_pids_[static_cast<std::size_t>(index)] > 0 &&
        process_matches(config, running_pids_[static_cast<std::size_t>(index)]) &&
        ::kill(running_pids_[static_cast<std::size_t>(index)], 0) == 0) {
        return true;
    }
    running_pids_[static_cast<std::size_t>(index)] = -1;
    const pid_t existing = stored_pid(config);
    if (existing > 0 && process_matches(config, existing) && ::kill(existing, 0) == 0) {
        running_pids_[static_cast<std::size_t>(index)] = existing;
        return true;
    }
    (void)remove_file(pid_path(config));
    if (!remove_file(result_path(config))) {
        return false;
    }
    std::vector<std::string> arguments = dhclient_arguments(
        dhclient_path_, hook_path_, pid_path(config),
        lease_path(config), config, false);
    const pid_t child = spawn_dhclient(arguments, result_path(config), config);
    if (child < 0) {
        return false;
    }
    running_pids_[static_cast<std::size_t>(index)] = child;
    int status = 0;
    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::milliseconds(250);
    while (std::chrono::steady_clock::now() < deadline) {
        const pid_t result_pid = ::waitpid(child, &status, WNOHANG);
        if (result_pid == child) {
            running_pids_[static_cast<std::size_t>(index)] = -1;
            return WIFEXITED(status) && WEXITSTATUS(status) == 0;
        }
        if (result_pid < 0 && errno != EINTR) {
            running_pids_[static_cast<std::size_t>(index)] = -1;
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return true;
}

bool ExecDhcpClient::current_lease(
    const InterfaceConfig& config, DhcpLease& lease) const {
    return interface_index(config) >= 0 && read_result(result_path(config), config, lease);
}

bool ExecDhcpClient::stop(
    const InterfaceConfig& config, const DhcpLease& lease) noexcept {
    static_cast<void>(lease);
    const int index = interface_index(config);
    if (index < 0) {
        return false;
    }
    pid_t pid = running_pids_[static_cast<std::size_t>(index)];
    if (pid <= 0) {
        pid = stored_pid(config);
    }
    const bool stopped = terminate_pid(config, pid);
    running_pids_[static_cast<std::size_t>(index)] = -1;
    return stopped && remove_file(result_path(config)) && remove_file(pid_path(config));
}

bool ExecDhcpClient::release(
    const InterfaceConfig& config, const DhcpLease& lease) noexcept {
    static_cast<void>(lease);
    if (config.name != "eth0" && config.name != "eth1") {
        return false;
    }
    return remove_file(probe_result_path(config)) && remove_file(probe_pid_path(config)) &&
        remove_file(probe_lease_path(config));
}

}  // namespace uhf::network
