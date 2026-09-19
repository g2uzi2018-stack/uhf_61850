// SPDX-License-Identifier: GPL-3.0-only
#include "platform/network/dhcp_client.hpp"
#include "test_check.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fcntl.h>
#include <fstream>
#include <poll.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

namespace {

void write_executable(const std::filesystem::path& path, const char* contents) {
    std::ofstream output(path);
    UHF_TEST_CHECK(output);
    output << contents;
    output.close();
    UHF_TEST_CHECK(::chmod(path.c_str(), S_IRWXU) == 0);
}

uhf::network::InterfaceConfig dhcp_config() {
    uhf::network::InterfaceConfig config;
    config.name = "eth0";
    config.mode = uhf::network::Mode::dhcp;
    config.address.clear();
    config.gateway.clear();
    config.dns_count = 0U;
    config.hostname = "smoke-host";
    config.dhcp_timeout_seconds = 15U;
    return config;
}

bool wait_for_exec_boundary(int descriptor, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        pollfd ready{descriptor, POLLIN | POLLHUP, 0};
        const int result = ::poll(
            &ready, 1, static_cast<int>(std::max<std::int64_t>(1, remaining.count())));
        if (result > 0 && (ready.revents & (POLLIN | POLLHUP)) != 0) {
            char value = 0;
            ssize_t read_result = -1;
            do {
                read_result = ::read(descriptor, &value, 1U);
            } while (read_result < 0 && errno == EINTR);
            return read_result == 0;
        }
        if (result > 0 && (ready.revents & (POLLERR | POLLNVAL)) != 0) {
            return false;
        }
        if (result < 0 && errno != EINTR) {
            return false;
        }
    }
    return false;
}

bool reap_child(pid_t child, int& status, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        const pid_t result = ::waitpid(child, &status, WNOHANG);
        if (result == child) {
            return true;
        }
        if (result < 0 && errno != EINTR) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}

}  // namespace

