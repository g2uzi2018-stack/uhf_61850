// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "acquisition/acquisition.hpp"
#include "config/config_store.hpp"
#include "health/health.hpp"
#include "iec61850/stats.hpp"
#include "logging/logger.hpp"
#include "platform/privileged/unix_socket.hpp"
#include "storage/event_store.hpp"
#include "storage/frame_store.hpp"
#include "web/auth_store.hpp"
#include "web/tls_context.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <netinet/in.h>
#include <optional>
#include <string_view>
#include <string>
#include <unordered_map>

namespace uhf::web {

using HealthInputProvider = std::function<health::Input()>;
using Iec61850StatsProvider = std::function<iec61850::RuntimeStats()>;

class HttpServer {
public:
    HttpServer(
        std::filesystem::path document_root,
        std::string bind_address,
        std::uint16_t port,
        std::filesystem::path state_directory,
        const acquisition::SnapshotStore* snapshot_store = nullptr,
        HealthInputProvider health_input_provider = {},
        config::ConfigStore* config_store = nullptr,
        bool tls_enabled = true,
        TlsFiles tls_files = {},
        logging::Logger* logger = nullptr,
        std::filesystem::path data_root = {},
        privileged::UnixSocketClient* network_client = nullptr,
        bool reload_web_endpoint = false,
        Iec61850StatsProvider iec61850_stats_provider = {});

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

    bool handle_client(int client_fd, SSL* tls, std::string remote_address);
    void serve_client(int client_fd, sockaddr_in client_address);
    void run_websocket(int client_fd, SSL* tls);
    void cleanup_sessions(std::chrono::steady_clock::time_point now);
    health::Report health_report(std::chrono::steady_clock::time_point now) const;
    std::string iec61850_json(std::chrono::steady_clock::time_point now) const;
    std::optional<std::string> snapshot_json() const;
    std::string logs_json(std::size_t limit) const;
    std::optional<std::string> frames_json() const;
    std::optional<std::string> events_json() const;
    std::optional<std::string> latest_frame_csv() const;
    std::optional<std::string> latest_event_csv() const;

    std::filesystem::path document_root_;
    std::string bind_address_;
    std::uint16_t port_;
    AuthStore auth_store_;
    const acquisition::SnapshotStore* snapshot_store_{nullptr};
    HealthInputProvider health_input_provider_;
    Iec61850StatsProvider iec61850_stats_provider_;
    config::ConfigStore* config_store_{nullptr};
    bool tls_enabled_{true};
    std::unique_ptr<TlsContext> tls_context_;
    logging::Logger* logger_{nullptr};
    std::filesystem::path data_root_;
    privileged::UnixSocketClient* network_client_{nullptr};
    bool reload_web_endpoint_{false};
    health::Aggregator health_aggregator_;
    std::unordered_map<std::string, Session> sessions_;
    std::unordered_map<std::string, LoginFailures> login_failures_;
    std::mutex state_mutex_;
    std::atomic<std::size_t> active_http_count_{0U};
    std::atomic<std::size_t> active_websocket_count_{0U};
};

}  // namespace uhf::web
