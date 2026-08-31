// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <filesystem>

typedef struct ssl_ctx_st SSL_CTX;

namespace uhf::web {

struct TlsFiles {
    std::filesystem::path certificate;
    std::filesystem::path private_key;
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

private:
    TlsFiles files_;
    SSL_CTX* context_{nullptr};
};

}  // namespace uhf::web
