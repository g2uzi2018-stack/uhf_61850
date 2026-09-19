// SPDX-License-Identifier: GPL-3.0-only
#include "platform/privileged/unix_socket.hpp"
#include "test_check.hpp"

#include <chrono>
#include <filesystem>
#include <string>
#include <unistd.h>
#include <thread>

int main() {
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() /
        ("uhf-privileged-socket-smoke-" +
         std::to_string(static_cast<long long>(::getpid()))) /
        "helper.sock";
    std::error_code ignored;
    std::filesystem::remove_all(path.parent_path(), ignored);
    uhf::privileged::UnixSocketServer server(
        path,
        ::getuid(),
        [](std::string_view request, uid_t uid, gid_t gid) {
            UHF_TEST_CHECK(uid == ::getuid());
            UHF_TEST_CHECK(gid == ::getgid());
            if (request == "ping") {
                return uhf::privileged::Reply{true, "pong", "ready"};
            }
            return uhf::privileged::Reply{false, "rejected", "fixed commands only"};
        });
    std::thread worker([&server] { UHF_TEST_CHECK(server.run() == 0); });
    for (int attempt = 0; attempt < 20; ++attempt) {
        if (std::filesystem::exists(path)) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    uhf::privileged::UnixSocketClient client(path);
    const uhf::privileged::Reply success = client.request("ping");
    UHF_TEST_CHECK(success.ok && success.code == "pong" && success.body == "ready");
    const uhf::privileged::Reply rejected = client.request("exec rm -rf /");
    UHF_TEST_CHECK(!rejected.ok && rejected.code == "rejected");
    const std::string oversized(uhf::privileged::kMaxMessageBytes + 1U, 'x');
    const uhf::privileged::Reply oversized_reply = client.request(oversized);
    UHF_TEST_CHECK(!oversized_reply.ok && oversized_reply.code == "message_too_large");

    server.stop();
    worker.join();
    std::filesystem::remove_all(path.parent_path(), ignored);
    return 0;
}
