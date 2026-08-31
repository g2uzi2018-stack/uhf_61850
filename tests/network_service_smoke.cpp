// SPDX-License-Identifier: GPL-3.0-only
#include "platform/privileged/network_service.hpp"

#include <cassert>
#include <filesystem>
#include <unistd.h>

namespace {

class FakeMaintenanceRunner final : public uhf::privileged::MaintenanceRunner {
public:
    bool restart_service() override {
        ++restart_count;
        return restart_ok;
    }

    bool reboot() override {
        ++reboot_count;
        return reboot_ok;
    }

    bool restart_ok{true};
    bool reboot_ok{true};
    int restart_count{0};
    int reboot_count{0};
};

}  // namespace

int main() {
    const std::filesystem::path directory =
        std::filesystem::temp_directory_path() / "uhf-network-service-smoke";
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
    FakeMaintenanceRunner maintenance;
    uhf::privileged::NetworkService service(
        directory / "etc" / "network.json",
        directory / "var" / "transaction.json",
        &maintenance);

    const uhf::privileged::Reply restart =
        service.handle("maintenance.restart-service", ::getuid(), ::getgid());
    assert(restart.ok && restart.code == "ok");
    assert(maintenance.restart_count == 1);
    const uhf::privileged::Reply reboot =
        service.handle("maintenance.reboot", ::getuid(), ::getgid());
    assert(reboot.ok && reboot.code == "ok");
    assert(maintenance.reboot_count == 1);
    maintenance.restart_ok = false;
    const uhf::privileged::Reply failed =
        service.handle("maintenance.restart-service", ::getuid(), ::getgid());
    assert(!failed.ok && failed.code == "maintenance_error");
    const uhf::privileged::Reply unknown =
        service.handle("maintenance.run arbitrary-command", ::getuid(), ::getgid());
    assert(!unknown.ok && unknown.code == "unknown_operation");
    std::filesystem::remove_all(directory, ignored);
    return 0;
}
