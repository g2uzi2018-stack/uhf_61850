// SPDX-License-Identifier: GPL-3.0-only
#include "platform/privileged/network_service.hpp"

#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <utility>

namespace {

std::string json_escape(std::string_view value) {
    std::string result;
    result.reserve(value.size());
    for (const char character : value) {
        if (character == '"' || character == '\\') {
            result.push_back('\\');
        }
        result.push_back(character);
    }
    return result;
}

uhf::privileged::Reply result_reply(
    uhf::network::TransactionResult result, std::string_view message) {
    const std::string body = "{\"result\":\"" +
        uhf::network::transaction_result_name(result) + "\",\"message\":\"" +
        json_escape(message) + "\"}\n";
    return {result == uhf::network::TransactionResult::ok, 
            uhf::network::transaction_result_name(result), body};
}

}  // namespace

namespace uhf::privileged {

NetworkService::NetworkService(
    std::filesystem::path network_file,
    std::filesystem::path transaction_file,
    MaintenanceRunner* maintenance_runner,
    bool apply_network_runtime,
    network::VendorNetworkPaths vendor_paths)
    : dhcp_client_(
          network_file.parent_path().empty() ? std::filesystem::path{"."} :
                                                network_file.parent_path() / "dhcp"),
      backend_(network_file, command_runner_, &dhcp_client_, std::move(vendor_paths)),
      transaction_store_(std::move(transaction_file)),
      transaction_manager_(transaction_store_, backend_, clock_),
      maintenance_runner_(maintenance_runner == nullptr ? default_maintenance_runner_ : *maintenance_runner),
      apply_network_runtime_(apply_network_runtime) {}

NetworkService::~NetworkService() {
    stop_rollback_monitor();
}

Reply NetworkService::handle(std::string_view request, uid_t uid, gid_t gid) {
    static_cast<void>(uid);
    static_cast<void>(gid);
    std::lock_guard<std::mutex> lock(mutex_);
    if (request == "network.status") {
        network::NetworkConfig current;
        if (!backend_.read_current(current)) {
            return {false, "backend_error", {}};
        }
        const network::LoadResult transaction = transaction_manager_.inspect();
        if (transaction.status == network::LoadStatus::corrupt ||
            transaction.status == network::LoadStatus::io_error) {
            return {false, "transaction_error", {}};
        }
        network::NetworkStatus actual;
        if (!status_reader_.read(actual)) {
            return {false, "status_error", {}};
        }
        const auto status_json = [](const network::InterfaceStatus& status) {
            return "{\"exists\":" + std::string(status.exists ? "true" : "false") +
                ",\"carrier_known\":" + std::string(status.carrier_known ? "true" : "false") +
                ",\"link_up\":" + std::string(status.link_up ? "true" : "false") +
                ",\"address\":\"" + json_escape(status.address) +
                "\",\"prefix\":" + std::to_string(status.prefix) + "}";
        };
        std::string body = "{\"config\":" + network::to_json(current) +
            ",\"actual\":{\"eth0\":" + status_json(actual.eth0) +
            ",\"eth1\":" + status_json(actual.eth1) + "},\"transaction\":";
        if (!transaction.transaction) {
            body += "null";
        } else {
            const std::uint64_t now = clock_.boottime_ns();
            const std::uint64_t remaining_ns = transaction.transaction->deadline_boottime_ns > now
                ? transaction.transaction->deadline_boottime_ns - now
                : 0U;
            const std::uint64_t remaining_seconds =
                (remaining_ns + 999999999ULL) / 1000000000ULL;
            body += "{\"id\":" + std::to_string(transaction.transaction->id) +
                ",\"state\":\"" +
                network::transaction_state_name(transaction.transaction->state) +
                "\",\"deadline_boottime_ns\":" +
                std::to_string(transaction.transaction->deadline_boottime_ns) +
                ",\"remaining_seconds\":" + std::to_string(remaining_seconds) + "}";
        }
        body += "}\n";
        return {true, "ok", std::move(body)};
    }
    if (request.rfind("network.stage\n", 0U) == 0U) {
        network::NetworkConfig candidate;
        if (!network::parse_flat_json(request.substr(14U), candidate)) {
            return result_reply(
                network::TransactionResult::invalid_candidate,
                "candidate network configuration is invalid");
        }
        const network::TransactionResult result = transaction_manager_.stage(candidate);
        return result_reply(result, transaction_manager_.last_error());
    }
    if (request == "network.confirm") {
        const network::TransactionResult result = transaction_manager_.confirm();
        if (apply_network_runtime_ &&
            (result == network::TransactionResult::ok ||
             result == network::TransactionResult::expired ||
             result == network::TransactionResult::boot_changed) &&
            !backend_.start_runtime()) {
            return result_reply(
                network::TransactionResult::backend_error,
                "network confirmed but DHCP runtime could not start");
        }
        return result_reply(result, transaction_manager_.last_error());
    }
    if (request == "network.rollback") {
        const network::TransactionResult result = transaction_manager_.rollback_now();
        if (apply_network_runtime_ && result == network::TransactionResult::ok &&
            !backend_.start_runtime()) {
            return result_reply(
                network::TransactionResult::backend_error,
                "network rolled back but DHCP runtime could not start");
        }
        return result_reply(result, transaction_manager_.last_error());
    }
    if (request == "maintenance.restart-service") {
        return maintenance_runner_.restart_service()
            ? Reply{true, "ok", "{\"result\":\"ok\"}\n"}
            : Reply{false, "maintenance_error", {}};
    }
    if (request == "maintenance.reboot") {
        return maintenance_runner_.reboot()
            ? Reply{true, "ok", "{\"result\":\"ok\"}\n"}
            : Reply{false, "maintenance_error", {}};
    }
    if (request.rfind("time.sync\n", 0U) == 0U) {
        const std::string_view server = request.substr(10U);
        if (!valid_sntp_server(server)) {
            return {false, "invalid_time_request", {}};
        }
        return maintenance_runner_.configure_sntp(server)
            ? Reply{true, "ok", "{\"result\":\"ok\"}\n"}
            : Reply{false, "maintenance_error", {}};
    }
    if (request == "time.disable") {
        return maintenance_runner_.disable_sntp()
            ? Reply{true, "ok", "{\"result\":\"ok\"}\n"}
            : Reply{false, "maintenance_error", {}};
    }
    if (request.rfind("time.set\n", 0U) == 0U) {
        const std::string_view local_time = request.substr(9U);
        if (!valid_system_time(local_time)) {
            return {false, "invalid_time_request", {}};
        }
        return maintenance_runner_.set_system_time(local_time)
            ? Reply{true, "ok", "{\"result\":\"ok\"}\n"}
            : Reply{false, "maintenance_error", {}};
    }
    return {false, "unknown_operation", {}};
}

network::TransactionResult NetworkService::recover_pending(bool start_runtime) {
    std::lock_guard<std::mutex> lock(mutex_);
    const network::TransactionResult result = transaction_manager_.rollback_if_needed();
    if (result == network::TransactionResult::not_due) {
        return result;
    }
    if (!start_runtime && result == network::TransactionResult::no_transaction) {
        network::NetworkConfig current;
        if (!backend_.read_current(current)) {
            return network::TransactionResult::backend_error;
        }
    }
    if (start_runtime && apply_network_runtime_ &&
        (result == network::TransactionResult::no_transaction ||
         result == network::TransactionResult::ok ||
         result == network::TransactionResult::expired ||
         result == network::TransactionResult::boot_changed)) {
        if (!backend_.start_runtime()) {
            return network::TransactionResult::backend_error;
        }
    }
    return result;
}

void NetworkService::start_rollback_monitor() {
    if (rollback_worker_.joinable()) {
        return;
    }
    stop_requested_.store(false);
    rollback_worker_ = std::thread(&NetworkService::rollback_loop, this);
}

void NetworkService::stop_rollback_monitor() noexcept {
    stop_requested_.store(true);
    if (rollback_worker_.joinable()) {
        rollback_worker_.join();
    }
}

void NetworkService::rollback_loop() {
    while (!stop_requested_.load()) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const network::TransactionResult result = transaction_manager_.rollback_if_needed();
            if (result != network::TransactionResult::not_due &&
                (result == network::TransactionResult::ok ||
                 result == network::TransactionResult::expired ||
                 result == network::TransactionResult::boot_changed)) {
                if (apply_network_runtime_) {
                    (void)backend_.start_runtime();
                    (void)backend_.refresh_runtime();
                }
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
}

}  // namespace uhf::privileged
