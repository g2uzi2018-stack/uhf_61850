// SPDX-License-Identifier: GPL-3.0-only
#include "web/auth_store.hpp"

#include "web/crypto.hpp"

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <utility>

namespace {

constexpr std::uint32_t kPasswordIterations = 210000;
constexpr std::size_t kSaltBytes = 16;
constexpr std::size_t kPasswordHashBytes = 32;
constexpr std::size_t kMaxPasswordBytes = 256;
constexpr mode_t kPrivateFileMode = S_IRUSR | S_IWUSR;
constexpr mode_t kPrivateDirectoryMode = S_IRWXU;

int hex_value(char value) {
    if (value >= '0' && value <= '9') {
        return value - '0';
    }
    if (value >= 'a' && value <= 'f') {
        return value - 'a' + 10;
    }
    if (value >= 'A' && value <= 'F') {
        return value - 'A' + 10;
    }
    return -1;
}

std::string hex_encode(const std::vector<unsigned char>& bytes) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(bytes.size() * 2U);
    for (const unsigned char byte : bytes) {
        result.push_back(digits[byte >> 4U]);
        result.push_back(digits[byte & 0x0FU]);
    }
    return result;
}

bool hex_decode(std::string_view encoded, std::vector<unsigned char>& bytes) {
    if (encoded.empty() || encoded.size() % 2U != 0U) {
        return false;
    }

    bytes.clear();
    bytes.reserve(encoded.size() / 2U);
    for (std::size_t index = 0; index < encoded.size(); index += 2U) {
        const int high = hex_value(encoded[index]);
        const int low = hex_value(encoded[index + 1U]);
        if (high < 0 || low < 0) {
            return false;
        }
        bytes.push_back(static_cast<unsigned char>((high << 4) | low));
    }
    return true;
}

std::string trim(std::string_view value) {
    std::size_t begin = 0;
    while (begin < value.size() &&
           (value[begin] == ' ' || value[begin] == '\t' || value[begin] == '\r' ||
            value[begin] == '\n')) {
        ++begin;
    }
    std::size_t end = value.size();
    while (end > begin &&
           (value[end - 1U] == ' ' || value[end - 1U] == '\t' || value[end - 1U] == '\r' ||
            value[end - 1U] == '\n')) {
        --end;
    }
    return std::string(value.substr(begin, end - begin));
}

std::optional<std::string> json_string_field(std::string_view json, std::string_view key) {
    const std::string marker = "\"" + std::string(key) + "\"";
    const std::size_t marker_position = json.find(marker);
    if (marker_position == std::string_view::npos) {
        return std::nullopt;
    }

    std::size_t position = marker_position + marker.size();
    while (position < json.size() &&
           (json[position] == ' ' || json[position] == '\t' || json[position] == '\r' ||
            json[position] == '\n')) {
        ++position;
    }
    if (position >= json.size() || json[position] != ':') {
        return std::nullopt;
    }
    ++position;
    while (position < json.size() &&
           (json[position] == ' ' || json[position] == '\t' || json[position] == '\r' ||
            json[position] == '\n')) {
        ++position;
    }
    if (position >= json.size() || json[position] != '"') {
        return std::nullopt;
    }
    ++position;

    std::string value;
    while (position < json.size()) {
        const char character = json[position++];
        if (character == '"') {
            return value;
        }
        if (character == '\\' || static_cast<unsigned char>(character) < 0x20U) {
            return std::nullopt;
        }
        value.push_back(character);
    }
    return std::nullopt;
}

std::optional<std::uint32_t> json_uint_field(std::string_view json, std::string_view key) {
    const std::string marker = "\"" + std::string(key) + "\"";
    const std::size_t marker_position = json.find(marker);
    if (marker_position == std::string_view::npos) {
        return std::nullopt;
    }

    std::size_t position = marker_position + marker.size();
    while (position < json.size() &&
           (json[position] == ' ' || json[position] == '\t' || json[position] == '\r' ||
            json[position] == '\n')) {
        ++position;
    }
    if (position >= json.size() || json[position] != ':') {
        return std::nullopt;
    }
    ++position;
    while (position < json.size() &&
           (json[position] == ' ' || json[position] == '\t' || json[position] == '\r' ||
            json[position] == '\n')) {
        ++position;
    }
    if (position >= json.size() || json[position] < '0' || json[position] > '9') {
        return std::nullopt;
    }

    std::uint64_t result = 0;
    while (position < json.size() && json[position] >= '0' && json[position] <= '9') {
        result = result * 10U + static_cast<std::uint64_t>(json[position] - '0');
        if (result > std::numeric_limits<std::uint32_t>::max()) {
            return std::nullopt;
        }
        ++position;
    }
    return static_cast<std::uint32_t>(result);
}

