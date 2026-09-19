// SPDX-License-Identifier: GPL-3.0-only
#include "platform/privileged/network_service.hpp"
#include "test_check.hpp"

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

    bool configure_sntp(std::string_view server) override {
        ++sntp_count;
        sntp_server = server;
        return sntp_ok;
    }

    bool disable_sntp() override {
        ++sntp_disable_count;
        return sntp_disable_ok;
    }

    bool set_system_time(std::string_view local_time) override {
        ++time_set_count;
        system_time = local_time;
        return time_set_ok;
    }

    bool restart_ok{true};
    bool reboot_ok{true};
    bool sntp_ok{true};
    bool sntp_disable_ok{true};
    bool time_set_ok{true};
    int restart_count{0};
    int reboot_count{0};
    int sntp_count{0};
    int sntp_disable_count{0};
    int time_set_count{0};
    std::string sntp_server;
    std::string system_time;
};

}  // namespace

int main() {
    const std::filesystem::path directory =
        std::filesystem::temp_directory_path() /
        ("uhf-network-service-smoke-" +
         std::to_string(static_cast<long long>(::getpid())));
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
    FakeMaintenanceRunner maintenance;
    uhf::privileged::NetworkService service(
        directory / "etc" / "network.json",
        directory / "var" / "transaction.json",
        &maintenance);

    const uhf::privileged::Reply restart =
        service.handle("maintenance.restart-service", ::getuid(), ::getgid());
    UHF_TEST_CHECK(restart.ok && restart.code == "ok");
    UHF_TEST_CHECK(maintenance.restart_count == 1);
    const uhf::privileged::Reply reboot =
        service.handle("maintenance.reboot", ::getuid(), ::getgid());
    UHF_TEST_CHECK(reboot.ok && reboot.code == "ok");
    UHF_TEST_CHECK(maintenance.reboot_count == 1);
    maintenance.restart_ok = false;
    const uhf::privileged::Reply failed =
        service.handle("maintenance.restart-service", ::getuid(), ::getgid());
    UHF_TEST_CHECK(!failed.ok && failed.code == "maintenance_error");
    const uhf::privileged::Reply time_sync =
        service.handle("time.sync\npool.ntp.org", ::getuid(), ::getgid());
    UHF_TEST_CHECK(time_sync.ok && maintenance.sntp_count == 1);
    UHF_TEST_CHECK(maintenance.sntp_server == "pool.ntp.org");
    const uhf::privileged::Reply invalid_time_sync =
        service.handle("time.sync\ninvalid server", ::getuid(), ::getgid());
    UHF_TEST_CHECK(!invalid_time_sync.ok && invalid_time_sync.code == "invalid_time_request");
    UHF_TEST_CHECK(maintenance.sntp_count == 1);
    const uhf::privileged::Reply time_disable =
        service.handle("time.disable", ::getuid(), ::getgid());
    UHF_TEST_CHECK(time_disable.ok && maintenance.sntp_disable_count == 1);
    const uhf::privileged::Reply time_set =
        service.handle("time.set\n2026-09-02T12:34:56", ::getuid(), ::getgid());
    UHF_TEST_CHECK(time_set.ok && maintenance.time_set_count == 1);
    UHF_TEST_CHECK(maintenance.system_time == "2026-09-02T12:34:56");
    const uhf::privileged::Reply invalid_time_set =
        service.handle("time.set\n2026-02-30T12:34", ::getuid(), ::getgid());
    UHF_TEST_CHECK(!invalid_time_set.ok && invalid_time_set.code == "invalid_time_request");
    UHF_TEST_CHECK(maintenance.time_set_count == 1);
    const uhf::privileged::Reply unknown =
        service.handle("maintenance.run arbitrary-command", ::getuid(), ::getgid());
    UHF_TEST_CHECK(!unknown.ok && unknown.code == "unknown_operation");
    std::filesystem::remove_all(directory, ignored);
    return 0;
}
