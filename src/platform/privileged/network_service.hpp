// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "platform/network/linux_backend.hpp"
#include "platform/network/linux_status.hpp"
#include "platform/network/network_transaction.hpp"
#include "platform/privileged/unix_socket.hpp"

#include <atomic>
#include <filesystem>
#include <mutex>
#include <string_view>
#include <thread>

namespace uhf::privileged {

class NetworkService {
public:
    NetworkService(std::filesystem::path network_file, std::filesystem::path transaction_file);
    ~NetworkService();

    NetworkService(const NetworkService&) = delete;
    NetworkService& operator=(const NetworkService&) = delete;

    Reply handle(std::string_view request, uid_t uid, gid_t gid);
    network::TransactionResult recover_pending();
    void start_rollback_monitor();
    void stop_rollback_monitor() noexcept;

private:
    void rollback_loop();

    network::ExecCommandRunner command_runner_;
    network::LinuxNetworkBackend backend_;
    network::LinuxStatusReader status_reader_;
    network::TransactionStore transaction_store_;
    network::SystemClock clock_;
    network::TransactionManager transaction_manager_;
    std::mutex mutex_;
    std::atomic<bool> stop_requested_{false};
    std::thread rollback_worker_;
};

}  // namespace uhf::privileged
