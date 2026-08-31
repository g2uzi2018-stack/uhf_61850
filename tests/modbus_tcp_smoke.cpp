// SPDX-License-Identifier: GPL-3.0-only
#include "modbus/tcp_server.hpp"

#include <array>
#include <arpa/inet.h>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <netinet/in.h>
#include <optional>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "Modbus TCP smoke failed: " << message << '\n';
        return false;
    }
    return true;
}

std::vector<std::uint8_t> request(
    std::uint16_t transaction, std::uint8_t unit_id, std::uint8_t function,
    std::uint16_t start_address, std::uint16_t count) {
    return {
        static_cast<std::uint8_t>(transaction >> 8U),
        static_cast<std::uint8_t>(transaction & 0x00FFU),
        0U,
        0U,
        0U,
        6U,
        unit_id,
        function,
        static_cast<std::uint8_t>(start_address >> 8U),
        static_cast<std::uint8_t>(start_address & 0x00FFU),
        static_cast<std::uint8_t>(count >> 8U),
        static_cast<std::uint8_t>(count & 0x00FFU),
    };
}

std::uint16_t unused_port() {
    const int descriptor = ::socket(AF_INET, SOCK_STREAM, 0);
    if (descriptor < 0) {
        return 0U;
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0U;
    if (::bind(descriptor, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) < 0) {
        ::close(descriptor);
        return 0U;
    }
    socklen_t length = sizeof(address);
    const bool available =
        ::getsockname(descriptor, reinterpret_cast<sockaddr*>(&address), &length) == 0;
    const std::uint16_t port = available ? ntohs(address.sin_port) : 0U;
    ::close(descriptor);
    return port;
}

}  // namespace

