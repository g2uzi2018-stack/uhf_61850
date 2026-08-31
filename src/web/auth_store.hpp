// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace uhf::web {

enum class PasswordChangeResult {
    changed,
    invalid_current_password,
    invalid_new_password,
    storage_error,
};

class AuthStore {
public:
    struct Record {
        std::uint32_t iterations{0};
        std::vector<unsigned char> salt;
        std::vector<unsigned char> password_hash;
        bool must_change{false};
    };

    explicit AuthStore(std::filesystem::path state_directory);

    bool verify_password(std::string_view username, std::string_view password) const;
    PasswordChangeResult change_password(
        std::string_view current_password, std::string_view new_password);

    bool must_change() const noexcept;
    const std::filesystem::path& bootstrap_password_path() const noexcept;

private:
    std::filesystem::path state_directory_;
    std::filesystem::path auth_path_;
    std::filesystem::path bootstrap_path_;
    Record record_;
};

}  // namespace uhf::web
