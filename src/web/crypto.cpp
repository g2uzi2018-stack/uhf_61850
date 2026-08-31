// SPDX-License-Identifier: GPL-3.0-only
#include "web/crypto.hpp"

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

#include <limits>
#include <stdexcept>

namespace {

constexpr char kHexDigits[] = "0123456789abcdef";

}  // namespace

namespace uhf::web {

std::string random_hex(std::size_t byte_count) {
    if (byte_count == 0 || byte_count > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        throw std::invalid_argument("random byte count is out of range");
    }

    std::vector<unsigned char> bytes(byte_count);
    if (RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) != 1) {
        throw std::runtime_error("secure random generation failed");
    }

    std::string result;
    result.reserve(bytes.size() * 2U);
    for (const unsigned char byte : bytes) {
        result.push_back(kHexDigits[byte >> 4U]);
        result.push_back(kHexDigits[byte & 0x0FU]);
    }
    return result;
}

std::vector<unsigned char> pbkdf2_sha256(
    std::string_view password,
    const std::vector<unsigned char>& salt,
    std::uint32_t iterations) {
    if (password.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
        salt.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
        iterations == 0 ||
        iterations > static_cast<std::uint32_t>(std::numeric_limits<int>::max())) {
        throw std::invalid_argument("PBKDF2 input is out of range");
    }

    const int digest_size = EVP_MD_size(EVP_sha256());
    if (digest_size <= 0) {
        throw std::runtime_error("SHA-256 digest is unavailable");
    }
    std::vector<unsigned char> result(static_cast<std::size_t>(digest_size));
    const int derived = PKCS5_PBKDF2_HMAC(
        password.data(),
        static_cast<int>(password.size()),
        salt.data(),
        static_cast<int>(salt.size()),
        static_cast<int>(iterations),
        EVP_sha256(),
        static_cast<int>(result.size()),
        result.data());
    if (derived != 1) {
        throw std::runtime_error("PBKDF2 password derivation failed");
    }
    return result;
}

bool constant_time_equal(std::string_view left, std::string_view right) noexcept {
    if (left.size() != right.size()) {
        return false;
    }
    if (left.empty()) {
        return true;
    }
    return CRYPTO_memcmp(left.data(), right.data(), left.size()) == 0;
}

}  // namespace uhf::web
