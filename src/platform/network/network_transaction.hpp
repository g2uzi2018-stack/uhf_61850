// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "platform/network/network_config.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace uhf::network {

class Clock {
public:
    virtual ~Clock() = default;

    virtual std::uint64_t boottime_ns() const noexcept = 0;
    virtual std::string boot_id() const = 0;
};

class SystemClock final : public Clock {
public:
    std::uint64_t boottime_ns() const noexcept override;
    std::string boot_id() const override;
};

enum class TransactionState {
    staged,
    confirmed,
    rolled_back,
};

struct Transaction {
    std::uint64_t id{0U};
    std::string boot_id;
    std::uint64_t staged_boottime_ns{0U};
    std::uint64_t deadline_boottime_ns{0U};
    TransactionState state{TransactionState::staged};
    NetworkConfig previous;
    NetworkConfig candidate;
};

enum class LoadStatus {
    none,
    valid,
    corrupt,
    io_error,
};

struct LoadResult {
    LoadStatus status{LoadStatus::none};
    std::optional<Transaction> transaction;
};

class TransactionStore {
public:
    explicit TransactionStore(std::filesystem::path file);

    LoadResult load() const;
    bool save(const Transaction& transaction) const noexcept;
    bool clear() const noexcept;
    const std::filesystem::path& path() const noexcept;

private:
    std::filesystem::path file_;
};

class Backend {
public:
    virtual ~Backend() = default;

    virtual bool read_current(NetworkConfig& config) = 0;
    virtual bool apply_stage(
        const NetworkConfig& previous, const NetworkConfig& candidate) = 0;
    virtual bool confirm(
        const NetworkConfig& previous, const NetworkConfig& candidate) = 0;
    virtual bool rollback(
        const NetworkConfig& previous, const NetworkConfig& candidate) = 0;
};

enum class TransactionResult {
    ok,
    not_due,
    no_transaction,
    invalid_candidate,
    busy,
    expired,
    boot_changed,
    corrupt,
    backend_error,
    storage_error,
};

class TransactionManager {
public:
    TransactionManager(
        TransactionStore& store,
        Backend& backend,
        const Clock& clock,
        std::uint64_t rollback_window_ns = 60ULL * 1000ULL * 1000ULL * 1000ULL);

    TransactionResult stage(const NetworkConfig& candidate);
    TransactionResult confirm();
    TransactionResult rollback_now();
    TransactionResult rollback_if_needed();
    LoadResult inspect() const;
    const std::string& last_error() const noexcept;

private:
    TransactionResult rollback_loaded(
        const Transaction& transaction, TransactionResult reason);
    TransactionResult set_error(TransactionResult result, std::string message);

    TransactionStore& store_;
    Backend& backend_;
    const Clock& clock_;
    std::uint64_t rollback_window_ns_;
    std::uint64_t next_id_{1U};
    std::string last_error_;
};

std::string transaction_state_name(TransactionState state) noexcept;
std::string transaction_result_name(TransactionResult result) noexcept;

}  // namespace uhf::network