int main() {
    uhf::acquisition::SnapshotStore store;
    uhf::domain::ParsedSnapshot payload;
    payload.raw_registers.fill(0x1234U);
    payload.raw_registers.front() = 0xFFCEU;
    payload.raw_registers.back() = 0x5678U;
    store.publish(payload, std::chrono::steady_clock::now(), std::chrono::steady_clock::now());

    uhf::modbus::ModbusTcpOptions options;
    options.bind_address = "127.0.0.1";
    options.port = 0;
    options.unit_id = 7U;
    uhf::modbus::ModbusTcpServer server(store, options);
    std::thread server_thread([&server]() { server.run(); });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (server.bound_port() == 0U && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    if (!expect(server.bound_port() != 0U, "server did not bind")) {
        server.stop();
        server_thread.join();
        return 1;
    }

    const int client_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (!expect(client_fd >= 0, "unable to create client socket")) {
        server.stop();
        server_thread.join();
        return 1;
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(server.bound_port());
    ::inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
    if (!expect(::connect(client_fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0,
            "unable to connect client")) {
        ::close(client_fd);
        server.stop();
        server_thread.join();
        return 1;
    }

    const std::vector<std::uint8_t> valid_request = request(0x1234U, 7U, 0x04U, 10001U, 5U);
    if (!expect(::send(client_fd, valid_request.data(), 3U, 0) == 3, "fragmented request prefix") ||
        !expect(::send(client_fd, valid_request.data() + 3U, valid_request.size() - 3U, 0) == 9,
            "fragmented request suffix")) {
        ::close(client_fd);
        server.stop();
        server_thread.join();
        return 1;
    }
    std::array<std::uint8_t, 32U> response{};
    const ssize_t received = ::recv(client_fd, response.data(), response.size(), 0);
    if (!expect(received == 19, "valid response size") ||
        !expect(response[0] == 0x12U && response[1] == 0x34U, "transaction echo") ||
        !expect(response[6] == 7U && response[7] == 0x04U && response[8] == 10U,
            "valid response header") ||
        !expect(response[9] == 0xFFU && response[10] == 0xCEU, "first register mirror")) {
        ::close(client_fd);
        server.stop();
        server_thread.join();
        return 1;
    }

    store.record_failure(std::chrono::steady_clock::now(), "serial timeout");
    const std::vector<std::uint8_t> stale_response = server.handle_request(valid_request);
    if (!expect(stale_response.size() == 19U, "stale response size") ||
        !expect(stale_response[9] == 0xFFU && stale_response[10] == 0xCEU,
            "stale last-good register mirror")) {
        ::close(client_fd);
        server.stop();
        server_thread.join();
        return 1;
    }

    const std::vector<std::uint8_t> invalid_address = request(2U, 7U, 0x04U, 10000U, 1U);
    ::send(client_fd, invalid_address.data(), invalid_address.size(), 0);
    const ssize_t invalid_received = ::recv(client_fd, response.data(), response.size(), 0);
    if (!expect(invalid_received == 9, "address exception size") ||
        !expect(response[7] == 0x84U && response[8] == 0x02U, "address exception")) {
        ::close(client_fd);
        server.stop();
        server_thread.join();
        return 1;
    }

    const std::vector<std::uint8_t> wrong_unit = request(3U, 8U, 0x04U, 10001U, 1U);
    ::send(client_fd, wrong_unit.data(), wrong_unit.size(), 0);
    const ssize_t wrong_unit_received = ::recv(client_fd, response.data(), response.size(), 0);
    if (!expect(wrong_unit_received == 9, "unit exception size") ||
        !expect(response[7] == 0x84U && response[8] == 0x0BU, "unit exception 0x0B")) {
        ::close(client_fd);
        server.stop();
        server_thread.join();
        return 1;
    }

    const std::vector<std::uint8_t> wrong_function = request(4U, 7U, 0x03U, 10001U, 1U);
    ::send(client_fd, wrong_function.data(), wrong_function.size(), 0);
    const ssize_t wrong_function_received = ::recv(client_fd, response.data(), response.size(), 0);
    if (!expect(wrong_function_received == 9, "function exception size") ||
        !expect(response[7] == 0x83U && response[8] == 0x01U, "function exception")) {
        ::close(client_fd);
        server.stop();
        server_thread.join();
        return 1;
    }
    ::close(client_fd);

    const std::uint16_t reloaded_port = unused_port();
    uhf::modbus::ModbusTcpOptions reloaded_options = options;
    reloaded_options.port = reloaded_port;
    reloaded_options.unit_id = 8U;
    if (!expect(reloaded_port != 0U, "unable to reserve reload port") ||
        !expect(server.update_options(reloaded_options), "valid TCP options reload")) {
        server.stop();
        server_thread.join();
        return 1;
    }
    const auto reload_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (server.bound_port() != reloaded_port &&
           std::chrono::steady_clock::now() < reload_deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    if (!expect(server.bound_port() == reloaded_port, "TCP endpoint was not rebound")) {
        server.stop();
        server_thread.join();
        return 1;
    }
    const int reloaded_client_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (!expect(reloaded_client_fd >= 0, "unable to create reloaded client socket")) {
        server.stop();
        server_thread.join();
        return 1;
    }
    address.sin_port = htons(reloaded_port);
    if (!expect(
            ::connect(
                reloaded_client_fd,
                reinterpret_cast<const sockaddr*>(&address),
                sizeof(address)) == 0,
            "unable to connect reloaded TCP endpoint")) {
        ::close(reloaded_client_fd);
        server.stop();
        server_thread.join();
        return 1;
    }
    const std::vector<std::uint8_t> reloaded_request =
        request(5U, 8U, 0x04U, 10001U, 1U);
    if (!expect(
            ::send(
                reloaded_client_fd,
                reloaded_request.data(),
                reloaded_request.size(),
                0) == static_cast<ssize_t>(reloaded_request.size()),
            "reloaded TCP request") ||
        !expect(
            ::recv(reloaded_client_fd, response.data(), response.size(), 0) == 11,
            "reloaded TCP response") ||
        !expect(response[6] == 8U && response[7] == 0x04U, "reloaded TCP response unit")) {
        ::close(reloaded_client_fd);
        server.stop();
        server_thread.join();
        return 1;
    }
    ::close(reloaded_client_fd);

    uhf::acquisition::SnapshotStore empty_store;
    uhf::modbus::ModbusTcpServer empty_server(empty_store, options);
    const std::vector<std::uint8_t> empty_response = empty_server.handle_request(valid_request);
    if (!expect(empty_response.size() == 9U, "empty snapshot response size") ||
        !expect(empty_response[7] == 0x84U && empty_response[8] == 0x04U,
            "empty snapshot device failure")) {
        server.stop();
        server_thread.join();
        return 1;
    }

    server.stop();
    server_thread.join();
    std::cout << "Modbus TCP smoke: OK\n";
    return 0;
}