std::optional<bool> json_bool_field(std::string_view json, std::string_view key) {
    const std::string marker = "\"" + std::string(key) + "\"";
    const std::size_t marker_position = json.find(marker);
    if (marker_position == std::string_view::npos) {
        return std::nullopt;
    }

    std::size_t position = marker_position + marker.size();
    while (position < json.size() &&
           (json[position] == ' ' || json[position] == '\t' || json[position] == '\r' ||
            json[position] == '\n')) {
        ++position;
    }
    if (position >= json.size() || json[position] != ':') {
        return std::nullopt;
    }
    ++position;
    while (position < json.size() &&
           (json[position] == ' ' || json[position] == '\t' || json[position] == '\r' ||
            json[position] == '\n')) {
        ++position;
    }
    if (json.substr(position, 4U) == "true") {
        return true;
    }
    if (json.substr(position, 5U) == "false") {
        return false;
    }
    return std::nullopt;
}

std::string read_file(const std::filesystem::path& path, std::size_t max_bytes) {
    std::error_code error;
    const std::uintmax_t file_size = std::filesystem::file_size(path, error);
    if (error || file_size > max_bytes) {
        throw std::runtime_error("authentication state is too large");
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("unable to read authentication state");
    }
    std::ostringstream contents;
    contents << input.rdbuf();
    if (input.bad()) {
        throw std::runtime_error("unable to read authentication state");
    }
    std::string result = contents.str();
    if (result.size() > max_bytes) {
        throw std::runtime_error("authentication state is too large");
    }
    return result;
}

void fsync_parent(const std::filesystem::path& path) {
    const std::filesystem::path parent = path.parent_path().empty() ? "." : path.parent_path();
    const int directory_fd = ::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (directory_fd < 0) {
        throw std::runtime_error("unable to sync authentication directory");
    }
    const int result = ::fsync(directory_fd);
    const int saved_errno = errno;
    ::close(directory_fd);
    if (result < 0) {
        errno = saved_errno;
        throw std::runtime_error("unable to sync authentication directory");
    }
}

void write_atomic(const std::filesystem::path& path, std::string_view contents, mode_t mode) {
    const std::filesystem::path temporary = path.string() + ".tmp";
    const int file_fd = ::open(
        temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode);
    if (file_fd < 0) {
        throw std::runtime_error("unable to write authentication state");
    }
    bool file_open = true;
    bool complete = false;
    try {
        if (::fchmod(file_fd, mode) < 0) {
            throw std::runtime_error("unable to protect authentication state");
        }
        std::size_t written = 0;
        while (written < contents.size()) {
            const ssize_t result = ::write(
                file_fd, contents.data() + written, contents.size() - written);
            if (result <= 0) {
                throw std::runtime_error("unable to write authentication state");
            }
            written += static_cast<std::size_t>(result);
        }
        if (::fsync(file_fd) < 0) {
            throw std::runtime_error("unable to sync authentication state");
        }
        if (::close(file_fd) < 0) {
            file_open = false;
            throw std::runtime_error("unable to close authentication state");
        }
        file_open = false;
        if (::rename(temporary.c_str(), path.c_str()) < 0) {
            throw std::runtime_error("unable to replace authentication state");
        }
        fsync_parent(path);
        complete = true;
    } catch (...) {
        if (!complete) {
            if (file_open) {
                ::close(file_fd);
            }
            ::unlink(temporary.c_str());
        }
        throw;
    }
}

void remove_file_best_effort(const std::filesystem::path& path) noexcept {
    if (::unlink(path.c_str()) == 0 || errno == ENOENT) {
        try {
            fsync_parent(path);
        } catch (...) {
        }
    }
}

void ensure_private_directory(const std::filesystem::path& path) {
    std::error_code error;
    std::filesystem::create_directories(path, error);
    if (error || !std::filesystem::is_directory(path, error) || error ||
        ::chmod(path.c_str(), kPrivateDirectoryMode) < 0) {
        throw std::runtime_error("unable to prepare authentication state directory");
    }
}

void ensure_private_file(const std::filesystem::path& path) {
    if (::chmod(path.c_str(), kPrivateFileMode) < 0) {
        throw std::runtime_error("unable to protect authentication state");
    }
}

uhf::web::AuthStore::Record load_record(const std::filesystem::path& path) {
    const std::string json = read_file(path, 4096U);
    const std::optional<std::string> username = json_string_field(json, "username");
    const std::optional<std::string> algorithm = json_string_field(json, "algorithm");
    const std::optional<std::string> salt_hex = json_string_field(json, "salt_hex");
    const std::optional<std::string> hash_hex = json_string_field(json, "hash_hex");
    const std::optional<std::uint32_t> iterations = json_uint_field(json, "iterations");
    const std::optional<bool> must_change = json_bool_field(json, "must_change");
    if (!username || !algorithm || !salt_hex || !hash_hex || !iterations || !must_change ||
        *username != "admin" || *algorithm != "PBKDF2-HMAC-SHA256" ||
        *iterations < 100000U || *iterations > 1000000U) {
        throw std::runtime_error("authentication state is invalid");
    }

    uhf::web::AuthStore::Record record;
    record.iterations = *iterations;
    record.must_change = *must_change;
    if (!hex_decode(*salt_hex, record.salt) || !hex_decode(*hash_hex, record.password_hash) ||
        record.salt.size() != kSaltBytes || record.password_hash.size() != kPasswordHashBytes) {
        throw std::runtime_error("authentication state is invalid");
    }
    return record;
}

