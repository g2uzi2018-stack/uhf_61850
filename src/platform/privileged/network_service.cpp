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
    std::filesystem::path network_file, std::filesystem::path transaction_file)
    : backend_(std::move(network_file), command_runner_),
      transaction_store_(std::move(transaction_file)),
      transaction_manager_(transaction_store_, backend_, clock_) {}

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
        std::string body = "{\"config\":" + network::to_flat_json(current) + ",\"transaction\":";
        if (transaction.status != network::LoadStatus::valid || !transaction.transaction) {
            body += "null";
        } else {
            body += "{\"id\":" + std::to_string(transaction.transaction->id) +
                ",\"state\":\"" +
                network::transaction_state_name(transaction.transaction->state) +
                "\",\"deadline_boottime_ns\":" +
                std::to_string(transaction.transaction->deadline_boottime_ns) + "}";
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
        return result_reply(result, transaction_manager_.last_error());
    }
    if (request == "network.rollback") {
        const network::TransactionResult result = transaction_manager_.rollback_now();
        return result_reply(result, transaction_manager_.last_error());
    }
    return {false, "unknown_operation", {}};
}

network::TransactionResult NetworkService::recover_pending() {
    std::lock_guard<std::mutex> lock(mutex_);
    return transaction_manager_.rollback_if_needed();
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
            (void)transaction_manager_.rollback_if_needed();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
}

}  // namespace uhf::privileged
