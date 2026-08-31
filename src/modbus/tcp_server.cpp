// SPDX-License-Identifier: GPL-3.0-only
#include "modbus/tcp_server.hpp"

#include "domain/snapshot.hpp"

#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <fcntl.h>
#include <limits>
#include <netinet/in.h>
#include <poll.h>
#include <stdexcept>
#include <sys/socket.h>
#include <unistd.h>
#include <utility>

namespace {

constexpr std::uint8_t kReadInputRegisters = 0x04U;
constexpr std::uint8_t kIllegalFunction = 0x01U;
constexpr std::uint8_t kIllegalDataAddress = 0x02U;
constexpr std::uint8_t kIllegalDataValue = 0x03U;
constexpr std::uint8_t kServerDeviceFailure = 0x04U;
constexpr std::uint8_t kGatewayTargetFailed = 0x0BU;
constexpr std::size_t kMbapHeaderBytes = 6U;
constexpr std::size_t kMaximumPduLength = 253U;
constexpr std::size_t kMaximumAduBytes = kMbapHeaderBytes + kMaximumPduLength;
constexpr std::size_t kMaximumInputBytes = 1024U;
constexpr std::size_t kMaximumOutputBytes = 2048U;

std::uint16_t read_u16(const std::uint8_t* bytes) noexcept {
    return static_cast<std::uint16_t>(
        static_cast<std::uint16_t>(bytes[0]) << 8U | static_cast<std::uint16_t>(bytes[1]));
}

void append_u16(std::vector<std::uint8_t>& bytes, std::uint16_t value) {
    bytes.push_back(static_cast<std::uint8_t>(value >> 8U));
    bytes.push_back(static_cast<std::uint8_t>(value & 0x00FFU));
}

std::vector<std::uint8_t> exception_response(
    const std::vector<std::uint8_t>& request, std::uint8_t function, std::uint8_t exception) {
    std::vector<std::uint8_t> response;
    response.reserve(9U);
    response.push_back(request[0]);
    response.push_back(request[1]);
    response.push_back(0U);
    response.push_back(0U);
    append_u16(response, 3U);
    response.push_back(request[6]);
    response.push_back(static_cast<std::uint8_t>(function | 0x80U));
    response.push_back(exception);
    return response;
}

bool set_nonblocking(int file_descriptor) {
    const int flags = ::fcntl(file_descriptor, F_GETFL, 0);
    return flags >= 0 && ::fcntl(file_descriptor, F_SETFL, flags | O_NONBLOCK) == 0;
}

struct Client {
    int file_descriptor;
    std::vector<std::uint8_t> input;
    std::vector<std::uint8_t> output;
};

}  // namespace

