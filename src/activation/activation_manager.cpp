// SPDX-License-Identifier: GPL-3.0-only
#include "activation/activation_manager.hpp"

#include <array>
#include <cctype>
#include <cerrno>
#include <fcntl.h>
#include <fstream>
#include <limits>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <system_error>
#include <unistd.h>
#include <utility>

namespace {

constexpr std::string_view kAlphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";
constexpr std::size_t kDigestBytes = 12U;
constexpr mode_t kFileMode = S_IRUSR | S_IWUSR;
constexpr mode_t kDirectoryMode = S_IRWXU;

std::string base32(std::string_view bytes) {
    std::string result;
    result.reserve((bytes.size() * 8U + 4U) / 5U);
    std::uint32_t buffer = 0U;
    std::size_t bits = 0U;
    for (const char character : bytes) {
        const auto value = static_cast<unsigned char>(character);
        buffer = (buffer << 8U) | value;
        bits += 8U;
        while (bits >= 5U) {
            bits -= 5U;
            result.push_back(kAlphabet[(buffer >> bits) & 0x1FU]);
        }
    }
    if (bits != 0U) {
        result.push_back(kAlphabet[(buffer << (5U - bits)) & 0x1FU]);
    }
    return result;
}

bool safe_field(std::string_view value) noexcept {
    if (value.empty() || value.size() > 256U) return false;
    for (const char raw_character : value) {
        const auto character = static_cast<unsigned char>(raw_character);
        if (character == '\n' || character == '\r' || character == '\0') return false;
    }
    return true;
}

bool constant_time_equal(std::string_view left, std::string_view right) noexcept {
    // Activation codes have a public, fixed length.  Check that first, then
    // use OpenSSL's constant-time byte comparison for the secret-dependent
    // part instead of std::string's early-exit equality operator.
    return left.size() == right.size() &&
        CRYPTO_memcmp(left.data(), right.data(), left.size()) == 0;
}

bool atomic_write(const std::filesystem::path& path, std::string_view contents) noexcept {
    std::error_code error;
    if (path.empty() || std::filesystem::is_symlink(path, error)) return false;
    if (error && error != std::make_error_code(std::errc::no_such_file_or_directory)) return false;
    error.clear();
    const std::filesystem::path parent = path.parent_path().empty() ? "." : path.parent_path();
    std::filesystem::create_directories(parent, error);
    if (error || ::chmod(parent.c_str(), kDirectoryMode) != 0) return false;
    const std::filesystem::path temporary = path.string() + ".tmp";
    const int fd = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, kFileMode);
    if (fd < 0) return false;
    std::size_t written = 0U;
    while (written < contents.size()) {
        const ssize_t result = ::write(fd, contents.data() + written, contents.size() - written);
        if (result <= 0) {
            ::close(fd);
            ::unlink(temporary.c_str());
            return false;
        }
        written += static_cast<std::size_t>(result);
    }
    const bool synced = ::fchmod(fd, kFileMode) == 0 && ::fsync(fd) == 0 && ::close(fd) == 0;
    if (!synced || ::rename(temporary.c_str(), path.c_str()) != 0) {
        ::unlink(temporary.c_str());
        return false;
    }
    const int parent_fd = ::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (parent_fd < 0) return false;
    const bool parent_synced = ::fsync(parent_fd) == 0;
    ::close(parent_fd);
    return parent_synced;
}

}  // namespace

namespace uhf::activation {

std::string Manager::canonicalize_code(std::string_view code) {
    std::string canonical;
    canonical.reserve(code.size());
    for (const char raw_character : code) {
        const auto character = static_cast<unsigned char>(raw_character);
        if (character == '-' || std::isspace(character) != 0) continue;
        canonical.push_back(static_cast<char>(std::toupper(character)));
    }
    return canonical;
}

std::string Manager::make_code(std::string_view device_id, std::string_view manufacturer_key) {
    if (!safe_field(device_id) || !safe_field(manufacturer_key) ||
        device_id.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
        manufacturer_key.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return {};
    }
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int digest_length = 0U;
    if (HMAC(EVP_sha256(), manufacturer_key.data(), static_cast<int>(manufacturer_key.size()),
             reinterpret_cast<const unsigned char*>(device_id.data()), device_id.size(),
             digest.data(), &digest_length) == nullptr || digest_length < kDigestBytes) {
        return {};
    }
    return base32(std::string_view(reinterpret_cast<const char*>(digest.data()), kDigestBytes));
}

Manager::Manager(Options options) : options_(std::move(options)) {
    expected_code_ = make_code(options_.device_id, options_.manufacturer_key);
    if (options_.requested_code) {
        active_ = activate(*options_.requested_code);
    } else {
        active_ = valid_identity() && verify_persisted();
    }
}

bool Manager::valid_identity() const noexcept {
    return safe_field(options_.device_id) && !options_.manufacturer_key.empty() &&
        !expected_code_.empty() && options_.state_file.has_filename();
}

bool Manager::verify_persisted() const {
    std::error_code error;
    if (!valid_identity() || std::filesystem::is_symlink(options_.state_file, error) || error ||
        !std::filesystem::is_regular_file(options_.state_file, error) || error) {
        return false;
    }
    if (std::filesystem::file_size(options_.state_file, error) > 1024U || error) return false;
    std::ifstream input(options_.state_file);
    std::string line;
    std::string identity;
    std::string code;
    while (std::getline(input, line)) {
        if (line.rfind("device_id=", 0U) == 0U) identity = line.substr(10U);
        if (line.rfind("activation_code=", 0U) == 0U) code = line.substr(16U);
    }
    return identity == options_.device_id &&
        canonicalize_code(code) == expected_code_;
}

bool Manager::persist(std::string_view code) const {
    const std::string contents = "version=1\ndevice_id=" + options_.device_id +
        "\nactivation_code=" + std::string(code) + "\n";
    return atomic_write(options_.state_file, contents);
}

bool Manager::activate(std::string_view code) {
    const std::string canonical = canonicalize_code(code);
    if (!valid_identity() || !constant_time_equal(canonical, expected_code_) ||
        !persist(canonical)) {
        active_ = false;
        return false;
    }
    active_ = true;
    return true;
}

bool Manager::active() const noexcept { return active_; }

const std::string& Manager::expected_code() const noexcept { return expected_code_; }

}  // namespace uhf::activation
