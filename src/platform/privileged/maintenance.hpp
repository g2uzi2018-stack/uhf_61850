// SPDX-License-Identifier: GPL-3.0-only
#pragma once

namespace uhf::privileged {

class MaintenanceRunner {
public:
    virtual ~MaintenanceRunner() = default;
    virtual bool restart_service() = 0;
    virtual bool reboot() = 0;
};

class ExecMaintenanceRunner final : public MaintenanceRunner {
public:
    bool restart_service() override;
    bool reboot() override;
};

}  // namespace uhf::privileged
