// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace uhf::web {

constexpr std::size_t kMaxIcdBytes = 48U * 1024U;

struct IcdStatus {
    bool available{false};
    bool override_active{false};
    bool previous_available{false};
    std::size_t size{0U};
    std::string sha256;
    std::string ied_name;
};

enum class IcdReplaceResult {
    replaced,
    invalid,
    storage_error,
};

enum class IcdRestoreResult {
    restored,
    no_override,
    storage_error,
};

class IcdStore {
public:
    explicit IcdStore(
        std::filesystem::path override_path,
        std::filesystem::path packaged_path = "/etc/uhf-gateway/UHFPD1.icd");

    IcdStatus status() const;
    std::optional<std::string> read_current() const;
    std::optional<std::string> read_previous() const;
    IcdReplaceResult replace(std::string_view contents) const;
    IcdRestoreResult restore() const;

    static bool validate(std::string_view contents, std::string& ied_name) noexcept;
    static std::string sha256(std::string_view contents);

private:
    std::optional<std::string> read_file(const std::filesystem::path& path) const;
    bool write_atomic(const std::filesystem::path& path, std::string_view contents) const noexcept;

    std::filesystem::path override_path_;
    std::filesystem::path packaged_path_;
    std::filesystem::path previous_path_;
};

}  // namespace uhf::web
