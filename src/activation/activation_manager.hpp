// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace uhf::activation {

struct Options {
    std::filesystem::path state_file;
    std::string device_id;
    std::string manufacturer_key;
    std::optional<std::string> requested_code;
};

class Manager {
public:
    explicit Manager(Options options);

    bool active() const noexcept;
    bool activate(std::string_view code);
    const std::string& expected_code() const noexcept;

    static std::string canonicalize_code(std::string_view code);
    static std::string make_code(std::string_view device_id,
                                 std::string_view manufacturer_key);

private:
    bool valid_identity() const noexcept;
    bool verify_persisted() const;
    bool persist(std::string_view code) const;

    Options options_;
    std::string expected_code_;
    bool active_{false};
};

}  // namespace uhf::activation
