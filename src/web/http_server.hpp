// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "acquisition/acquisition.hpp"
#include "health/health.hpp"
#include "web/auth_store.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string_view>
#include <string>
#include <unordered_map>

namespace uhf::web {

using HealthInputProvider = std::function<health::Input()>;

class HttpServer {
public:
    HttpServer(
        std::filesystem::path document_root,
        std::string bind_address,
        std::uint16_t port,
        std::filesystem::path state_directory,
        const acquisition::SnapshotStore* snapshot_store = nullptr,
        HealthInputProvider health_input_provider = {});

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
    void run_websocket(int client_fd);
    void cleanup_sessions(std::chrono::steady_clock::time_point now);
    health::Report health_report(std::chrono::steady_clock::time_point now) const;
    std::optional<std::string> snapshot_json() const;

    std::filesystem::path document_root_;
    std::string bind_address_;
    std::uint16_t port_;
    AuthStore auth_store_;
    const acquisition::SnapshotStore* snapshot_store_{nullptr};
    HealthInputProvider health_input_provider_;
    health::Aggregator health_aggregator_;
    std::unordered_map<std::string, Session> sessions_;
    std::unordered_map<std::string, LoginFailures> login_failures_;
    std::atomic<std::size_t> active_websocket_count_{0U};
};

}  // namespace uhf::web
