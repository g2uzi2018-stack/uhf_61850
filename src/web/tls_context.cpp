// SPDX-License-Identifier: GPL-3.0-only
#include "web/tls_context.hpp"

#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <cctype>
#include <cstddef>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/objects.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>
#include <limits>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>

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

SSL_CTX* create_context() {
    SSL_CTX* context = SSL_CTX_new(TLS_server_method());
    if (context == nullptr || SSL_CTX_set_min_proto_version(context, TLS1_2_VERSION) != 1 ||
        SSL_CTX_set_options(context, SSL_OP_NO_COMPRESSION) == 0 ||
        SSL_CTX_set_cipher_list(context, "HIGH:!aNULL:!eNULL:!MD5:!RC4:!3DES") != 1) {
        SSL_CTX_free(context);
        return nullptr;
    }
    SSL_CTX_set_verify(context, SSL_VERIFY_NONE, nullptr);
    SSL_CTX_set_session_cache_mode(context, SSL_SESS_CACHE_OFF);
    return context;
}

bool valid_certificate(X509* certificate, EVP_PKEY* key) {
    if (certificate == nullptr || key == nullptr ||
        X509_check_private_key(certificate, key) != 1 ||
        X509_cmp_current_time(X509_get_notBefore(certificate)) > 0 ||
        X509_cmp_current_time(X509_get_notAfter(certificate)) < 0) {
        return false;
    }

    int critical = 0;
    int extension_index = -1;
    GENERAL_NAMES* names = static_cast<GENERAL_NAMES*>(
        X509_get_ext_d2i(
            certificate, NID_subject_alt_name, &critical, &extension_index));
    if (names == nullptr || sk_GENERAL_NAME_num(names) == 0) {
        GENERAL_NAMES_free(names);
        return false;
    }
    bool usable_name = false;
    for (int index = 0; index < sk_GENERAL_NAME_num(names); ++index) {
        const GENERAL_NAME* name = sk_GENERAL_NAME_value(names, index);
        if (name->type == GEN_DNS && name->d.dNSName != nullptr &&
            ASN1_STRING_length(name->d.dNSName) > 0) {
            usable_name = true;
        } else if (name->type == GEN_IPADD && name->d.iPAddress != nullptr &&
                   (ASN1_STRING_length(name->d.iPAddress) == 4 ||
                    ASN1_STRING_length(name->d.iPAddress) == 16)) {
            usable_name = true;
        }
    }
    GENERAL_NAMES_free(names);
    if (!usable_name) {
        return false;
    }

    extension_index = -1;
    EXTENDED_KEY_USAGE* extended_usage = static_cast<EXTENDED_KEY_USAGE*>(
        X509_get_ext_d2i(
            certificate, NID_ext_key_usage, &critical, &extension_index));
    if (extended_usage == nullptr) {
        EXTENDED_KEY_USAGE_free(extended_usage);
        return false;
    }
    bool server_auth = false;
    for (int index = 0; index < sk_ASN1_OBJECT_num(extended_usage); ++index) {
        if (OBJ_obj2nid(sk_ASN1_OBJECT_value(extended_usage, index)) == NID_server_auth) {
            server_auth = true;
            break;
        }
    }
    EXTENDED_KEY_USAGE_free(extended_usage);
    if (!server_auth) {
        return false;
    }

    extension_index = -1;
    BASIC_CONSTRAINTS* constraints = static_cast<BASIC_CONSTRAINTS*>(
        X509_get_ext_d2i(
            certificate, NID_basic_constraints, &critical, &extension_index));
    const bool is_ca = constraints != nullptr && constraints->ca != 0;
    BASIC_CONSTRAINTS_free(constraints);
    if (is_ca) {
        return false;
    }

    extension_index = -1;
    ASN1_BIT_STRING* key_usage = static_cast<ASN1_BIT_STRING*>(
        X509_get_ext_d2i(certificate, NID_key_usage, &critical, &extension_index));
    if (key_usage != nullptr) {
        const bool can_sign = ASN1_BIT_STRING_get_bit(key_usage, 0) != 0;
        const bool can_encrypt = ASN1_BIT_STRING_get_bit(key_usage, 2) != 0;
        ASN1_BIT_STRING_free(key_usage);
        if (!can_sign && !can_encrypt) {
            return false;
        }
    }
    return true;
}

