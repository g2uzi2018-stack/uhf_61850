// SPDX-License-Identifier: GPL-3.0-only
#include "platform/network/linux_backend.hpp"

#include <cassert>
#include <filesystem>
#include <string>
#include <vector>

namespace {

class FakeRunner final : public uhf::network::CommandRunner {
public:
    bool run(const std::vector<std::string>& arguments) override {
        commands.push_back(arguments);
        return run_ok;
    }

    bool run_ok{true};
    std::vector<std::vector<std::string>> commands;
};

class FakeDhcpClient final : public uhf::network::DhcpClient {
public:
    bool acquire(const uhf::network::InterfaceConfig& config, uhf::network::DhcpLease& lease) override {
        static_cast<void>(config);
        ++acquire_count;
        if (!acquire_ok) {
            return false;
        }
        lease.address = "192.168.3.240";
        lease.prefix = 24U;
        lease.gateway = "192.168.3.2";
        lease.dns[0U] = "192.168.3.1";
        lease.dns_count = 1U;
        return true;
    }

    bool start(
        const uhf::network::InterfaceConfig& config,
        const uhf::network::DhcpLease& lease) override {
        static_cast<void>(config);
        static_cast<void>(lease);
        ++start_count;
        return start_ok;
    }

    bool current_lease(
        const uhf::network::InterfaceConfig& config,
        uhf::network::DhcpLease& lease) const override {
        static_cast<void>(config);
        static_cast<void>(lease);
        return false;
    }

    bool stop(
        const uhf::network::InterfaceConfig& config,
        const uhf::network::DhcpLease& lease) noexcept override {
        static_cast<void>(config);
        static_cast<void>(lease);
        ++stop_count;
        return stop_ok;
    }

    bool release(const uhf::network::InterfaceConfig& config, const uhf::network::DhcpLease& lease) noexcept override {
        static_cast<void>(config);
        static_cast<void>(lease);
        ++release_count;
        return release_ok;
    }

    bool acquire_ok{true};
    bool start_ok{true};
    bool stop_ok{true};
    bool release_ok{true};
    int acquire_count{0};
    int start_count{0};
    int stop_count{0};
    int release_count{0};
};

std::filesystem::path config_path() {
    return std::filesystem::temp_directory_path() / "uhf-linux-backend-smoke" / "network.json";
}

}  // namespace

int main() {
    const std::filesystem::path path = config_path();
    std::error_code ignored;
    std::filesystem::remove_all(path.parent_path(), ignored);
    FakeRunner runner;
    uhf::network::LinuxNetworkBackend backend(path, runner);
    uhf::network::NetworkConfig current;
    assert(backend.read_current(current));

    uhf::network::NetworkConfig candidate = current;
    candidate.eth0.address = "192.168.3.231";
    candidate.eth0.gateway = "192.168.3.2";
    assert(backend.apply_stage(current, candidate));
    assert(runner.commands.size() == 2U);
    assert(runner.commands[0U][0U] == "/usr/sbin/ip");
    assert(runner.commands[0U][1U] == "address");
    assert(runner.commands[0U][2U] == "add");
    assert(runner.commands[0U][3U] == "192.168.3.231/24");
    assert(runner.commands[1U][1U] == "route");
    assert(runner.commands[1U][2U] == "add");
    assert(backend.confirm(current, candidate));
    assert(runner.commands.size() == 4U);
    assert(runner.commands[2U][2U] == "del");
    uhf::network::NetworkConfig loaded;
    assert(backend.read_current(loaded));
    assert(loaded.eth0.address == "192.168.3.231");

    candidate.eth0.address = "192.168.3.232";
    candidate.eth0.gateway = "192.168.3.3";
    const std::size_t before_rollback = runner.commands.size();
    assert(backend.apply_stage(loaded, candidate));
    assert(backend.rollback(loaded, candidate));
    assert(runner.commands.size() == before_rollback + 4U);
    assert(runner.commands.back()[2U] == "del");

    candidate = loaded;
    candidate.eth0.mode = uhf::network::Mode::dhcp;
    candidate.eth0.address.clear();
    candidate.eth0.gateway.clear();
    assert(!backend.apply_stage(loaded, candidate));

    const std::filesystem::path dhcp_path =
        std::filesystem::temp_directory_path() / "uhf-linux-backend-dhcp" / "network.json";
    std::filesystem::remove_all(dhcp_path.parent_path(), ignored);
    FakeDhcpClient dhcp;
    runner.run_ok = true;
    runner.commands.clear();
    uhf::network::LinuxNetworkBackend dhcp_backend(dhcp_path, runner, &dhcp);
    uhf::network::NetworkConfig dhcp_previous;
    assert(dhcp_backend.read_current(dhcp_previous));
    uhf::network::NetworkConfig dhcp_candidate = dhcp_previous;
    dhcp_candidate.eth0.mode = uhf::network::Mode::dhcp;
    dhcp_candidate.eth0.address.clear();
    dhcp_candidate.eth0.gateway.clear();
    dhcp_candidate.eth0.dns_count = 0U;
    assert(dhcp_backend.apply_stage(dhcp_previous, dhcp_candidate));
    assert(dhcp.acquire_count == 1);
    assert(runner.commands.size() == 2U);
    assert(runner.commands[0U][1U] == "address");
    assert(runner.commands[0U][3U] == "192.168.3.240/24");
    assert(runner.commands[1U][1U] == "route");
    assert(dhcp_backend.confirm(dhcp_previous, dhcp_candidate));
    uhf::network::NetworkConfig dhcp_loaded;
    assert(dhcp_backend.read_current(dhcp_loaded));
    assert(dhcp_loaded.eth0.mode == uhf::network::Mode::dhcp);

    uhf::network::NetworkConfig static_candidate = dhcp_loaded;
    static_candidate.eth0.mode = uhf::network::Mode::static_address;
    static_candidate.eth0.address = "192.168.3.231";
    static_candidate.eth0.gateway = "192.168.3.2";
    static_candidate.eth0.dns_count = 0U;
    assert(dhcp_backend.apply_stage(dhcp_loaded, static_candidate));
    assert(dhcp_backend.confirm(dhcp_loaded, static_candidate));
    assert(dhcp_backend.read_current(dhcp_loaded));
    assert(dhcp_loaded.eth0.mode == uhf::network::Mode::static_address);

    dhcp_candidate = dhcp_loaded;
    dhcp_candidate.eth0.mode = uhf::network::Mode::dhcp;
    dhcp_candidate.eth0.address.clear();
    dhcp_candidate.eth0.gateway.clear();
    assert(dhcp_backend.apply_stage(dhcp_loaded, dhcp_candidate));
    assert(dhcp_backend.rollback(dhcp_loaded, dhcp_candidate));
    assert(dhcp.release_count == 1);
    assert(dhcp_backend.read_current(dhcp_loaded));
    assert(dhcp_loaded.eth0.mode == uhf::network::Mode::static_address);

    dhcp.acquire_ok = false;
    assert(!dhcp_backend.apply_stage(dhcp_loaded, dhcp_candidate));

    runner.run_ok = false;
    candidate = loaded;
    candidate.eth0.address = "192.168.3.233";
    assert(!backend.apply_stage(loaded, candidate));
    std::filesystem::remove_all(path.parent_path(), ignored);
    std::filesystem::remove_all(dhcp_path.parent_path(), ignored);
    return 0;
}
