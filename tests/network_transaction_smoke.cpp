// SPDX-License-Identifier: GPL-3.0-only
#include "platform/network/network_transaction.hpp"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

class TestClock final : public uhf::network::Clock {
public:
    std::uint64_t boottime_ns() const noexcept override { return now; }
    std::string boot_id() const override { return id; }

    std::uint64_t now{100U};
    std::string id{"boot-a"};
};

class FakeBackend final : public uhf::network::Backend {
public:
    bool read_current(uhf::network::NetworkConfig& output) override {
        output = current;
        ++read_count;
        return read_ok;
    }

    bool apply_stage(
        const uhf::network::NetworkConfig&, const uhf::network::NetworkConfig& candidate) override {
        if (!stage_ok) {
            return false;
        }
        current = candidate;
        ++stage_count;
        return true;
    }

    bool confirm(
        const uhf::network::NetworkConfig&, const uhf::network::NetworkConfig&) override {
        ++confirm_count;
        return confirm_ok;
    }

    bool rollback(
        const uhf::network::NetworkConfig& previous,
        const uhf::network::NetworkConfig&) override {
        if (!rollback_ok) {
            return false;
        }
        current = previous;
        ++rollback_count;
        return true;
    }

    uhf::network::NetworkConfig current;
    bool read_ok{true};
    bool stage_ok{true};
    bool confirm_ok{true};
    bool rollback_ok{true};
    int read_count{0};
    int stage_count{0};
    int confirm_count{0};
    int rollback_count{0};
};

std::filesystem::path temporary_path() {
    return std::filesystem::temp_directory_path() /
        "uhf-network-transaction-smoke" / "transaction.json";
}

}  // namespace

int main() {
    const std::filesystem::path path = temporary_path();
    std::error_code ignored;
    std::filesystem::remove_all(path.parent_path(), ignored);
    std::filesystem::remove(path.string() + ".tmp", ignored);
    uhf::network::TransactionStore store(path);
    FakeBackend backend;
    TestClock clock;
    uhf::network::TransactionManager manager(store, backend, clock, 60U);

    uhf::network::NetworkConfig candidate = backend.current;
    candidate.eth0.address = "192.168.3.231";
    assert(manager.stage(candidate) == uhf::network::TransactionResult::ok);
    assert(backend.stage_count == 1);
    assert(store.load().status == uhf::network::LoadStatus::valid);
    assert(manager.rollback_if_needed() == uhf::network::TransactionResult::not_due);
    assert(manager.stage(candidate) == uhf::network::TransactionResult::busy);

    clock.now = 161U;
    assert(manager.rollback_if_needed() == uhf::network::TransactionResult::expired);
    assert(backend.rollback_count == 1);
    assert(store.load().status == uhf::network::LoadStatus::none);
    assert(backend.current.eth0.address == "192.168.3.230");

    candidate.eth0.address = "192.168.3.232";
    clock.now = 200U;
    assert(manager.stage(candidate) == uhf::network::TransactionResult::ok);
    clock.now = 201U;
    assert(manager.confirm() == uhf::network::TransactionResult::ok);
    assert(backend.confirm_count == 1);
    assert(store.load().status == uhf::network::LoadStatus::none);
    assert(backend.current.eth0.address == "192.168.3.232");

    candidate.eth0.address = "192.168.3.233";
    clock.now = 300U;
    assert(manager.stage(candidate) == uhf::network::TransactionResult::ok);
    clock.id = "boot-b";
    assert(manager.rollback_if_needed() == uhf::network::TransactionResult::boot_changed);
    assert(backend.current.eth0.address == "192.168.3.232");

    clock.id = "boot-a";
    candidate.eth0.address = "192.168.3.234";
    backend.stage_ok = false;
    clock.now = 400U;
    assert(manager.stage(candidate) == uhf::network::TransactionResult::backend_error);
    assert(store.load().status == uhf::network::LoadStatus::none);
    backend.stage_ok = true;

    std::ofstream corrupt(path);
    corrupt << "{\"version\":1,\"state\":\"staged\"}";
    corrupt.close();
    assert(manager.rollback_if_needed() == uhf::network::TransactionResult::corrupt);
    std::filesystem::remove_all(path.parent_path(), ignored);
    return 0;
}
