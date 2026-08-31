// SPDX-License-Identifier: GPL-3.0-only
#include "platform/privileged/network_service.hpp"

#include <charconv>
#include <cstdint>
#include <iostream>
#include <limits>
#include <pwd.h>
#include <string>
#include <string_view>
#include <unistd.h>

namespace {

struct Options {
    std::string socket_path{"/run/uhf-gateway/privileged.sock"};
    std::string network_file{"/etc/uhf-gateway/network.json"};
    std::string transaction_file{"/var/lib/uhf-privileged/network-transaction.json"};
    uid_t allowed_uid{0U};
    bool allowed_uid_explicit{false};
};

bool parse_uid(std::string_view text, uid_t& value) {
    std::uint64_t parsed = 0U;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), parsed);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size() ||
        parsed > std::numeric_limits<uid_t>::max()) {
        return false;
    }
    value = static_cast<uid_t>(parsed);
    return true;
}

void usage() {
    std::cerr << "usage: uhf-privilegedd [--socket PATH] [--network-config PATH] "
                 "[--transaction PATH] [--allowed-uid UID]\n";
}

}  // namespace

int main(int argc, char* argv[]) {
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument = argv[index];
        if (argument == "--socket" && index + 1 < argc) {
            options.socket_path = argv[++index];
        } else if (argument == "--network-config" && index + 1 < argc) {
            options.network_file = argv[++index];
        } else if (argument == "--transaction" && index + 1 < argc) {
            options.transaction_file = argv[++index];
        } else if (argument == "--allowed-uid" && index + 1 < argc) {
            if (!parse_uid(argv[++index], options.allowed_uid)) {
                usage();
                return 2;
            }
            options.allowed_uid_explicit = true;
        } else {
            usage();
            return 2;
        }
    }
    if (!options.allowed_uid_explicit) {
        const passwd* gateway_user = ::getpwnam("uhfgateway");
        options.allowed_uid = gateway_user == nullptr ? ::getuid() : gateway_user->pw_uid;
    }

    try {
        uhf::privileged::NetworkService service(options.network_file, options.transaction_file);
        const uhf::network::TransactionResult recovery = service.recover_pending(true);
        if (recovery == uhf::network::TransactionResult::corrupt ||
            recovery == uhf::network::TransactionResult::backend_error ||
            recovery == uhf::network::TransactionResult::storage_error) {
            std::cerr << "network transaction recovery failed\n";
            return 1;
        }
        uhf::privileged::UnixSocketServer server(
            options.socket_path,
            options.allowed_uid,
            [&service](std::string_view request, uid_t uid, gid_t gid) {
                return service.handle(request, uid, gid);
            });
        service.start_rollback_monitor();
        const int result = server.run();
        service.stop_rollback_monitor();
        return result;
    } catch (const std::exception& error) {
        std::cerr << "unable to start privileged service: " << error.what() << '\n';
        return 1;
    }
}
