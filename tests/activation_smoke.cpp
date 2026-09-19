// SPDX-License-Identifier: GPL-3.0-only
#include "activation/activation_manager.hpp"

#include <cassert>
#include <filesystem>
#include <string>
#include <unistd.h>

int main() {
    const auto root = std::filesystem::temp_directory_path() /
        ("uhf-activation-" + std::to_string(static_cast<long long>(::getpid())));
    std::error_code error;
    std::filesystem::remove_all(root, error);

    const std::string identity = "board-serial-42";
    const std::string key = "host-test-key-never-a-production-secret";
    const std::string expected = uhf::activation::Manager::make_code(identity, key);
    assert(expected.size() == 20U);
    assert(uhf::activation::Manager::canonicalize_code(expected) == expected);

    uhf::activation::Options options;
    options.state_file = root / "activation.state";
    options.device_id = identity;
    options.manufacturer_key = key;
    options.requested_code = expected.substr(0U, 5U) + "-" + expected.substr(5U, 5U) +
        "-" + expected.substr(10U, 5U) + "-" + expected.substr(15U, 5U);
    uhf::activation::Manager activated(options);
    assert(activated.active());

    options.requested_code.reset();
    uhf::activation::Manager restarted(options);
    assert(restarted.active());

    options.requested_code = "AAAAA-AAAAA-AAAAA-AAAAA";
    uhf::activation::Manager rejected(options);
    assert(!rejected.active());

    options.requested_code.reset();
    options.device_id = "different-board";
    uhf::activation::Manager mismatch(options);
    assert(!mismatch.active());

    options.device_id = identity;
    options.manufacturer_key.clear();
    uhf::activation::Manager missing_key(options);
    assert(!missing_key.active());

    std::filesystem::remove_all(root, error);
    return 0;
}
