// SPDX-License-Identifier: GPL-3.0-only
#include "platform/systemd/notify.hpp"

#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace uhf::systemd {

bool notify(std::string_view message) noexcept {
    const char* socket_name = std::getenv("NOTIFY_SOCKET");
    if (socket_name == nullptr || socket_name[0] == '\0' || message.empty()) {
        return true;
    }

    const std::string_view name(socket_name);
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    socklen_t address_length = 0U;
    if (name.front() == '@') {
        const std::string_view abstract_name = name.substr(1U);
        if (abstract_name.empty() || abstract_name.size() + 1U >= sizeof(address.sun_path)) {
            return false;
        }
        std::memcpy(address.sun_path + 1, abstract_name.data(), abstract_name.size());
        address_length = static_cast<socklen_t>(
            offsetof(sockaddr_un, sun_path) + 1U + abstract_name.size());
    } else {
        if (name.size() >= sizeof(address.sun_path)) {
            return false;
        }
        std::memcpy(address.sun_path, name.data(), name.size());
        address_length = static_cast<socklen_t>(
            offsetof(sockaddr_un, sun_path) + name.size() + 1U);
    }

    const int descriptor = ::socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (descriptor < 0) {
        return false;
    }
    const ssize_t sent = ::sendto(
        descriptor,
        message.data(),
        message.size(),
        MSG_NOSIGNAL,
        reinterpret_cast<const sockaddr*>(&address),
        address_length);
    (void)::close(descriptor);
    return sent == static_cast<ssize_t>(message.size());
}

}  // namespace uhf::systemd
