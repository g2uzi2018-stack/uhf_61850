// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "platform/network/dhcp_client.hpp"
#include "platform/network/network_transaction.hpp"

#include <array>
#include <filesystem>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace uhf::network {

class CommandRunner {
public:
    virtual ~CommandRunner() = default;

    virtual bool run(const std::vector<std::string>& arguments) = 0;
};

class ExecCommandRunner final : public CommandRunner {
public:
    bool run(const std::vector<std::string>& arguments) override;
};

class LinuxNetworkBackend final : public Backend {
public:
    LinuxNetworkBackend(
        std::filesystem::path persistent_file,
        CommandRunner& command_runner,
        DhcpClient* dhcp_client = nullptr);

    bool read_current(NetworkConfig& config) override;
    bool apply_stage(const NetworkConfig& previous, const NetworkConfig& candidate) override;
    bool confirm(const NetworkConfig& previous, const NetworkConfig& candidate) override;
    bool rollback(const NetworkConfig& previous, const NetworkConfig& candidate) override;

    bool start_runtime();
    bool refresh_runtime();

private:
    bool load(NetworkConfig& config) const;
    bool save(const NetworkConfig& config) const noexcept;
    bool load_leases(
        const LeaseStore& store,
        std::array<std::optional<DhcpLease>, 2U>& leases) const;
    bool apply_address_additions(
        const NetworkConfig& previous,
        const NetworkConfig& candidate,
        const std::array<std::optional<DhcpLease>, 2U>& previous_leases,
        const std::array<std::optional<DhcpLease>, 2U>& candidate_leases);
    bool remove_candidate_state(
        const NetworkConfig& previous,
        const NetworkConfig& candidate,
        const std::array<std::optional<DhcpLease>, 2U>& previous_leases,
        const std::array<std::optional<DhcpLease>, 2U>& candidate_leases);
    bool remove_previous_state(
        const NetworkConfig& previous,
        const NetworkConfig& candidate,
        const std::array<std::optional<DhcpLease>, 2U>& previous_leases,
        const std::array<std::optional<DhcpLease>, 2U>& candidate_leases);
    bool restore_previous_state(
        const NetworkConfig& previous,
        const NetworkConfig& candidate,
        const std::array<std::optional<DhcpLease>, 2U>& previous_leases,
        const std::array<std::optional<DhcpLease>, 2U>& candidate_leases);
    std::filesystem::path persistent_file_;
    CommandRunner& command_runner_;
    DhcpClient* dhcp_client_{nullptr};
    LeaseStore lease_store_;
    LeaseStore staged_lease_store_;
    bool staged_{false};
    NetworkConfig staged_previous_;
    NetworkConfig staged_candidate_;
};

}  // namespace uhf::network
