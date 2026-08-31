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
