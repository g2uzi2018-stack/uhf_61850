// SPDX-License-Identifier: GPL-3.0-only
#include "web/tls_context.hpp"

#include <cerrno>
#include <cstddef>
#include <fcntl.h>
#include <filesystem>
#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace {

constexpr mode_t kPrivateDirectoryMode = S_IRWXU;
constexpr mode_t kPrivateFileMode = S_IRUSR | S_IWUSR;

void ensure_private_directory(const std::filesystem::path& path) {
    std::error_code error;
    std::filesystem::create_directories(path, error);
    if (error || !std::filesystem::is_directory(path, error) || error ||
        ::chmod(path.c_str(), kPrivateDirectoryMode) < 0) {
        throw std::runtime_error("unable to prepare TLS directory");
    }
}

void sync_parent(const std::filesystem::path& path) {
    const std::filesystem::path parent = path.parent_path().empty() ? "." : path.parent_path();
    const int directory_fd = ::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (directory_fd < 0) {
        throw std::runtime_error("unable to sync TLS directory");
    }
    const int result = ::fsync(directory_fd);
    const int saved_errno = errno;
    ::close(directory_fd);
    if (result < 0) {
        errno = saved_errno;
        throw std::runtime_error("unable to sync TLS directory");
    }
}

void write_atomic(
    const std::filesystem::path& path, const unsigned char* data, std::size_t size) {
    const std::filesystem::path temporary = path.string() + ".tmp";
    const int file_fd = ::open(
        temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW,
        kPrivateFileMode);
    if (file_fd < 0) {
        throw std::runtime_error("unable to write TLS file");
    }
    bool open = true;
    bool complete = false;
    try {
        if (::fchmod(file_fd, kPrivateFileMode) < 0) {
            throw std::runtime_error("unable to protect TLS file");
        }
        std::size_t written = 0U;
        while (written < size) {
            const ssize_t result = ::write(file_fd, data + written, size - written);
            if (result <= 0) {
                throw std::runtime_error("unable to write TLS file");
            }
            written += static_cast<std::size_t>(result);
        }
        if (::fsync(file_fd) < 0 || ::close(file_fd) < 0) {
            open = false;
            throw std::runtime_error("unable to sync TLS file");
        }
        open = false;
        if (::rename(temporary.c_str(), path.c_str()) < 0) {
            throw std::runtime_error("unable to replace TLS file");
        }
        sync_parent(path);
        complete = true;
    } catch (...) {
        if (open) {
            ::close(file_fd);
        }
        if (!complete) {
            ::unlink(temporary.c_str());
        }
        throw;
    }
}

std::string openssl_error(const char* fallback) {
    const unsigned long error = ERR_get_error();
    if (error == 0U) {
        return fallback;
    }
    char buffer[256]{};
    ERR_error_string_n(error, buffer, sizeof(buffer));
    return std::string(fallback) + ": " + buffer;
}

class OpenSslPointerGuard {
public:
    explicit OpenSslPointerGuard(EVP_PKEY* value) : value_(value) {}
    ~OpenSslPointerGuard() { EVP_PKEY_free(value_); }
    OpenSslPointerGuard(const OpenSslPointerGuard&) = delete;
    OpenSslPointerGuard& operator=(const OpenSslPointerGuard&) = delete;
    EVP_PKEY* get() const noexcept { return value_; }

private:
    EVP_PKEY* value_;
};

class X509PointerGuard {
public:
    explicit X509PointerGuard(X509* value) : value_(value) {}
    ~X509PointerGuard() { X509_free(value_); }
    X509PointerGuard(const X509PointerGuard&) = delete;
    X509PointerGuard& operator=(const X509PointerGuard&) = delete;
    X509* get() const noexcept { return value_; }

private:
    X509* value_;
};

class BioPointerGuard {
public:
    explicit BioPointerGuard(BIO* value) : value_(value) {}
    ~BioPointerGuard() { BIO_free(value_); }
    BioPointerGuard(const BioPointerGuard&) = delete;
    BioPointerGuard& operator=(const BioPointerGuard&) = delete;
    BIO* get() const noexcept { return value_; }

private:
    BIO* value_;
};

