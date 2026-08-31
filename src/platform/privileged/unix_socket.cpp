// SPDX-License-Identifier: GPL-3.0-only
#include "platform/privileged/unix_socket.hpp"

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <poll.h>
#include <stdexcept>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <utility>

namespace {

constexpr int kSocketBacklog = 4;
constexpr int kIoTimeoutMilliseconds = 2000;
constexpr mode_t kSocketMode = S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP;

std::uint32_t read_u32(const std::uint8_t* bytes) noexcept {
    return static_cast<std::uint32_t>(bytes[0]) << 24U |
        static_cast<std::uint32_t>(bytes[1]) << 16U |
        static_cast<std::uint32_t>(bytes[2]) << 8U | static_cast<std::uint32_t>(bytes[3]);
}

void write_u32(std::uint8_t* bytes, std::uint32_t value) noexcept {
    bytes[0] = static_cast<std::uint8_t>(value >> 24U);
    bytes[1] = static_cast<std::uint8_t>(value >> 16U);
    bytes[2] = static_cast<std::uint8_t>(value >> 8U);
    bytes[3] = static_cast<std::uint8_t>(value);
}

bool wait_for(int file_descriptor, short events) noexcept {
    pollfd descriptor{file_descriptor, events, 0};
    while (true) {
        const int result = ::poll(&descriptor, 1, kIoTimeoutMilliseconds);
        if (result < 0 && errno == EINTR) {
            continue;
        }
        return result > 0 && (descriptor.revents & events) != 0;
    }
}

bool write_all(int file_descriptor, const void* data, std::size_t size) noexcept {
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    std::size_t offset = 0U;
    while (offset < size) {
        if (!wait_for(file_descriptor, POLLOUT)) {
            return false;
        }
        const ssize_t written = ::send(
            file_descriptor, bytes + offset, size - offset, MSG_NOSIGNAL);
        if (written > 0) {
            offset += static_cast<std::size_t>(written);
        } else if (written < 0 && errno == EINTR) {
            continue;
        } else {
            return false;
        }
    }
    return true;
}

bool read_all(int file_descriptor, void* data, std::size_t size) noexcept {
    auto* bytes = static_cast<std::uint8_t*>(data);
    std::size_t offset = 0U;
    while (offset < size) {
        if (!wait_for(file_descriptor, POLLIN)) {
            return false;
        }
        const ssize_t received = ::recv(file_descriptor, bytes + offset, size - offset, 0);
        if (received > 0) {
            offset += static_cast<std::size_t>(received);
        } else if (received < 0 && errno == EINTR) {
            continue;
        } else {
            return false;
        }
    }
    return true;
}

bool make_address(const std::filesystem::path& path, sockaddr_un& address) noexcept {
    const std::string text = path.string();
    if (text.empty() || text.size() >= sizeof(address.sun_path)) {
        return false;
    }
    address = {};
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, text.c_str(), text.size() + 1U);
    return true;
}

bool send_message(int file_descriptor, std::string_view message) noexcept {
    if (message.size() > uhf::privileged::kMaxMessageBytes ||
        message.size() > std::numeric_limits<std::uint32_t>::max()) {
        return false;
    }
    std::uint8_t header[4U]{};
    write_u32(header, static_cast<std::uint32_t>(message.size()));
    return write_all(file_descriptor, header, sizeof(header)) &&
        write_all(file_descriptor, message.data(), message.size());
}

bool receive_message(int file_descriptor, std::string& message) noexcept {
    std::uint8_t header[4U]{};
    if (!read_all(file_descriptor, header, sizeof(header))) {
        return false;
    }
    const std::uint32_t length = read_u32(header);
    if (length > uhf::privileged::kMaxMessageBytes) {
        return false;
    }
    message.resize(length);
    return length == 0U || read_all(file_descriptor, message.data(), message.size());
}

std::string reply_message(const uhf::privileged::Reply& reply) {
    const std::string prefix = reply.ok ? "OK\n" : "ERROR\n";
    return prefix + reply.code + "\n" + reply.body;
}

uhf::privileged::Reply parse_reply(std::string_view message) {
    const bool ok = message.rfind("OK\n", 0U) == 0U;
    const bool error = message.rfind("ERROR\n", 0U) == 0U;
    if (!ok && !error) {
        return {false, "protocol_error", {}};
    }
    const std::size_t prefix_size = ok ? 3U : 6U;
    const std::size_t separator = message.find('\n', prefix_size);
    if (separator == std::string_view::npos || separator == prefix_size ||
        message.find('\n', prefix_size) != separator) {
        return {false, "protocol_error", {}};
    }
    return {ok, std::string(message.substr(prefix_size, separator - prefix_size)),
            std::string(message.substr(separator + 1U))};
}

}  // namespace

