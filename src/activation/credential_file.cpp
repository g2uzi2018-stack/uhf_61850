// SPDX-License-Identifier: GPL-3.0-only
#include "activation/credential_file.hpp"

#include <array>
#include <cerrno>
#include <fcntl.h>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>

namespace uhf::activation {
namespace {

constexpr std::size_t kMaximumCredentialBytes = 4096U;

std::optional<std::string> fail(std::string& error, std::string_view message) {
    error.assign(message);
    return std::nullopt;
}

}  // namespace

std::optional<std::string> read_credential_file(
    const std::filesystem::path& path, std::string& error) {
    error.clear();
    if (path.empty()) {
        return fail(error, "credential path is empty");
    }
    const int descriptor = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (descriptor < 0) {
        return fail(error, "credential file cannot be opened");
    }
    struct stat status {};
    if (::fstat(descriptor, &status) != 0 || !S_ISREG(status.st_mode)) {
        ::close(descriptor);
        return fail(error, "credential path is not a regular file");
    }
    if ((status.st_mode & (S_IWGRP | S_IROTH | S_IWOTH)) != 0 ||
        (status.st_uid != 0U && status.st_uid != ::geteuid())) {
        ::close(descriptor);
        return fail(error, "credential file permissions or owner are unsafe");
    }

    std::string value;
    value.reserve(256U);
    std::array<char, 512U> buffer{};
    while (true) {
        const ssize_t count = ::read(descriptor, buffer.data(), buffer.size());
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count < 0) {
            ::close(descriptor);
            return fail(error, "credential file cannot be read");
        }
        if (count == 0) {
            break;
        }
        const std::size_t bytes = static_cast<std::size_t>(count);
        if (bytes > kMaximumCredentialBytes - value.size()) {
            ::close(descriptor);
            return fail(error, "credential file is too large");
        }
        value.append(buffer.data(), bytes);
    }
    if (::close(descriptor) != 0) {
        return fail(error, "credential file cannot be closed");
    }
    while (!value.empty() && (value.back() == '\n' || value.back() == '\r')) {
        value.pop_back();
    }
    if (value.empty() || value.size() > 256U ||
        value.find_first_of("\r\n\0", 0U, 3U) != std::string::npos) {
        return fail(error, "credential file must contain one non-empty line");
    }
    return value;
}

}  // namespace uhf::activation
