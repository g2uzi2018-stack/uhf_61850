// SPDX-License-Identifier: GPL-3.0-only
#include "platform/network/linux_backend.hpp"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

class FakeRunner final : public uhf::network::CommandRunner {
public:
    bool run(const std::vector<std::string>& arguments) override {
        commands.push_back(arguments);
        return run_ok;
    }

    bool run_allow_missing(const std::vector<std::string>& arguments) override {
        commands.push_back(arguments);
        return run_ok || allow_missing;
    }

    bool run_ok{true};
    bool allow_missing{false};
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
        if (!current_lease_ok) {
            return false;
        }
        lease = current_lease_value;
        return true;
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
    bool current_lease_ok{false};
    uhf::network::DhcpLease current_lease_value{
        "192.168.3.241", 24U, "192.168.3.3", {"192.168.3.1", ""}, 1U};
    int acquire_count{0};
    int start_count{0};
    int stop_count{0};
    int release_count{0};
};

std::filesystem::path config_path() {
    return std::filesystem::temp_directory_path() / "uhf-linux-backend-smoke" / "network.json";
}

std::string read_text(const std::filesystem::path& path) {
    std::ifstream input(path);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
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
    runner.commands.clear();
    assert(backend.start_runtime());
    assert(runner.commands.size() == 3U);
    assert(runner.commands[0U][1U] == "address");
    assert(runner.commands[0U][2U] == "replace");
    assert(runner.commands[1U][1U] == "route");
    assert(runner.commands[1U][2U] == "replace");
    assert(runner.commands[2U][1U] == "address");
    assert(runner.commands[2U][2U] == "replace");

    candidate.eth0.address = "192.168.3.232";
    candidate.eth0.gateway = "192.168.3.3";
    const std::size_t before_rollback = runner.commands.size();
    assert(backend.apply_stage(loaded, candidate));
    assert(backend.confirm(loaded, candidate));
    assert(backend.rollback(loaded, candidate));
    assert(runner.commands.size() == before_rollback + 6U);
    assert(runner.commands.back()[2U] == "del");
    assert(backend.read_current(loaded));
    assert(loaded.eth0.address == "192.168.3.231");

    const std::filesystem::path vendor_root =
        std::filesystem::temp_directory_path() / "uhf-linux-backend-vendor";
    const std::filesystem::path vendor_network =
        vendor_root / "etc" / "uhf-gateway" / "network.json";
    const uhf::network::VendorNetworkPaths vendor_paths{
        vendor_root / "etc" / "net.conf", vendor_root / "etc" / "net2.conf"};
    std::filesystem::remove_all(vendor_root, ignored);
    std::filesystem::create_directories(vendor_paths.eth0_config.parent_path());
    uhf::network::LinuxNetworkBackend vendor_backend(
        vendor_network, runner, nullptr, vendor_paths);
    uhf::network::NetworkConfig vendor_current;
    assert(vendor_backend.read_current(vendor_current));
    assert(read_text(vendor_paths.eth0_config).find(
               "METHOD=STATIC\nIPADDR=192.168.3.230\nNETMASK=255.255.255.0\nGATEWAY=192.168.3.1\n") == 0U);
    assert(read_text(vendor_paths.eth1_config).find(
               "METHOD=STATIC\nIPADDR=192.168.0.230\nNETMASK=255.255.255.0\nGATEWAY=\n") == 0U);
    std::filesystem::remove(vendor_paths.eth1_config);
    assert(vendor_backend.read_current(vendor_current));
    assert(std::filesystem::is_regular_file(vendor_paths.eth1_config));
    uhf::network::NetworkConfig vendor_candidate = vendor_current;
    vendor_candidate.eth0.address = "192.168.3.232";
    vendor_candidate.eth0.gateway = "192.168.3.2";
    assert(vendor_backend.apply_stage(vendor_current, vendor_candidate));
    assert(vendor_backend.confirm(vendor_current, vendor_candidate));
    assert(read_text(vendor_paths.eth0_config).find(
               "METHOD=STATIC\nIPADDR=192.168.3.232\nNETMASK=255.255.255.0\nGATEWAY=192.168.3.2\n") == 0U);
    std::filesystem::remove(vendor_network);
    {
        std::ofstream eth0(vendor_paths.eth0_config);
        eth0 << "METHOD=STATIC\nIPADDR=192.168.3.233\nNETMASK=255.255.255.0\nGATEWAY=192.168.3.3\n";
    }
    {
        std::ofstream eth1(vendor_paths.eth1_config);
        eth1 << "METHOD=STATIC\nIPADDR=192.168.0.231\nNETMASK=255.255.255.0\n";
    }
    uhf::network::LinuxNetworkBackend imported_backend(
        vendor_network, runner, nullptr, vendor_paths);
    uhf::network::NetworkConfig imported;
    assert(imported_backend.read_current(imported));
    assert(imported.eth0.address == "192.168.3.233");
    assert(imported.eth1.address == "192.168.0.231");

    candidate = loaded;
    candidate.eth0.mode = uhf::network::Mode::dhcp;
    candidate.eth0.address.clear();
    candidate.eth0.gateway.clear();
    assert(!backend.apply_stage(loaded, candidate));

    const std::filesystem::path dhcp_path =
        std::filesystem::temp_directory_path() / "uhf-linux-backend-dhcp" / "network.json";
    std::filesystem::remove_all(dhcp_path.parent_path(), ignored);
    const uhf::network::VendorNetworkPaths dhcp_vendor_paths{
        dhcp_path.parent_path() / "net.conf", dhcp_path.parent_path() / "net2.conf"};
    FakeDhcpClient dhcp;
    runner.run_ok = true;
    runner.commands.clear();
    uhf::network::LinuxNetworkBackend dhcp_backend(
        dhcp_path, runner, &dhcp, dhcp_vendor_paths);
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
    assert(read_text(dhcp_vendor_paths.eth0_config).find("METHOD=DHCP\n") == 0U);
    uhf::network::NetworkConfig dhcp_loaded;
    assert(dhcp_backend.read_current(dhcp_loaded));
    assert(dhcp_loaded.eth0.mode == uhf::network::Mode::dhcp);
    assert(dhcp_backend.start_runtime());
    assert(dhcp.start_count == 1);
    runner.commands.clear();
    dhcp.current_lease_ok = true;
    assert(dhcp_backend.refresh_runtime());
    assert(runner.commands.size() == 4U);
    assert(runner.commands[0U][1U] == "address");
    assert(runner.commands[0U][2U] == "add");
    assert(runner.commands[0U][3U] == "192.168.3.241/24");
    assert(runner.commands[1U][1U] == "route");
    assert(runner.commands[1U][2U] == "add");
    assert(runner.commands[2U][1U] == "route");
    assert(runner.commands[2U][2U] == "del");
    assert(runner.commands[3U][1U] == "address");
    assert(runner.commands[3U][2U] == "del");
    const std::size_t after_refresh = runner.commands.size();
    assert(dhcp_backend.refresh_runtime());
    assert(runner.commands.size() == after_refresh);
    dhcp.current_lease_ok = false;
    assert(dhcp_backend.refresh_runtime());
    assert(runner.commands.size() == after_refresh);

    const uhf::network::NetworkConfig dhcp_before_static = dhcp_loaded;
    uhf::network::NetworkConfig static_candidate = dhcp_loaded;
    static_candidate.eth0.mode = uhf::network::Mode::static_address;
    static_candidate.eth0.address = "192.168.3.231";
    static_candidate.eth0.gateway = "192.168.3.2";
    static_candidate.eth0.dns_count = 0U;
    assert(dhcp_backend.apply_stage(dhcp_loaded, static_candidate));
    assert(dhcp_backend.confirm(dhcp_loaded, static_candidate));
    assert(dhcp.stop_count == 1);
    assert(dhcp_backend.read_current(dhcp_loaded));
    assert(dhcp_loaded.eth0.mode == uhf::network::Mode::static_address);
    assert(dhcp_backend.rollback(dhcp_before_static, static_candidate));
    assert(dhcp_backend.read_current(dhcp_loaded));
    assert(dhcp_loaded.eth0.mode == uhf::network::Mode::dhcp);
    assert(dhcp_backend.apply_stage(dhcp_loaded, static_candidate));
    assert(dhcp_backend.confirm(dhcp_loaded, static_candidate));
    assert(dhcp.stop_count == 2);
    assert(dhcp_backend.read_current(dhcp_loaded));
    assert(dhcp_loaded.eth0.mode == uhf::network::Mode::static_address);

    dhcp_candidate = dhcp_loaded;
    dhcp_candidate.eth0.mode = uhf::network::Mode::dhcp;
    dhcp_candidate.eth0.address.clear();
    dhcp_candidate.eth0.gateway.clear();
    assert(dhcp_backend.apply_stage(dhcp_loaded, dhcp_candidate));
    assert(dhcp_backend.confirm(dhcp_loaded, dhcp_candidate));
    assert(dhcp_backend.rollback(dhcp_loaded, dhcp_candidate));
    assert(dhcp.release_count == 1);
    assert(dhcp_backend.read_current(dhcp_loaded));
    assert(dhcp_loaded.eth0.mode == uhf::network::Mode::static_address);

    const std::filesystem::path idempotent_path =
        std::filesystem::temp_directory_path() / "uhf-linux-backend-idempotent" / "network.json";
    std::filesystem::remove_all(idempotent_path.parent_path(), ignored);
    FakeRunner missing_runner;
    missing_runner.run_ok = false;
    missing_runner.allow_missing = true;
    uhf::network::LinuxNetworkBackend idempotent_backend(idempotent_path, missing_runner);
    uhf::network::NetworkConfig idempotent_previous;
    assert(idempotent_backend.read_current(idempotent_previous));
    uhf::network::NetworkConfig idempotent_candidate = idempotent_previous;
    idempotent_candidate.eth0.address = "192.168.3.235";
    idempotent_candidate.eth0.gateway = "192.168.3.4";
    assert(idempotent_backend.rollback(idempotent_previous, idempotent_candidate));
    assert(missing_runner.commands.size() == 2U);
    assert(missing_runner.commands[0U][2U] == "del");
    assert(missing_runner.commands[1U][2U] == "del");

    dhcp.acquire_ok = false;
    assert(!dhcp_backend.apply_stage(dhcp_loaded, dhcp_candidate));

    runner.run_ok = false;
    candidate = loaded;
    candidate.eth0.address = "192.168.3.233";
    assert(!backend.apply_stage(loaded, candidate));

    const std::filesystem::path corrupt_path =
        std::filesystem::temp_directory_path() / "uhf-linux-backend-corrupt" / "network.json";
    std::filesystem::remove_all(corrupt_path.parent_path(), ignored);
    std::filesystem::create_directories(corrupt_path.parent_path());
    {
        std::ofstream output(corrupt_path);
        output << "{not-valid-network-config";
    }
    uhf::network::LinuxNetworkBackend corrupt_backend(corrupt_path, runner);
    uhf::network::NetworkConfig ignored_config;
    assert(!corrupt_backend.read_current(ignored_config));
    std::ifstream preserved(corrupt_path);
    const std::string preserved_contents{
        std::istreambuf_iterator<char>(preserved), std::istreambuf_iterator<char>()};
    assert(preserved_contents == "{not-valid-network-config");

    std::filesystem::remove_all(path.parent_path(), ignored);
    std::filesystem::remove_all(vendor_root, ignored);
    std::filesystem::remove_all(dhcp_path.parent_path(), ignored);
    std::filesystem::remove_all(idempotent_path.parent_path(), ignored);
    std::filesystem::remove_all(corrupt_path.parent_path(), ignored);
    return 0;
}
