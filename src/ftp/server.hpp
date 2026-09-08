// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <vector>

namespace uhf::ftp {

struct ServerOptions {
    std::string bind_address{"0.0.0.0"};
    std::uint16_t port{21U};
    std::filesystem::path root;
    std::size_t max_connections{4U};
    std::size_t max_files{64U};
    std::uint64_t max_file_bytes{64U * 1024U * 1024U};
    std::uint64_t max_total_bytes{256U * 1024U * 1024U};
};

using PasswordVerifier =
    std::function<bool(std::string_view username, std::string_view password)>;

class Server final {
public:
    Server(ServerOptions options, PasswordVerifier password_verifier);
    ~Server();

    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    bool start();
    void stop() noexcept;
    bool running() const noexcept;
    std::uint16_t bound_port() const noexcept;

private:
    struct Worker;

    void run();
    void serve_client(int client_fd);
    void reap_workers(bool all);
    void track_client(int client_fd);
    void untrack_client(int client_fd);

    ServerOptions options_;
    PasswordVerifier password_verifier_;
    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> running_{false};
    std::atomic<std::uint16_t> bound_port_{0U};
    std::atomic<std::uint64_t> upload_sequence_{0U};
    int listener_fd_{-1};
    std::thread listener_worker_;
    std::vector<std::unique_ptr<Worker>> workers_;
    std::mutex clients_mutex_;
    std::mutex storage_mutex_;
    std::unordered_set<int> client_fds_;
};

}  // namespace uhf::ftp