namespace uhf::privileged {

UnixSocketClient::UnixSocketClient(std::filesystem::path socket_path)
    : socket_path_(std::move(socket_path)) {}

Reply UnixSocketClient::request(std::string_view message) const {
    if (message.size() > kMaxMessageBytes) {
        return {false, "message_too_large", {}};
    }
    const int file_descriptor = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (file_descriptor < 0) {
        return {false, "unavailable", {}};
    }
    sockaddr_un address{};
    if (!make_address(socket_path_, address) ||
        ::connect(
            file_descriptor,
            reinterpret_cast<const sockaddr*>(&address),
            static_cast<socklen_t>(sizeof(address))) < 0 ||
        !send_message(file_descriptor, message)) {
        ::close(file_descriptor);
        return {false, "unavailable", {}};
    }
    std::string response;
    const bool received = receive_message(file_descriptor, response);
    ::close(file_descriptor);
    return received ? parse_reply(response) : Reply{false, "unavailable", {}};
}

Reply UnixSocketClient::network_status() const {
    return request("network.status");
}

Reply UnixSocketClient::network_stage(const uhf::network::NetworkConfig& candidate) const {
    return request("network.stage\n" + uhf::network::to_flat_json(candidate));
}

Reply UnixSocketClient::network_confirm() const {
    return request("network.confirm");
}

Reply UnixSocketClient::network_rollback() const {
    return request("network.rollback");
}

UnixSocketServer::UnixSocketServer(
    std::filesystem::path socket_path, uid_t allowed_uid, RequestHandler handler)
    : socket_path_(std::move(socket_path)), allowed_uid_(allowed_uid), handler_(std::move(handler)) {
    if (socket_path_.empty() || !handler_) {
        throw std::invalid_argument("invalid privileged socket options");
    }
}

UnixSocketServer::~UnixSocketServer() {
    stop();
    if (!socket_path_.empty()) {
        (void)::unlink(socket_path_.c_str());
    }
}

int UnixSocketServer::run() {
    const std::filesystem::path parent = socket_path_.parent_path().empty()
        ? "."
        : socket_path_.parent_path();
    std::error_code error;
    std::filesystem::create_directories(parent, error);
    if (error || !std::filesystem::is_directory(parent, error) || error) {
        return 1;
    }
    (void)::unlink(socket_path_.c_str());
    server_fd_ = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (server_fd_ < 0) {
        return 1;
    }
    sockaddr_un address{};
    if (!make_address(socket_path_, address) ||
        ::bind(
            server_fd_,
            reinterpret_cast<const sockaddr*>(&address),
            static_cast<socklen_t>(sizeof(address))) < 0 ||
        ::chmod(socket_path_.c_str(), kSocketMode) < 0 ||
        ::listen(server_fd_, kSocketBacklog) < 0) {
        ::close(server_fd_);
        server_fd_ = -1;
        (void)::unlink(socket_path_.c_str());
        return 1;
    }

    while (!stop_requested_) {
        pollfd descriptor{server_fd_, POLLIN, 0};
        const int result = ::poll(&descriptor, 1, 100);
        if (result < 0 && errno == EINTR) {
            continue;
        }
        if (result <= 0) {
            continue;
        }
        if ((descriptor.revents & POLLIN) != 0) {
            const int client_fd = ::accept4(server_fd_, nullptr, nullptr, SOCK_CLOEXEC);
            if (client_fd >= 0) {
                handle_client(client_fd);
                ::close(client_fd);
            }
        }
    }
    ::close(server_fd_);
    server_fd_ = -1;
    return 0;
}

void UnixSocketServer::stop() noexcept {
    stop_requested_ = true;
}

void UnixSocketServer::handle_client(int client_fd) {
    std::string request;
    if (!receive_message(client_fd, request)) {
        return;
    }
    ucred credentials{};
    socklen_t length = sizeof(credentials);
    if (::getsockopt(client_fd, SOL_SOCKET, SO_PEERCRED, &credentials, &length) < 0) {
        return;
    }
    Reply reply;
    if (credentials.uid != allowed_uid_) {
        reply = {false, "unauthorized_peer", {}};
    } else {
        try {
            reply = handler_(request, credentials.uid, credentials.gid);
        } catch (...) {
            reply = {false, "handler_error", {}};
        }
    }
    const std::string response = reply_message(reply);
    (void)send_message(client_fd, response);
}

}  // namespace uhf::privileged