SSL_CTX* context_from_pem(std::string_view certificate_pem, std::string_view private_key_pem) {
    if (certificate_pem.empty() || private_key_pem.empty() ||
        certificate_pem.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
        private_key_pem.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return nullptr;
    }
    BioPointerGuard certificate_bio(BIO_new_mem_buf(
        certificate_pem.data(), static_cast<int>(certificate_pem.size())));
    BioPointerGuard key_bio(BIO_new_mem_buf(
        private_key_pem.data(), static_cast<int>(private_key_pem.size())));
    if (certificate_bio.get() == nullptr || key_bio.get() == nullptr) {
        return nullptr;
    }
    X509PointerGuard certificate(PEM_read_bio_X509(certificate_bio.get(), nullptr, nullptr, nullptr));
    OpenSslPointerGuard key(PEM_read_bio_PrivateKey(key_bio.get(), nullptr, nullptr, nullptr));
    if (!valid_certificate(certificate.get(), key.get())) {
        return nullptr;
    }
    SSL_CTX* context = create_context();
    if (context == nullptr || SSL_CTX_use_certificate(context, certificate.get()) != 1 ||
        SSL_CTX_use_PrivateKey(context, key.get()) != 1 || SSL_CTX_check_private_key(context) != 1) {
        SSL_CTX_free(context);
        return nullptr;
    }
    return context;
}

SSL_CTX* context_from_files(const uhf::web::TlsFiles& files) {
    SSL_CTX* context = create_context();
    if (context == nullptr ||
        SSL_CTX_use_certificate_chain_file(context, files.certificate.c_str()) != 1 ||
        SSL_CTX_use_PrivateKey_file(context, files.private_key.c_str(), SSL_FILETYPE_PEM) != 1 ||
        SSL_CTX_check_private_key(context) != 1 ||
        !valid_certificate(
            SSL_CTX_get0_certificate(context), SSL_CTX_get0_privatekey(context))) {
        SSL_CTX_free(context);
        return nullptr;
    }
    return context;
}

std::string read_bounded_file(const std::filesystem::path& path) {
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error) || error ||
        std::filesystem::file_size(path, error) > 128U * 1024U || error) {
        throw std::runtime_error("unable to read TLS backup");
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("unable to read TLS backup");
    }
    std::string contents{
        std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    if (input.bad() || contents.size() > 128U * 1024U) {
        throw std::runtime_error("unable to read TLS backup");
    }
    return contents;
}

void backup_file(const std::filesystem::path& path) {
    const std::string contents = read_bounded_file(path);
    write_atomic(
        path.string() + ".previous",
        reinterpret_cast<const unsigned char*>(contents.data()), contents.size());
}

void remove_file(const std::filesystem::path& path) noexcept {
    if (::unlink(path.c_str()) == 0 || errno == ENOENT) {
        try {
            sync_parent(path);
        } catch (...) {
        }
    }
}

void restore_backup(const uhf::web::TlsFiles& files) noexcept {
    try {
        const std::string certificate = read_bounded_file(files.certificate.string() + ".previous");
        const std::string private_key = read_bounded_file(files.private_key.string() + ".previous");
        write_atomic(
            files.certificate,
            reinterpret_cast<const unsigned char*>(certificate.data()), certificate.size());
        write_atomic(
            files.private_key,
            reinterpret_cast<const unsigned char*>(private_key.data()), private_key.size());
    } catch (...) {
    }
}

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

bool valid_dns_name(std::string_view value) noexcept {
    if (value.empty() || value.size() > 253U) {
        return false;
    }
    std::size_t label_begin = 0U;
    for (std::size_t index = 0U; index <= value.size(); ++index) {
        if (index == value.size() || value[index] == '.') {
            const std::size_t label_size = index - label_begin;
            if (label_size == 0U || label_size > 63U ||
                value[label_begin] == '-' || value[index - 1U] == '-') {
                return false;
            }
            label_begin = index + 1U;
            continue;
        }
        const unsigned char character = static_cast<unsigned char>(value[index]);
        if (std::isalnum(character) == 0 && value[index] != '-') {
            return false;
        }
    }
    return true;
}