std::string serialize_record(const uhf::web::AuthStore::Record& record) {
    return "{\"version\":1,\"username\":\"admin\",\"algorithm\":\"PBKDF2-HMAC-SHA256\",\"iterations\":" +
        std::to_string(record.iterations) + ",\"salt_hex\":\"" + hex_encode(record.salt) +
        "\",\"hash_hex\":\"" + hex_encode(record.password_hash) +
        "\",\"must_change\":" + (record.must_change ? "true" : "false") + "}\n";
}

}  // namespace

namespace uhf::web {

AuthStore::AuthStore(std::filesystem::path state_directory)
    : state_directory_(std::move(state_directory)),
      auth_path_(state_directory_ / "auth.json"),
      bootstrap_path_(state_directory_ / "initial-password") {
    ensure_private_directory(state_directory_);
    std::error_code error;
    const bool auth_exists = std::filesystem::exists(auth_path_, error);
    if (error) {
        throw std::runtime_error("unable to inspect authentication state");
    }

    if (auth_exists) {
        record_ = load_record(auth_path_);
        ensure_private_file(auth_path_);
        if (std::filesystem::exists(bootstrap_path_, error) && !error) {
            ensure_private_file(bootstrap_path_);
        }
        return;
    }

    std::string bootstrap_password;
    const bool bootstrap_exists = std::filesystem::exists(bootstrap_path_, error);
    if (error) {
        throw std::runtime_error("unable to inspect authentication state");
    }
    if (bootstrap_exists) {
        bootstrap_password = trim(read_file(bootstrap_path_, kMaxPasswordBytes + 1U));
        ensure_private_file(bootstrap_path_);
        if (bootstrap_password.size() != 32U) {
            throw std::runtime_error("initial authentication state is invalid");
        }
    } else {
        bootstrap_password = random_hex(kSaltBytes);
        write_atomic(bootstrap_path_, bootstrap_password + "\n", kPrivateFileMode);
    }

    record_.iterations = kPasswordIterations;
    record_.salt.resize(kSaltBytes);
    const std::string salt_hex = random_hex(kSaltBytes);
    if (!hex_decode(salt_hex, record_.salt)) {
        throw std::runtime_error("unable to initialize authentication state");
    }
    record_.password_hash = pbkdf2_sha256(bootstrap_password, record_.salt, record_.iterations);
    record_.must_change = true;
    write_atomic(auth_path_, serialize_record(record_), kPrivateFileMode);
}

bool AuthStore::verify_password(std::string_view username, std::string_view password) const {
    if (username != "admin" || password.size() > kMaxPasswordBytes) {
        return false;
    }
    const std::vector<unsigned char> candidate =
        pbkdf2_sha256(password, record_.salt, record_.iterations);
    if (candidate.size() != record_.password_hash.size()) {
        return false;
    }
    return constant_time_equal(
        std::string_view(reinterpret_cast<const char*>(candidate.data()), candidate.size()),
        std::string_view(
            reinterpret_cast<const char*>(record_.password_hash.data()),
            record_.password_hash.size()));
}

PasswordChangeResult AuthStore::change_password(
    std::string_view current_password, std::string_view new_password) {
    if (new_password.size() < 12U || new_password.size() > kMaxPasswordBytes ||
        current_password.size() > kMaxPasswordBytes || new_password == current_password) {
        return PasswordChangeResult::invalid_new_password;
    }
    if (!verify_password("admin", current_password)) {
        return PasswordChangeResult::invalid_current_password;
    }

    Record updated;
    updated.iterations = record_.iterations;
    updated.salt.resize(kSaltBytes);
    const std::string salt_hex = random_hex(kSaltBytes);
    if (!hex_decode(salt_hex, updated.salt)) {
        return PasswordChangeResult::storage_error;
    }
    updated.password_hash = pbkdf2_sha256(new_password, updated.salt, updated.iterations);
    updated.must_change = false;

    try {
        write_atomic(auth_path_, serialize_record(updated), kPrivateFileMode);
    } catch (...) {
        return PasswordChangeResult::storage_error;
    }
    record_ = std::move(updated);
    remove_file_best_effort(bootstrap_path_);
    return PasswordChangeResult::changed;
}

bool AuthStore::must_change() const noexcept {
    return record_.must_change;
}

const std::filesystem::path& AuthStore::bootstrap_password_path() const noexcept {
    return bootstrap_path_;
}

}  // namespace uhf::web
