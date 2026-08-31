// SPDX-License-Identifier: GPL-3.0-only
#include "platform/network/dhcp_client.hpp"

#include <cassert>
#include <chrono>
#include <filesystem>
#include <fcntl.h>
#include <fstream>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

namespace {

void write_executable(const std::filesystem::path& path, const char* contents) {
    std::ofstream output(path);
    assert(output);
    output << contents;
    output.close();
    assert(::chmod(path.c_str(), S_IRWXU) == 0);
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

}  // namespace

int main() {
    const std::filesystem::path directory =
        std::filesystem::temp_directory_path() / "uhf-dhcp-client-smoke";
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
    assert(client.acquire(dhcp_config(), lease));
    assert(lease.address == "192.168.3.240");
    assert(lease.prefix == 24U);
    assert(lease.gateway == "192.168.3.1");
    assert(lease.dns_count == 1U && lease.dns[0U] == "192.168.3.1");
    assert(std::filesystem::exists(directory / "eth0.result"));
    assert(std::filesystem::exists(directory / "eth0.pid"));
    assert(std::filesystem::exists(directory / "eth0.lease"));
    assert(client.release(dhcp_config(), lease));
    assert(std::filesystem::exists(directory / "eth0.result"));
    assert(std::filesystem::exists(directory / "eth0.pid"));
    assert(std::filesystem::exists(directory / "eth0.lease"));
    std::filesystem::remove(directory / "eth0.result");
    std::filesystem::remove(directory / "eth0.pid");
    std::filesystem::remove(directory / "eth0.lease");
    assert(client.start(dhcp_config(), lease));
    uhf::network::DhcpLease renewed;
    assert(client.current_lease(dhcp_config(), renewed));
    assert(renewed.address == lease.address && renewed.prefix == lease.prefix);
    assert(client.stop(dhcp_config(), lease));
    assert(client.release(dhcp_config(), lease));

    {
        std::ofstream stale_pid(directory / "eth0.pid");
        stale_pid << ::getpid() << '\n';
        std::ofstream stale_result(directory / "eth0.result");
        stale_result << "stale-result\n";
        assert(client.stop(dhcp_config(), lease));
        assert(::kill(::getpid(), 0) == 0);
        assert(!std::filesystem::exists(directory / "eth0.pid"));
        assert(!std::filesystem::exists(directory / "eth0.result"));
    }

    const std::filesystem::path matching_pid_path = directory / "eth0.pid";
    const pid_t matching_pid = ::fork();
    assert(matching_pid >= 0);
    if (matching_pid == 0) {
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
    {
        std::ofstream matching_pid_file(matching_pid_path);
        matching_pid_file << matching_pid << '\n';
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    assert(client.stop(dhcp_config(), lease));
    int matching_status = 0;
    assert(::waitpid(matching_pid, &matching_status, 0) == matching_pid);
    assert(WIFSIGNALED(matching_status) || WIFEXITED(matching_status));

    write_executable(
        fake_dhclient,
        "#!/bin/sh\n"
        "printf '%s\\n' version=1 interface=eth0 >\"$UHF_DHCP_RESULT_FILE\"\n");
    assert(!client.acquire(dhcp_config(), lease));

    uhf::network::InterfaceConfig invalid = dhcp_config();
    invalid.name = "eth9";
    assert(!client.acquire(invalid, lease));

    std::filesystem::remove_all(directory, ignored);
    return 0;
}