void append_san(std::vector<std::string>& names, std::string value) {
    if (std::find(names.begin(), names.end(), value) == names.end()) {
        names.push_back(std::move(value));
    }
}

void generate_certificate(
    const uhf::web::TlsFiles& files, std::string_view subject_alt_names) {
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
    const std::string san_text(subject_alt_names);
    add_extension(certificate, NID_subject_alt_name, san_text.c_str());
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
        generate_certificate(files, uhf::web::local_subject_alt_names());
    }
    if (::chmod(files.certificate.c_str(), kPrivateFileMode) < 0 ||
        ::chmod(files.private_key.c_str(), kPrivateFileMode) < 0) {
        throw std::runtime_error("unable to protect TLS files");
    }
}

}  // namespace

namespace uhf::web {

std::string local_subject_alt_names() {
    std::vector<std::string> names;
    names.emplace_back("DNS:localhost");
    names.emplace_back("IP:127.0.0.1");

    char hostname[256]{};
    if (::gethostname(hostname, sizeof(hostname) - 1U) == 0 &&
        valid_dns_name(hostname)) {
        append_san(names, "DNS:" + std::string(hostname));
    }

    ifaddrs* interfaces = nullptr;
    if (::getifaddrs(&interfaces) == 0) {
        std::size_t address_count = 0U;
        for (const ifaddrs* current = interfaces;
             current != nullptr && address_count < 16U;
             current = current->ifa_next) {
            if (current->ifa_addr == nullptr ||
                current->ifa_addr->sa_family != AF_INET) {
                continue;
            }
            char address_text[INET_ADDRSTRLEN]{};
            const auto* address = reinterpret_cast<const sockaddr_in*>(
                current->ifa_addr);
            if (::inet_ntop(
                    AF_INET, &address->sin_addr, address_text,
                    sizeof(address_text)) == nullptr) {
                continue;
            }
            append_san(names, "IP:" + std::string(address_text));
            ++address_count;
        }
        ::freeifaddrs(interfaces);
    }

    std::string result;
    for (const std::string& name : names) {
        if (!result.empty()) {
            result.push_back(',');
        }
        result += name;
    }
    return result;
}

void ensure_tls_files(const TlsFiles& files) {
    if (files.certificate.empty() || files.private_key.empty() ||
        files.certificate.parent_path() != files.private_key.parent_path()) {
        throw std::invalid_argument("TLS certificate paths are invalid");
    }
    ensure_private_directory(
        files.certificate.parent_path().empty() ? std::filesystem::path{"."} :
                                                   files.certificate.parent_path());
    ensure_certificate(files);
}

TlsContext::TlsContext(TlsFiles files) : files_(std::move(files)) {
    ensure_tls_files(files_);

    context_ = context_from_files(files_);
    if (context_ == nullptr) {
        const std::string message = openssl_error("unable to load TLS certificate");
        throw std::runtime_error(message);
    }
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

TlsReplaceResult TlsContext::replace(
    std::string_view certificate_pem, std::string_view private_key_pem) {
    SSL_CTX* replacement = context_from_pem(certificate_pem, private_key_pem);
    if (replacement == nullptr) {
        ERR_clear_error();
        return TlsReplaceResult::invalid;
    }
    try {
        backup_file(files_.certificate);
        backup_file(files_.private_key);
        write_atomic(
            files_.private_key,
            reinterpret_cast<const unsigned char*>(private_key_pem.data()), private_key_pem.size());
        write_atomic(
            files_.certificate,
            reinterpret_cast<const unsigned char*>(certificate_pem.data()), certificate_pem.size());
        SSL_CTX* previous = context_;
        context_ = replacement;
        replacement = nullptr;
        SSL_CTX_free(previous);
        remove_file(files_.certificate.string() + ".previous");
        remove_file(files_.private_key.string() + ".previous");
        return TlsReplaceResult::replaced;
    } catch (...) {
        restore_backup(files_);
        SSL_CTX_free(replacement);
        return TlsReplaceResult::storage_error;
    }
}

}  // namespace uhf::web
