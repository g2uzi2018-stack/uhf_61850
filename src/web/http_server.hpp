// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "web/auth_store.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string_view>
#include <string>
#include <unordered_map>

namespace uhf::web {

class HttpServer {
public:
    HttpServer(
        std::filesystem::path document_root,
        std::string bind_address,
        std::uint16_t port,
        std::filesystem::path state_directory);

    int run();

private:
    struct Session {
        std::string csrf_token;
        std::string remote_address;
        std::chrono::steady_clock::time_point expires_at;
    };

    struct LoginFailures {
        std::size_t count{0};
        std::chrono::steady_clock::time_point window_started;
        std::chrono::steady_clock::time_point blocked_until;
    };

    void handle_client(int client_fd, std::string remote_address);
    void cleanup_sessions(std::chrono::steady_clock::time_point now);

    std::filesystem::path document_root_;
    std::string bind_address_;
    std::uint16_t port_;
    AuthStore auth_store_;
    std::unordered_map<std::string, Session> sessions_;
    std::unordered_map<std::string, LoginFailures> login_failures_;
};

}  // namespace uhf::web