void add_extension(X509* certificate, int nid, const char* value) {
    X509V3_CTX context;
    X509V3_set_ctx_nodb(&context);
    X509V3_set_ctx(&context, certificate, certificate, nullptr, nullptr, 0);
    X509_EXTENSION* extension = X509V3_EXT_conf_nid(
        nullptr, &context, nid, const_cast<char*>(value));
    if (extension == nullptr || X509_add_ext(certificate, extension, -1) != 1) {
        X509_EXTENSION_free(extension);
        throw std::runtime_error(openssl_error("unable to add certificate extension"));
    }
    X509_EXTENSION_free(extension);
}

void generate_certificate(const uhf::web::TlsFiles& files) {
    EVP_PKEY_CTX* key_context = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr);
    if (key_context == nullptr || EVP_PKEY_keygen_init(key_context) != 1 ||
        EVP_PKEY_CTX_set_rsa_keygen_bits(key_context, 2048) != 1) {
        EVP_PKEY_CTX_free(key_context);
        throw std::runtime_error(openssl_error("unable to initialize TLS key generation"));
    }
    EVP_PKEY* key = nullptr;
    if (EVP_PKEY_keygen(key_context, &key) != 1) {
        EVP_PKEY_CTX_free(key_context);
        throw std::runtime_error(openssl_error("unable to generate TLS key"));
    }
    EVP_PKEY_CTX_free(key_context);
    OpenSslPointerGuard key_guard(key);

    X509* certificate = X509_new();
    if (certificate == nullptr || X509_set_version(certificate, 2) != 1 ||
        ASN1_INTEGER_set(X509_get_serialNumber(certificate), 1) != 1 ||
        X509_gmtime_adj(X509_get_notBefore(certificate), 0) == nullptr ||
        X509_gmtime_adj(X509_get_notAfter(certificate), 60L * 60L * 24L * 3650L) == nullptr ||
        X509_set_pubkey(certificate, key_guard.get()) != 1) {
        X509_free(certificate);
        throw std::runtime_error(openssl_error("unable to initialize TLS certificate"));
    }
    X509PointerGuard certificate_guard(certificate);

    X509_NAME* name = X509_get_subject_name(certificate);
    if (name == nullptr || X509_NAME_add_entry_by_txt(
            name, "CN", MBSTRING_ASC,
            reinterpret_cast<const unsigned char*>("UHF 61850 Gateway"), -1, -1, 0) != 1 ||
        X509_set_issuer_name(certificate, name) != 1) {
        throw std::runtime_error(openssl_error("unable to set TLS certificate name"));
    }
    add_extension(certificate, NID_basic_constraints, "critical,CA:FALSE");
    add_extension(certificate, NID_key_usage, "critical,digitalSignature,keyEncipherment");
    add_extension(certificate, NID_ext_key_usage, "serverAuth");
    add_extension(certificate, NID_subject_alt_name, "DNS:localhost,IP:127.0.0.1");
    if (X509_sign(certificate, key_guard.get(), EVP_sha256()) <= 0) {
        throw std::runtime_error(openssl_error("unable to sign TLS certificate"));
    }

    BioPointerGuard key_bio(BIO_new(BIO_s_mem()));
    BioPointerGuard certificate_bio(BIO_new(BIO_s_mem()));
    if (key_bio.get() == nullptr || certificate_bio.get() == nullptr ||
        PEM_write_bio_PrivateKey(key_bio.get(), key_guard.get(), nullptr, nullptr, 0, nullptr, nullptr) != 1 ||
        PEM_write_bio_X509(certificate_bio.get(), certificate_guard.get()) != 1) {
        throw std::runtime_error(openssl_error("unable to serialize TLS certificate"));
    }

    BUF_MEM* key_memory = nullptr;
    BUF_MEM* certificate_memory = nullptr;
    BIO_get_mem_ptr(key_bio.get(), &key_memory);
    BIO_get_mem_ptr(certificate_bio.get(), &certificate_memory);
    if (key_memory == nullptr || certificate_memory == nullptr) {
        throw std::runtime_error("unable to access serialized TLS certificate");
    }
    write_atomic(
        files.private_key,
        reinterpret_cast<const unsigned char*>(key_memory->data),
        key_memory->length);
    try {
        write_atomic(
            files.certificate,
            reinterpret_cast<const unsigned char*>(certificate_memory->data),
            certificate_memory->length);
    } catch (...) {
        ::unlink(files.private_key.c_str());
        throw;
    }
}