namespace uhf::modbus {

ModbusTcpServer::ModbusTcpServer(
    acquisition::SnapshotStore& snapshot_store, ModbusTcpOptions options)
    : snapshot_store_(snapshot_store), options_(std::move(options)) {
    if (options_.unit_id == 0U || options_.unit_id > 247U || options_.max_connections == 0U) {
        throw std::invalid_argument("invalid Modbus TCP options");
    }
}

std::vector<std::uint8_t> ModbusTcpServer::handle_request(
    const std::vector<std::uint8_t>& request) const {
    if (request.size() < kMbapHeaderBytes + 6U || request.size() > kMaximumAduBytes ||
        read_u16(request.data() + 2U) != 0U ||
        static_cast<std::size_t>(read_u16(request.data() + 4U)) + kMbapHeaderBytes !=
            request.size()) {
        return {};
    }

    const std::uint8_t unit_id = request[6];
    const std::uint8_t function = request[7];
    if (unit_id != options_.unit_id) {
        return exception_response(request, function, kGatewayTargetFailed);
    }
    if (function != kReadInputRegisters) {
        return exception_response(request, function, kIllegalFunction);
    }
    if (request.size() != kMbapHeaderBytes + 6U) {
        return exception_response(request, function, kIllegalDataValue);
    }

    const std::uint16_t start_address = read_u16(request.data() + 8U);
    const std::uint16_t register_count = read_u16(request.data() + 10U);
    if (register_count == 0U || register_count > 125U) {
        return exception_response(request, function, kIllegalDataValue);
    }
    const std::uint32_t last_address =
        static_cast<std::uint32_t>(start_address) + register_count - 1U;
    if (start_address < domain::kPd1000FirstAddress ||
        last_address > domain::kPd1000LastAddress) {
        return exception_response(request, function, kIllegalDataAddress);
    }

    const acquisition::ServingView serving_view = snapshot_store_.serving_view();
    if (!serving_view.snapshot) {
        return exception_response(request, function, kServerDeviceFailure);
    }

    const std::size_t offset = static_cast<std::size_t>(
        start_address - domain::kPd1000FirstAddress);
    const std::size_t count = static_cast<std::size_t>(register_count);
    std::vector<std::uint8_t> response;
    response.reserve(9U + count * 2U);
    response.push_back(request[0]);
    response.push_back(request[1]);
    response.push_back(0U);
    response.push_back(0U);
    append_u16(response, static_cast<std::uint16_t>(3U + count * 2U));
    response.push_back(unit_id);
    response.push_back(function);
    response.push_back(static_cast<std::uint8_t>(count * 2U));
    for (std::size_t index = 0; index < count; ++index) {
        append_u16(response, serving_view.snapshot->payload.raw_registers[offset + index]);
    }
    return response;
}

int ModbusTcpServer::run() {
    const int server_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        return 1;
    }
    if (!set_nonblocking(server_fd)) {
        ::close(server_fd);
        return 1;
    }
    const int reuse = 1;
    if (::setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
        ::close(server_fd);
        return 1;
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(options_.port);
    if (::inet_pton(AF_INET, options_.bind_address.c_str(), &address.sin_addr) != 1 ||
        ::bind(server_fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) < 0 ||
        ::listen(server_fd, 16) < 0) {
        ::close(server_fd);
        return 1;
    }
    sockaddr_in bound_address{};
    socklen_t bound_length = sizeof(bound_address);
    if (::getsockname(
            server_fd, reinterpret_cast<sockaddr*>(&bound_address), &bound_length) < 0) {
        ::close(server_fd);
        return 1;
    }
    bound_port_.store(ntohs(bound_address.sin_port));

    std::vector<Client> clients;
    while (!stop_requested_.load()) {
        std::vector<pollfd> descriptors;
        descriptors.reserve(clients.size() + 1U);
        descriptors.push_back(pollfd{server_fd, POLLIN, 0});
        for (const Client& client : clients) {
            short events = POLLIN;
            if (!client.output.empty()) {
                events = static_cast<short>(events | POLLOUT);
            }
            descriptors.push_back(pollfd{client.file_descriptor, events, 0});
        }

        const int poll_result = ::poll(descriptors.data(), descriptors.size(), 50);
        if (poll_result < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        for (std::size_t index = clients.size(); index > 0U; --index) {
            const std::size_t client_index = index - 1U;
            Client& client = clients[client_index];
            const short events = descriptors[client_index + 1U].revents;
            bool close_client = (events & (POLLERR | POLLHUP | POLLNVAL)) != 0;
            if (!close_client && (events & POLLIN) != 0) {
                std::uint8_t buffer[512];
                const ssize_t received = ::recv(client.file_descriptor, buffer, sizeof(buffer), 0);
                if (received <= 0) {
                    close_client = true;
                } else {
                    client.input.insert(
                        client.input.end(), buffer, buffer + static_cast<std::size_t>(received));
                    if (client.input.size() > kMaximumInputBytes) {
                        close_client = true;
                    }
                }
                while (!close_client && client.input.size() >= kMbapHeaderBytes) {
                    const std::size_t length =
                        static_cast<std::size_t>(read_u16(client.input.data() + 4U));
                    if (length < 2U || length > kMaximumPduLength) {
                        close_client = true;
                        break;
                    }
                    const std::size_t adu_size = kMbapHeaderBytes + length;
                    if (client.input.size() < adu_size) {
                        break;
                    }
                    const std::vector<std::uint8_t> request(
                        client.input.begin(), client.input.begin() + static_cast<std::ptrdiff_t>(adu_size));
                    client.input.erase(
                        client.input.begin(), client.input.begin() + static_cast<std::ptrdiff_t>(adu_size));
                    const std::vector<std::uint8_t> response = handle_request(request);
                    if (response.empty() || client.output.size() + response.size() > kMaximumOutputBytes) {
                        close_client = true;
                        break;
                    }
                    client.output.insert(client.output.end(), response.begin(), response.end());
                }
            }
            if (!close_client && (events & POLLOUT) != 0 && !client.output.empty()) {
                const ssize_t sent = ::send(
                    client.file_descriptor,
                    client.output.data(),
                    client.output.size(),
                    MSG_NOSIGNAL);
                if (sent > 0) {
                    client.output.erase(
                        client.output.begin(),
                        client.output.begin() + static_cast<std::ptrdiff_t>(sent));
                } else if (sent < 0 && (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
                    close_client = true;
                }
            }
            if (close_client) {
                ::close(client.file_descriptor);
                clients.erase(clients.begin() + static_cast<std::ptrdiff_t>(client_index));
            }
        }

        if ((descriptors[0].revents & POLLIN) != 0) {
            while (clients.size() < options_.max_connections) {
                const int client_fd = ::accept(server_fd, nullptr, nullptr);
                if (client_fd < 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                        break;
                    }
                    break;
                }
                if (!set_nonblocking(client_fd)) {
                    ::close(client_fd);
                    continue;
                }
                clients.push_back(Client{client_fd, {}, {}});
            }
            if (clients.size() >= options_.max_connections) {
                const int extra_fd = ::accept(server_fd, nullptr, nullptr);
                if (extra_fd >= 0) {
                    ::close(extra_fd);
                }
            }
        }
    }

    for (const Client& client : clients) {
        ::close(client.file_descriptor);
    }
    ::close(server_fd);
    bound_port_.store(0);
    return 0;
}

void ModbusTcpServer::stop() noexcept {
    stop_requested_.store(true);
}

std::uint16_t ModbusTcpServer::bound_port() const noexcept {
    return bound_port_.load();
}

}  // namespace uhf::modbus
