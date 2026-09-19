// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "acquisition/acquisition.hpp"
#include "iec61850/endpoint.hpp"
#include "iec61850/scl_model.hpp"
#include "iec61850/stats.hpp"
#include "v3/acquisition.hpp"

#if UHF_ENABLE_IEC61850
#include "iec61850/model.hpp"
#endif

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace uhf::iec61850 {

struct ServerOptions {
    std::string bind_address{"127.0.0.1"};
    std::uint16_t port{102U};
    std::string ied_name{"UHFPD1"};
    std::function<bool()> alarm_provider;
    std::optional<SclModelDefinition> model_definition;
    const v3::SnapshotStore* v3_snapshot_store{nullptr};
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
    RuntimeStats stats() const noexcept;
    RuntimeEndpoint endpoint() const;
    SclModelDefinition model_definition() const;

private:
#if UHF_ENABLE_IEC61850
    void start_locked();
    void stop_locked() noexcept;
    void update_loop();
    void publish_invalid_values(bool communication_alarm = false);
    void publish_snapshot(const acquisition::ServingView& serving_view);
    void publish_v3_snapshot(const v3::UnifiedSnapshot& snapshot);
    void publish_v3_invalid_values();
    void update_communication_alarm(bool active, std::uint64_t timestamp_ms);
    void update_timestamp(DataAttribute* attribute, std::uint64_t timestamp_ms);
#endif

    acquisition::SnapshotStore& snapshot_store_;
    ServerOptions options_;
#if UHF_ENABLE_IEC61850
    std::unique_ptr<Model> model_;
    IedServer server_{nullptr};
#endif
    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> running_{false};
    std::thread update_worker_;
    std::function<bool()> alarm_provider_;
    const v3::SnapshotStore* v3_snapshot_store_{nullptr};
    mutable std::mutex lifecycle_mutex_;
};

}  // namespace uhf::iec61850
