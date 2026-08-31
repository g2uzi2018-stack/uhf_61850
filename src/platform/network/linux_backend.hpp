// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "platform/network/network_transaction.hpp"

#include <filesystem>
#include <cstddef>
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
    LinuxNetworkBackend(std::filesystem::path persistent_file, CommandRunner& command_runner);

    bool read_current(NetworkConfig& config) override;
    bool apply_stage(const NetworkConfig& previous, const NetworkConfig& candidate) override;
    bool confirm(const NetworkConfig& previous, const NetworkConfig& candidate) override;
    bool rollback(const NetworkConfig& previous, const NetworkConfig& candidate) override;

private:
    bool load(NetworkConfig& config) const;
    bool save(const NetworkConfig& config) const noexcept;
    bool apply_address_additions(
        const NetworkConfig& previous, const NetworkConfig& candidate);
    bool remove_candidate_state(const NetworkConfig& previous, const NetworkConfig& candidate);
    bool remove_previous_state(const NetworkConfig& previous, const NetworkConfig& candidate);
    bool restore_previous_state(const NetworkConfig& previous, const NetworkConfig& candidate);
    std::filesystem::path persistent_file_;
    CommandRunner& command_runner_;
    bool staged_{false};
    NetworkConfig staged_previous_;
    NetworkConfig staged_candidate_;
};

}  // namespace uhf::network