void ensure_certificate(const uhf::web::TlsFiles& files) {
    std::error_code certificate_error;
    std::error_code key_error;
    const bool certificate_exists = std::filesystem::exists(files.certificate, certificate_error);
    const bool key_exists = std::filesystem::exists(files.private_key, key_error);
    if (certificate_error || key_error) {
        throw std::runtime_error("unable to inspect TLS files");
    }
    if (certificate_exists != key_exists) {
        throw std::runtime_error("TLS certificate and private key must be provided together");
    }
    if (!certificate_exists) {
        generate_certificate(files);
    }
    if (::chmod(files.certificate.c_str(), kPrivateFileMode) < 0 ||
        ::chmod(files.private_key.c_str(), kPrivateFileMode) < 0) {
        throw std::runtime_error("unable to protect TLS files");
    }
}

}  // namespace

namespace uhf::web {

TlsContext::TlsContext(TlsFiles files) : files_(std::move(files)) {
    if (files_.certificate.empty() || files_.private_key.empty() ||
        files_.certificate.parent_path() != files_.private_key.parent_path()) {
        throw std::invalid_argument("TLS certificate paths are invalid");
    }
    ensure_private_directory(
        files_.certificate.parent_path().empty() ? std::filesystem::path{"."} :
                                                   files_.certificate.parent_path());
    ensure_certificate(files_);

    context_ = SSL_CTX_new(TLS_server_method());
    if (context_ == nullptr) {
        throw std::runtime_error(openssl_error("unable to create TLS context"));
    }
    if (SSL_CTX_set_min_proto_version(context_, TLS1_2_VERSION) != 1 ||
        SSL_CTX_set_options(context_, SSL_OP_NO_COMPRESSION) == 0 ||
        SSL_CTX_use_certificate_chain_file(context_, files_.certificate.c_str()) != 1 ||
        SSL_CTX_use_PrivateKey_file(context_, files_.private_key.c_str(), SSL_FILETYPE_PEM) != 1 ||
        SSL_CTX_check_private_key(context_) != 1 ||
        SSL_CTX_set_cipher_list(context_, "HIGH:!aNULL:!eNULL:!MD5:!RC4:!3DES") != 1) {
        const std::string message = openssl_error("unable to load TLS certificate");
        SSL_CTX_free(context_);
        context_ = nullptr;
        throw std::runtime_error(message);
    }
    SSL_CTX_set_verify(context_, SSL_VERIFY_NONE, nullptr);
    SSL_CTX_set_session_cache_mode(context_, SSL_SESS_CACHE_OFF);
}

TlsContext::~TlsContext() {
    SSL_CTX_free(context_);
}

TlsContext::TlsContext(TlsContext&& other) noexcept
    : files_(std::move(other.files_)), context_(other.context_) {
    other.context_ = nullptr;
}

TlsContext& TlsContext::operator=(TlsContext&& other) noexcept {
    if (this != &other) {
        SSL_CTX_free(context_);
        files_ = std::move(other.files_);
        context_ = other.context_;
        other.context_ = nullptr;
    }
    return *this;
}

SSL_CTX* TlsContext::native() const noexcept {
    return context_;
}

const TlsFiles& TlsContext::files() const noexcept {
    return files_;
}

}  // namespace uhf::web
