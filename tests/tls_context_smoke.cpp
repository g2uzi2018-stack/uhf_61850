// SPDX-License-Identifier: GPL-3.0-only
#include "web/tls_context.hpp"

#include <filesystem>
#include <iostream>
#include <sys/stat.h>
#include <unistd.h>

namespace {

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "TLS context smoke failed: " << message << '\n';
        return false;
    }
    return true;
}

}  // namespace

int main() {
    const std::filesystem::path root = std::filesystem::temp_directory_path() /
        ("uhf-tls-context-" + std::to_string(static_cast<long long>(::getpid())));
    std::error_code cleanup_error;
    std::filesystem::remove_all(root, cleanup_error);
    try {
        const uhf::web::TlsFiles files{root / "tls" / "server.crt", root / "tls" / "server.key"};
        {
            uhf::web::TlsContext context(files);
            if (!expect(context.native() != nullptr, "TLS context created") ||
                !expect(std::filesystem::is_regular_file(files.certificate), "certificate generated") ||
                !expect(std::filesystem::is_regular_file(files.private_key), "private key generated")) {
                return 1;
            }
        }
        struct stat certificate_status {};
        struct stat key_status {};
        if (!expect(::stat(files.certificate.c_str(), &certificate_status) == 0, "certificate stat") ||
            !expect(::stat(files.private_key.c_str(), &key_status) == 0, "key stat") ||
            !expect((certificate_status.st_mode & 0777) == 0600, "certificate mode") ||
            !expect((key_status.st_mode & 0777) == 0600, "private key mode")) {
            return 1;
        }
        uhf::web::TlsContext reopened(files);
        if (!expect(reopened.native() != nullptr, "existing TLS files load") ||
            !expect(reopened.files().certificate == files.certificate, "certificate path retained")) {
            return 1;
        }
        std::filesystem::remove_all(root, cleanup_error);
        std::cout << "TLS context smoke: OK\n";
        return 0;
    } catch (const std::exception& error) {
        std::filesystem::remove_all(root, cleanup_error);
        std::cerr << "TLS context smoke failed: " << error.what() << '\n';
        return 1;
    }
}
