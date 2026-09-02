// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <string_view>

namespace uhf::privileged {

bool valid_sntp_server(std::string_view server) noexcept;
bool valid_system_time(std::string_view local_time) noexcept;

class MaintenanceRunner {
public:
    virtual ~MaintenanceRunner() = default;
    virtual bool restart_service() = 0;
    virtual bool reboot() = 0;
    virtual bool configure_sntp(std::string_view server) = 0;
    virtual bool disable_sntp() = 0;
    virtual bool set_system_time(std::string_view local_time) = 0;
};

class ExecMaintenanceRunner final : public MaintenanceRunner {
public:
    bool restart_service() override;
    bool reboot() override;
    bool configure_sntp(std::string_view server) override;
    bool disable_sntp() override;
    bool set_system_time(std::string_view local_time) override;
};

}  // namespace uhf::privileged
