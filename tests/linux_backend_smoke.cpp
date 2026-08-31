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
    assert(backend.confirm());
    assert(runner.commands.size() == 4U);
    assert(runner.commands[2U][2U] == "del");
    uhf::network::NetworkConfig loaded;
    assert(backend.read_current(loaded));
    assert(loaded.eth0.address == "192.168.3.231");

    candidate.eth0.address = "192.168.3.232";
    candidate.eth0.gateway = "192.168.3.3";
    const std::size_t before_rollback = runner.commands.size();
    assert(backend.apply_stage(loaded, candidate));
    assert(backend.rollback(loaded));
    assert(runner.commands.size() == before_rollback + 4U);
    assert(runner.commands.back()[2U] == "del");

    candidate = loaded;
    candidate.eth0.mode = uhf::network::Mode::dhcp;
    candidate.eth0.address.clear();
    candidate.eth0.gateway.clear();
    assert(!backend.apply_stage(loaded, candidate));

    runner.run_ok = false;
    candidate = loaded;
    candidate.eth0.address = "192.168.3.233";
    assert(!backend.apply_stage(loaded, candidate));
    std::filesystem::remove_all(path.parent_path(), ignored);
    return 0;
}
