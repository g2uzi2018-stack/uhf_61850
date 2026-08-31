// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "platform/network/network_config.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <atomic>
#include <string>
#include <string_view>
#include <sys/types.h>

namespace uhf::privileged {

constexpr std::size_t kMaxMessageBytes = 64U * 1024U;

struct Reply {
    bool ok{false};
    std::string code;
    std::string body;
};

using RequestHandler = std::function<Reply(std::string_view, uid_t, gid_t)>;

class UnixSocketClient {
public:
    explicit UnixSocketClient(std::filesystem::path socket_path);

    Reply request(std::string_view message) const;
    Reply network_status() const;
    Reply network_stage(const uhf::network::NetworkConfig& candidate) const;
    Reply network_confirm() const;
    Reply network_rollback() const;
    Reply maintenance_restart_service() const;
    Reply maintenance_reboot() const;

private:
    std::filesystem::path socket_path_;
};

class UnixSocketServer {
public:
    UnixSocketServer(
        std::filesystem::path socket_path, uid_t allowed_uid, RequestHandler handler);
    ~UnixSocketServer();

    UnixSocketServer(const UnixSocketServer&) = delete;
    UnixSocketServer& operator=(const UnixSocketServer&) = delete;

    int run();
    void stop() noexcept;

private:
    void handle_client(int client_fd);

    std::filesystem::path socket_path_;
    uid_t allowed_uid_;
    RequestHandler handler_;
    int server_fd_{-1};
    std::atomic<bool> stop_requested_{false};
};

}  // namespace uhf::privileged
