// SPDX-License-Identifier: GPL-3.0-only
#include "activation/activation_manager.hpp"
#include "activation/credential_file.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unistd.h>

namespace {

void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

}  // namespace

int main() try {
    const auto root = std::filesystem::temp_directory_path() /
        ("uhf-activation-" + std::to_string(static_cast<long long>(::getpid())));
    std::error_code error;
    std::filesystem::remove_all(root, error);

    const std::string identity = "board-serial-42";
    const std::string key = "host-test-key-never-a-production-secret";
    const std::string expected = uhf::activation::Manager::make_code(identity, key);
    check(expected.size() == 20U, "activation code length");
    check(uhf::activation::Manager::canonicalize_code(expected) == expected,
          "activation code canonicalization");

    uhf::activation::Options options;
    options.state_file = root / "activation.state";
    options.device_id = identity;
    options.manufacturer_key = key;
    options.requested_code = expected.substr(0U, 5U) + "-" + expected.substr(5U, 5U) +
        "-" + expected.substr(10U, 5U) + "-" + expected.substr(15U, 5U);
    uhf::activation::Manager activated(options);
    check(activated.active(), "valid activation is accepted");

    options.requested_code.reset();
    uhf::activation::Manager restarted(options);
    check(restarted.active(), "persisted activation is rechecked");

    options.requested_code = "AAAAA-AAAAA-AAAAA-AAAAA";
    uhf::activation::Manager rejected(options);
    check(!rejected.active(), "invalid activation is rejected");

    options.requested_code = expected.substr(0U, expected.size() - 1U) +
        (expected.back() == 'A' ? "B" : "A");
    uhf::activation::Manager late_mismatch(options);
    check(!late_mismatch.active(), "late activation mismatch is rejected");

    options.requested_code = expected + "A";
    uhf::activation::Manager wrong_length(options);
    check(!wrong_length.active(), "wrong activation length is rejected");

    options.requested_code.reset();
    options.device_id = "different-board";
    uhf::activation::Manager mismatch(options);
    check(!mismatch.active(), "device binding mismatch is rejected");

    options.device_id = identity;
    options.manufacturer_key.clear();
    uhf::activation::Manager missing_key(options);
    check(!missing_key.active(), "missing manufacturer key is rejected");

    std::filesystem::create_directories(root, error);
    const auto credential = root / "manufacturer.key";
    {
        std::ofstream output(credential);
        output << key << "\r\n";
    }
    std::filesystem::permissions(
        credential, std::filesystem::perms::owner_read |
            std::filesystem::perms::owner_write |
            std::filesystem::perms::group_read,
        std::filesystem::perm_options::replace, error);
    std::string credential_error;
    const auto loaded = uhf::activation::read_credential_file(
        credential, credential_error);
    check(loaded && *loaded == key && credential_error.empty(),
          "protected credential file is loaded and line ending is stripped");

    std::filesystem::permissions(
        credential, std::filesystem::perms::owner_read |
            std::filesystem::perms::owner_write |
            std::filesystem::perms::others_read,
        std::filesystem::perm_options::replace, error);
    check(!uhf::activation::read_credential_file(credential, credential_error) &&
              credential_error.find("unsafe") != std::string::npos,
          "world-readable credential is rejected");
    std::filesystem::permissions(
        credential, std::filesystem::perms::owner_read |
            std::filesystem::perms::owner_write |
            std::filesystem::perms::group_read,
        std::filesystem::perm_options::replace, error);
    const auto link = root / "manufacturer-link.key";
    std::filesystem::create_symlink(credential, link, error);
    check(!uhf::activation::read_credential_file(link, credential_error),
          "credential symlink is rejected");

    std::filesystem::remove_all(root, error);
    return 0;
} catch (const std::exception& exception) {
    std::cerr << exception.what() << '\n';
    return 1;
}
