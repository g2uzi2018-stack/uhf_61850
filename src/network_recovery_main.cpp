// SPDX-License-Identifier: GPL-3.0-only
#include "platform/privileged/network_service.hpp"

#include <iostream>
#include <string_view>

namespace {

struct Options {
    std::string_view network_file{"/etc/uhf-gateway/network.json"};
    std::string_view transaction_file{"/var/lib/uhf-privileged/network-transaction.json"};
};

bool parse_options(int argc, char* argv[], Options& options) {
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument = argv[index];
        if (argument == "--network-config" && index + 1 < argc) {
            options.network_file = argv[++index];
        } else if (argument == "--transaction" && index + 1 < argc) {
            options.transaction_file = argv[++index];
        } else {
            return false;
        }
    }
    return true;
}

}  // namespace

int main(int argc, char* argv[]) {
    Options options;
    if (!parse_options(argc, argv, options)) {
        std::cerr << "usage: uhf-network-recovery [--network-config PATH] [--transaction PATH]\n";
        return 2;
    }
    try {
        uhf::privileged::NetworkService service(
            std::string(options.network_file), std::string(options.transaction_file));
        const uhf::network::TransactionResult result = service.recover_pending();
        if (result == uhf::network::TransactionResult::no_transaction ||
            result == uhf::network::TransactionResult::not_due ||
            result == uhf::network::TransactionResult::ok ||
            result == uhf::network::TransactionResult::expired ||
            result == uhf::network::TransactionResult::boot_changed) {
            return 0;
        }
        std::cerr << "network recovery failed: "
                  << uhf::network::transaction_result_name(result) << '\n';
        return 1;
    } catch (const std::exception& error) {
        std::cerr << "network recovery unavailable: " << error.what() << '\n';
        return 1;
    }
}
