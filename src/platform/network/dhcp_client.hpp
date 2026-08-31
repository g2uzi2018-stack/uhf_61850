// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "platform/network/network_config.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace uhf::network {

struct DhcpLease {
    std::string address;
    std::uint8_t prefix{0U};
    std::string gateway;
    std::array<std::string, kMaxDnsServers> dns{};
    std::uint8_t dns_count{0U};
};

bool validate_lease(const DhcpLease& lease) noexcept;

class DhcpClient {
public:
    virtual ~DhcpClient() = default;

    virtual bool acquire(const InterfaceConfig& config, DhcpLease& lease) = 0;
    virtual bool release(const InterfaceConfig& config, const DhcpLease& lease) noexcept = 0;
};

class ExecDhcpClient final : public DhcpClient {
public:
    explicit ExecDhcpClient(
        std::filesystem::path state_directory,
        std::filesystem::path dhclient_path = "/usr/sbin/dhclient",
        std::filesystem::path hook_path = "/usr/lib/uhf-gateway/dhclient-hook");

    bool acquire(const InterfaceConfig& config, DhcpLease& lease) override;
    bool release(const InterfaceConfig& config, const DhcpLease& lease) noexcept override;

private:
    bool read_result(const InterfaceConfig& config, DhcpLease& lease) const;
    std::filesystem::path result_path(const InterfaceConfig& config) const;
    std::filesystem::path pid_path(const InterfaceConfig& config) const;
    std::filesystem::path lease_path(const InterfaceConfig& config) const;

    std::filesystem::path state_directory_;
    std::filesystem::path dhclient_path_;
    std::filesystem::path hook_path_;
};

enum class LeaseLoadStatus {
    none,
    valid,
    corrupt,
    io_error,
};

struct LeaseLoadResult {
    LeaseLoadStatus status{LeaseLoadStatus::none};
    std::array<std::optional<DhcpLease>, 2U> leases{};
};

class LeaseStore final {
public:
    explicit LeaseStore(std::filesystem::path file);

    LeaseLoadResult load() const;
    bool save(const std::array<std::optional<DhcpLease>, 2U>& leases) const noexcept;
    bool clear() const noexcept;

private:
    std::filesystem::path file_;
};

}  // namespace uhf::network
