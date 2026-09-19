// SPDX-License-Identifier: GPL-3.0-only
#include "platform/systemd/notify.hpp"
#include "test_check.hpp"

#include <cstring>
#include <filesystem>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

int main() {
    const std::filesystem::path path = std::filesystem::temp_directory_path() /
        ("uhf-notify-" + std::to_string(static_cast<long long>(::getpid())) + ".sock");
    std::error_code ignored;
    std::filesystem::remove(path, ignored);

    const int receiver = ::socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    UHF_TEST_CHECK(receiver >= 0);
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    const std::string path_string = path.string();
    UHF_TEST_CHECK(path_string.size() < sizeof(address.sun_path));
    std::memcpy(address.sun_path, path_string.c_str(), path_string.size() + 1U);
    UHF_TEST_CHECK(::bind(
               receiver,
               reinterpret_cast<const sockaddr*>(&address),
               static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + path_string.size() + 1U)) == 0);
    UHF_TEST_CHECK(::setenv("NOTIFY_SOCKET", path_string.c_str(), 1) == 0);
    UHF_TEST_CHECK(uhf::systemd::notify("READY=1\nSTATUS=smoke"));
    pollfd descriptor{receiver, POLLIN, 0};
    UHF_TEST_CHECK(::poll(&descriptor, 1, 1000) == 1);
    char buffer[128]{};
    const ssize_t received = ::recv(receiver, buffer, sizeof(buffer), 0);
    UHF_TEST_CHECK(received == 20);
    UHF_TEST_CHECK(std::string(buffer, buffer + received) == "READY=1\nSTATUS=smoke");
    UHF_TEST_CHECK(::unsetenv("NOTIFY_SOCKET") == 0);
    UHF_TEST_CHECK(uhf::systemd::notify("WATCHDOG=1"));
    ::close(receiver);
    std::filesystem::remove(path, ignored);
    return 0;
}