int main() {
    const std::filesystem::path directory =
        std::filesystem::temp_directory_path() /
        ("uhf-dhcp-client-smoke-" +
         std::to_string(static_cast<long long>(::getpid())));
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
    std::filesystem::create_directories(directory);
    const std::filesystem::path fake_dhclient = directory / "fake-dhclient";
    const std::filesystem::path hook = directory / "hook";
    write_executable(
        fake_dhclient,
        "#!/bin/sh\n"
        "printf '%s\\n' version=1 >\"$UHF_DHCP_RESULT_FILE\"\n"
        "printf '%s\\n' interface=$UHF_DHCP_INTERFACE >>\"$UHF_DHCP_RESULT_FILE\"\n"
        "printf '%s\\n' address=192.168.3.240 >>\"$UHF_DHCP_RESULT_FILE\"\n"
        "printf '%s\\n' mask=255.255.255.0 >>\"$UHF_DHCP_RESULT_FILE\"\n"
        "printf '%s\\n' gateway=192.168.3.1 >>\"$UHF_DHCP_RESULT_FILE\"\n"
        "printf '%s\\n' dns1=192.168.3.1 >>\"$UHF_DHCP_RESULT_FILE\"\n"
        "printf '%s\\n' dns2=- >>\"$UHF_DHCP_RESULT_FILE\"\n");
    write_executable(hook, "#!/bin/sh\nexit 0\n");

    uhf::network::ExecDhcpClient client(directory, fake_dhclient, hook);
    {
        std::ofstream active_result(directory / "eth0.result");
        active_result << "active-result\n";
        std::ofstream active_pid(directory / "eth0.pid");
        active_pid << "active-pid\n";
        std::ofstream active_lease(directory / "eth0.lease");
        active_lease << "active-lease\n";
    }
    uhf::network::DhcpLease lease;
    UHF_TEST_CHECK(client.acquire(dhcp_config(), lease));
    UHF_TEST_CHECK(lease.address == "192.168.3.240");
    UHF_TEST_CHECK(lease.prefix == 24U);
    UHF_TEST_CHECK(lease.gateway == "192.168.3.1");
    UHF_TEST_CHECK(lease.dns_count == 1U && lease.dns[0U] == "192.168.3.1");
    UHF_TEST_CHECK(std::filesystem::exists(directory / "eth0.result"));
    UHF_TEST_CHECK(std::filesystem::exists(directory / "eth0.pid"));
    UHF_TEST_CHECK(std::filesystem::exists(directory / "eth0.lease"));
    UHF_TEST_CHECK(client.release(dhcp_config(), lease));
    UHF_TEST_CHECK(std::filesystem::exists(directory / "eth0.result"));
    UHF_TEST_CHECK(std::filesystem::exists(directory / "eth0.pid"));
    UHF_TEST_CHECK(std::filesystem::exists(directory / "eth0.lease"));
    std::filesystem::remove(directory / "eth0.result");
    std::filesystem::remove(directory / "eth0.pid");
    std::filesystem::remove(directory / "eth0.lease");
    UHF_TEST_CHECK(client.start(dhcp_config(), lease));
    uhf::network::DhcpLease renewed;
    UHF_TEST_CHECK(client.current_lease(dhcp_config(), renewed));
    UHF_TEST_CHECK(renewed.address == lease.address && renewed.prefix == lease.prefix);
    UHF_TEST_CHECK(client.stop(dhcp_config(), lease));
    UHF_TEST_CHECK(client.release(dhcp_config(), lease));

    {
        std::ofstream stale_pid(directory / "eth0.pid");
        stale_pid << ::getpid() << '\n';
        std::ofstream stale_result(directory / "eth0.result");
        stale_result << "stale-result\n";
        UHF_TEST_CHECK(client.stop(dhcp_config(), lease));
        UHF_TEST_CHECK(::kill(::getpid(), 0) == 0);
        UHF_TEST_CHECK(!std::filesystem::exists(directory / "eth0.pid"));
        UHF_TEST_CHECK(!std::filesystem::exists(directory / "eth0.result"));
    }

    const std::filesystem::path matching_pid_path = directory / "eth0.pid";
    int exec_boundary[2] = {-1, -1};
    UHF_TEST_CHECK(::pipe2(exec_boundary, O_CLOEXEC) == 0);
    const pid_t matching_pid = ::fork();
    UHF_TEST_CHECK(matching_pid >= 0);
    if (matching_pid == 0) {
        ::close(exec_boundary[0]);
        const int null_device = ::open("/dev/null", O_RDWR);
        if (null_device >= 0) {
            (void)::dup2(null_device, STDOUT_FILENO);
            (void)::dup2(null_device, STDERR_FILENO);
            if (null_device > STDERR_FILENO) {
                ::close(null_device);
            }
        }
        ::execl(
            "/usr/bin/yes", "yes",
            fake_dhclient.c_str(), matching_pid_path.c_str(), "eth0", nullptr);
        _exit(127);
    }
    ::close(exec_boundary[1]);
    const bool exec_ready = wait_for_exec_boundary(
        exec_boundary[0], std::chrono::seconds(2));
    ::close(exec_boundary[0]);
    int early_status = 0;
    const pid_t early_result = ::waitpid(matching_pid, &early_status, WNOHANG);
    if (!exec_ready || early_result != 0) {
        if (early_result == 0) {
            (void)::kill(matching_pid, SIGKILL);
            while (::waitpid(matching_pid, &early_status, 0) < 0 && errno == EINTR) {
            }
        }
        UHF_TEST_CHECK(exec_ready && early_result == 0);
    }
    {
        std::ofstream matching_pid_file(matching_pid_path);
        matching_pid_file << matching_pid << '\n';
    }
    UHF_TEST_CHECK(client.stop(dhcp_config(), lease));
    int matching_status = 0;
    const bool matching_reaped = reap_child(
        matching_pid, matching_status, std::chrono::seconds(2));
    if (!matching_reaped) {
        (void)::kill(matching_pid, SIGKILL);
        while (::waitpid(matching_pid, &matching_status, 0) < 0 && errno == EINTR) {
        }
    }
    UHF_TEST_CHECK(matching_reaped);
    UHF_TEST_CHECK(WIFSIGNALED(matching_status) || WIFEXITED(matching_status));

    write_executable(
        fake_dhclient,
        "#!/bin/sh\n"
        "printf '%s\\n' version=1 interface=eth0 >\"$UHF_DHCP_RESULT_FILE\"\n");
    UHF_TEST_CHECK(!client.acquire(dhcp_config(), lease));

    uhf::network::InterfaceConfig invalid = dhcp_config();
    invalid.name = "eth9";
    UHF_TEST_CHECK(!client.acquire(invalid, lease));

    std::filesystem::remove_all(directory, ignored);
    return 0;
}
