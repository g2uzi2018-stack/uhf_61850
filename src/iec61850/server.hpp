// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "acquisition/acquisition.hpp"
#include "iec61850/model.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace uhf::iec61850 {

struct ServerOptions {
    std::string bind_address{"127.0.0.1"};
    std::uint16_t port{102U};
    std::string ied_name{"UHFPD1"};
    std::function<bool()> alarm_provider;
};

class Server final {
public:
    Server(acquisition::SnapshotStore& snapshot_store, ServerOptions options = {});
    ~Server();

    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    void start();
    bool update_endpoint(std::string bind_address, std::uint16_t port);
    void stop() noexcept;
    bool running() const noexcept;

private:
    void start_locked();
    void stop_locked() noexcept;
    void update_loop();
    void publish_invalid_values();
    void publish_snapshot(const acquisition::ServingView& serving_view);
    void update_timestamp(DataAttribute* attribute, std::uint64_t timestamp_ms);

    acquisition::SnapshotStore& snapshot_store_;
    ServerOptions options_;
    std::unique_ptr<Model> model_;
    IedServer server_{nullptr};
    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> running_{false};
    std::thread update_worker_;
    std::function<bool()> alarm_provider_;
    mutable std::mutex lifecycle_mutex_;
};

}  // namespace uhf::iec61850
