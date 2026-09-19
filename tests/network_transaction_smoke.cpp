// SPDX-License-Identifier: GPL-3.0-only
#include "platform/network/network_transaction.hpp"
#include "test_check.hpp"

#include <filesystem>
#include <fstream>
#include <string>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

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
        if (store != nullptr) {
            const uhf::network::LoadResult loaded = store->load();
            confirming_seen = loaded.transaction.has_value() &&
                loaded.transaction->state == uhf::network::TransactionState::confirming;
        }
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
    uhf::network::TransactionStore* store{nullptr};
    bool confirming_seen{false};
};

std::filesystem::path temporary_path() {
    return std::filesystem::temp_directory_path() /
        ("uhf-network-transaction-smoke-" +
         std::to_string(static_cast<long long>(::getpid()))) /
        "transaction.json";
}

}  // namespace

int main() {
    const std::filesystem::path path = temporary_path();
    std::error_code ignored;
    std::filesystem::remove_all(path.parent_path(), ignored);
    std::filesystem::remove(path.string() + ".tmp", ignored);
    uhf::network::TransactionStore store(path);
    FakeBackend backend;
    backend.store = &store;
    TestClock clock;
    uhf::network::TransactionManager manager(store, backend, clock, 60U);

    uhf::network::NetworkConfig candidate = backend.current;
    candidate.eth0.address = "192.168.3.231";
    UHF_TEST_CHECK(manager.stage(candidate) == uhf::network::TransactionResult::ok);
    UHF_TEST_CHECK(backend.stage_count == 1);
    UHF_TEST_CHECK(store.load().status == uhf::network::LoadStatus::valid);
    UHF_TEST_CHECK(manager.rollback_if_needed() == uhf::network::TransactionResult::not_due);
    UHF_TEST_CHECK(manager.stage(candidate) == uhf::network::TransactionResult::busy);

    clock.now = 161U;
    UHF_TEST_CHECK(manager.rollback_if_needed() == uhf::network::TransactionResult::expired);
    UHF_TEST_CHECK(backend.rollback_count == 1);
    UHF_TEST_CHECK(store.load().status == uhf::network::LoadStatus::none);
    UHF_TEST_CHECK(backend.current.eth0.address == "192.168.3.230");

    candidate.eth0.address = "192.168.3.232";
    clock.now = 200U;
    UHF_TEST_CHECK(manager.stage(candidate) == uhf::network::TransactionResult::ok);
    clock.now = 201U;
    UHF_TEST_CHECK(manager.confirm() == uhf::network::TransactionResult::ok);
    UHF_TEST_CHECK(backend.confirm_count == 1);
    UHF_TEST_CHECK(backend.confirming_seen);
    UHF_TEST_CHECK(store.load().status == uhf::network::LoadStatus::none);
    UHF_TEST_CHECK(backend.current.eth0.address == "192.168.3.232");

    candidate.eth0.address = "192.168.3.233";
    clock.now = 300U;
    UHF_TEST_CHECK(manager.stage(candidate) == uhf::network::TransactionResult::ok);

    const int lock_descriptor = ::open(
        (path.string() + ".lock").c_str(), O_RDWR | O_CLOEXEC);
    UHF_TEST_CHECK(lock_descriptor >= 0);
    UHF_TEST_CHECK(::flock(lock_descriptor, LOCK_EX | LOCK_NB) == 0);
    UHF_TEST_CHECK(manager.confirm() == uhf::network::TransactionResult::busy);
    UHF_TEST_CHECK(manager.rollback_if_needed() == uhf::network::TransactionResult::busy);
    UHF_TEST_CHECK(::flock(lock_descriptor, LOCK_UN) == 0);
    UHF_TEST_CHECK(::close(lock_descriptor) == 0);
    UHF_TEST_CHECK(manager.rollback_now() == uhf::network::TransactionResult::ok);
    UHF_TEST_CHECK(backend.current.eth0.address == "192.168.3.232");

    candidate.eth0.address = "192.168.3.234";
    clock.now = 400U;
    UHF_TEST_CHECK(manager.stage(candidate) == uhf::network::TransactionResult::ok);
    uhf::network::Transaction transaction = *store.load().transaction;
    transaction.state = uhf::network::TransactionState::confirming;
    UHF_TEST_CHECK(store.save(transaction));
    backend.current = candidate;
    UHF_TEST_CHECK(manager.rollback_if_needed() == uhf::network::TransactionResult::ok);
    UHF_TEST_CHECK(store.load().status == uhf::network::LoadStatus::none);
    UHF_TEST_CHECK(backend.current.eth0.address == "192.168.3.232");

    candidate.eth0.address = "192.168.3.235";
    clock.now = 500U;
    UHF_TEST_CHECK(manager.stage(candidate) == uhf::network::TransactionResult::ok);
    clock.id = "boot-b";
    UHF_TEST_CHECK(manager.rollback_if_needed() == uhf::network::TransactionResult::boot_changed);
    UHF_TEST_CHECK(backend.current.eth0.address == "192.168.3.232");

    clock.id = "boot-a";
    candidate.eth0.address = "192.168.3.236";
    backend.stage_ok = false;
    clock.now = 600U;
    UHF_TEST_CHECK(manager.stage(candidate) == uhf::network::TransactionResult::backend_error);
    UHF_TEST_CHECK(store.load().status == uhf::network::LoadStatus::none);
    backend.stage_ok = true;

    std::ofstream corrupt(path);
    corrupt << "{\"version\":1,\"state\":\"staged\"}";
    corrupt.close();
    UHF_TEST_CHECK(manager.rollback_if_needed() == uhf::network::TransactionResult::corrupt);
    std::filesystem::remove_all(path.parent_path(), ignored);
    return 0;
}
