// SPDX-License-Identifier: GPL-3.0-only
#include "iec61850/server.hpp"

#include <stdexcept>
#include <utility>

namespace uhf::iec61850 {

Server::Server(acquisition::SnapshotStore& snapshot_store, ServerOptions options)
    : snapshot_store_(snapshot_store),
      options_(std::move(options)),
      alarm_provider_(options_.alarm_provider),
      v3_snapshot_store_(options_.v3_snapshot_store) {}

Server::~Server() = default;

void Server::start() {
    throw std::runtime_error("IEC 61850 support is disabled in this build");
}

bool Server::update_endpoint(std::string bind_address, std::uint16_t port) {
    static_cast<void>(bind_address);
    static_cast<void>(port);
    return false;
}

void Server::stop() noexcept {
    running_.store(false);
}

bool Server::running() const noexcept {
    return false;
}

RuntimeStats Server::stats() const noexcept {
    return {};
}

RuntimeEndpoint Server::endpoint() const {
    return {options_.bind_address, options_.port};
}

SclModelDefinition Server::model_definition() const {
    if (options_.model_definition) {
        return *options_.model_definition;
    }
    return options_.v3_snapshot_store != nullptr
        ? default_v3_model_definition(options_.ied_name)
        : default_model_definition(options_.ied_name);
}

}  // namespace uhf::iec61850
