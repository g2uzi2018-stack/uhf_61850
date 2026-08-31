// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <filesystem>
#include <string>
#include <string_view>

typedef struct ssl_ctx_st SSL_CTX;
typedef struct ssl_st SSL;

namespace uhf::web {

struct TlsFiles {
    std::filesystem::path certificate;
    std::filesystem::path private_key;
};

std::string local_subject_alt_names();
void ensure_tls_files(const TlsFiles& files);

enum class TlsReplaceResult {
    replaced,
    invalid,
    storage_error,
};

class TlsContext {
public:
    explicit TlsContext(TlsFiles files);
    ~TlsContext();

    TlsContext(const TlsContext&) = delete;
    TlsContext& operator=(const TlsContext&) = delete;

    TlsContext(TlsContext&& other) noexcept;
    TlsContext& operator=(TlsContext&& other) noexcept;

    SSL_CTX* native() const noexcept;
    const TlsFiles& files() const noexcept;
    TlsReplaceResult replace(std::string_view certificate_pem, std::string_view private_key_pem);

private:
    TlsFiles files_;
    SSL_CTX* context_{nullptr};
};

}  // namespace uhf::web
