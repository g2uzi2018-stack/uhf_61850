// SPDX-License-Identifier: GPL-3.0-only
#include "web/icd_store.hpp"

#include "iec61850/scl_model.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstddef>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <openssl/evp.h>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

constexpr mode_t kIcdFileMode = S_IRUSR | S_IWUSR;
constexpr std::string_view kSclNamespace = "http://www.iec.ch/61850/2003/SCL";

bool is_name_start(char value) noexcept {
    return (value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z') ||
        value == '_' || value == ':';
}

bool is_name_character(char value) noexcept {
    return is_name_start(value) || (value >= '0' && value <= '9') || value == '-' || value == '.';
}

void skip_space(std::string_view input, std::size_t& position) noexcept {
    while (position < input.size() && std::isspace(static_cast<unsigned char>(input[position])) != 0) {
        ++position;
    }
}

bool parse_name(std::string_view input, std::size_t& position, std::string_view& name) noexcept {
    const std::size_t begin = position;
    if (position >= input.size() || !is_name_start(input[position])) {
        return false;
    }
    ++position;
    while (position < input.size() && is_name_character(input[position])) {
        ++position;
    }
    name = input.substr(begin, position - begin);
    return true;
}

bool parse_attributes(
    std::string_view input,
    std::size_t& position,
    std::string& ied_name,
    std::string_view element_name) noexcept {
    while (true) {
        skip_space(input, position);
        if (position >= input.size() || input[position] == '/' || input[position] == '>') {
            return true;
        }
        std::string_view attribute_name;
        if (!parse_name(input, position, attribute_name)) {
            return false;
        }
        skip_space(input, position);
        if (position >= input.size() || input[position++] != '=') {
            return false;
        }
        skip_space(input, position);
        if (position >= input.size() || (input[position] != '\'' && input[position] != '"')) {
            return false;
        }
        const char quote = input[position++];
        const std::size_t value_begin = position;
        while (position < input.size() && input[position] != quote) {
            if (input[position] == '<' || input[position] == '\0') {
                return false;
            }
            ++position;
        }
        if (position >= input.size()) {
            return false;
        }
        if (element_name == "IED" && attribute_name == "name") {
            ied_name.assign(input.substr(value_begin, position - value_begin));
        }
        ++position;
    }
}

bool has_open_element(std::string_view input, std::string_view element_name) noexcept {
    std::size_t position = 0U;
    while ((position = input.find('<', position)) != std::string_view::npos) {
        ++position;
        if (position >= input.size()) {
            continue;
        }
        if (input.compare(position, 3U, "!--") == 0) {
            const std::size_t end = input.find("-->", position + 3U);
            position = end == std::string_view::npos ? input.size() : end + 3U;
            continue;
        }
        if (input[position] == '/' || input[position] == '!' || input[position] == '?') {
            continue;
        }
        std::string_view name;
        if (!parse_name(input, position, name)) {
            continue;
        }
        if (name == element_name) {
            return true;
        }
    }
    return false;
}

bool has_attribute_value(
    std::string_view input,
    std::string_view element_name,
    std::string_view attribute_name,
    std::string_view attribute_value) noexcept {
    std::size_t position = 0U;
    while ((position = input.find('<', position)) != std::string_view::npos) {
        ++position;
        if (position >= input.size()) {
            continue;
        }
        if (input.compare(position, 3U, "!--") == 0) {
            const std::size_t end = input.find("-->", position + 3U);
            position = end == std::string_view::npos ? input.size() : end + 3U;
            continue;
        }
        if (input[position] == '/' || input[position] == '!' || input[position] == '?') {
            continue;
        }
        std::string_view name;
        if (!parse_name(input, position, name)) {
            continue;
        }
        std::size_t tag_end = position;
        bool quoted = false;
        char quote = 0;
        while (tag_end < input.size()) {
            const char character = input[tag_end++];
            if (quoted) {
                if (character == quote) {
                    quoted = false;
                }
            } else if (character == '\'' || character == '"') {
                quoted = true;
                quote = character;
            } else if (character == '>') {
                break;
            }
        }
        if (tag_end > input.size() || quoted || input[tag_end - 1U] != '>') {
            return false;
        }
        const std::string_view tag = input.substr(position, tag_end - position - 1U);
        const std::string marker = std::string(attribute_name) + "=\"" +
            std::string(attribute_value) + "\"";
        const std::string single_marker = std::string(attribute_name) + "='" +
            std::string(attribute_value) + "'";
        if (name == element_name &&
            (tag.find(marker) != std::string_view::npos ||
             tag.find(single_marker) != std::string_view::npos)) {
            return true;
        }
        position = tag_end;
    }
    return false;
}

bool valid_xml(std::string_view input, std::string& ied_name) noexcept {
    if (input.empty() || input.size() > uhf::web::kMaxIcdBytes || input.find('\0') != std::string_view::npos ||
        input.find("<!DOCTYPE") != std::string_view::npos ||
        input.find("<!doctype") != std::string_view::npos ||
        input.find("<!ENTITY") != std::string_view::npos ||
        input.find("<!entity") != std::string_view::npos ||
        input.find("<![CDATA[") != std::string_view::npos) {
        return false;
    }

    std::vector<std::string_view> stack;
    stack.reserve(32U);
    std::size_t position = 0U;
    bool root_seen = false;
    while (position < input.size()) {
        const std::size_t open = input.find('<', position);
        if (open == std::string_view::npos) {
            for (const char character : input.substr(position)) {
                if (std::iscntrl(static_cast<unsigned char>(character)) != 0 &&
                    character != '\r' && character != '\n' && character != '\t') {
                    return false;
                }
            }
            position = input.size();
            break;
        }
        for (const char character : input.substr(position, open - position)) {
            if (std::iscntrl(static_cast<unsigned char>(character)) != 0 &&
                character != '\r' && character != '\n' && character != '\t') {
                return false;
            }
        }
        position = open + 1U;
        if (position >= input.size()) {
            return false;
        }
        if (input.compare(position, 3U, "!--") == 0) {
            const std::size_t comment_end = input.find("-->", position + 3U);
            if (comment_end == std::string_view::npos) {
                return false;
            }
            position = comment_end + 3U;
            continue;
        }
        if (input[position] == '?') {
            const std::size_t declaration_end = input.find("?>", position + 1U);
            if (declaration_end == std::string_view::npos) {
                return false;
            }
            position = declaration_end + 2U;
            continue;
        }
        if (input[position] == '!') {
            return false;
        }

        const bool closing = input[position] == '/';
        if (closing) {
            ++position;
        }
        skip_space(input, position);
        std::string_view name;
        if (!parse_name(input, position, name)) {
            return false;
        }
        if (closing) {
            skip_space(input, position);
            if (position >= input.size() || input[position++] != '>' || stack.empty() ||
                stack.back() != name) {
                return false;
            }
            stack.pop_back();
            continue;
        }

        std::string_view tag_tail = input.substr(position);
        std::size_t tag_end = position;
        bool quoted = false;
        char quote = 0;
        while (tag_end < input.size()) {
            const char character = input[tag_end++];
            if (quoted) {
                if (character == quote) {
                    quoted = false;
                }
            } else if (character == '\'' || character == '"') {
                quoted = true;
                quote = character;
            } else if (character == '>') {
                break;
            }
        }
        if (tag_end > input.size() || quoted || input[tag_end - 1U] != '>') {
            return false;
        }
        const bool self_closing = tag_end >= 2U && input[tag_end - 2U] == '/';
        const std::size_t attributes_end = self_closing ? tag_end - 2U : tag_end - 1U;
        std::size_t attributes_position = position;
        if (attributes_end < attributes_position) {
            return false;
        }
        tag_tail = input.substr(attributes_position, attributes_end - attributes_position);
        if (!parse_attributes(tag_tail, attributes_position = 0U, ied_name, name)) {
            return false;
        }
        if (!root_seen) {
            if (name != "SCL" || !has_attribute_value(input.substr(open, tag_end - open), "SCL", "xmlns", kSclNamespace)) {
                return false;
            }
            root_seen = true;
        } else if (stack.empty()) {
            return false;
        }
        if (!self_closing) {
            if (stack.size() >= 64U) {
                return false;
            }
            stack.push_back(name);
        }
        position = tag_end;
    }
    if (!root_seen || !stack.empty() || ied_name.empty()) {
        return false;
    }
    return has_open_element(input, "DataSet") &&
        has_attribute_value(input, "DataSet", "name", "DSMeasurements") &&
        has_attribute_value(input, "DataSet", "name", "DSState") &&
        has_attribute_value(input, "ReportControl", "name", "RPMeasurements") &&
        has_attribute_value(input, "ReportControl", "name", "RPState") &&
        has_attribute_value(input, "ReportControl", "datSet", "DSMeasurements") &&
        has_attribute_value(input, "ReportControl", "datSet", "DSState") &&
        has_open_element(input, "LDevice");
}

std::string hash_hex(const unsigned char* bytes, std::size_t size) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(size * 2U);
    for (std::size_t index = 0U; index < size; ++index) {
        result.push_back(digits[bytes[index] >> 4U]);
        result.push_back(digits[bytes[index] & 0x0FU]);
    }
    return result;
}

}  // namespace

namespace uhf::web {

IcdStore::IcdStore(std::filesystem::path override_path, std::filesystem::path packaged_path)
    : override_path_(std::move(override_path)),
      packaged_path_(std::move(packaged_path)),
      previous_path_(override_path_.string() + ".previous") {}

std::optional<std::string> IcdStore::read_file(const std::filesystem::path& path) const {
    std::error_code error;
    if (std::filesystem::is_symlink(path, error) || error ||
        !std::filesystem::is_regular_file(path, error) || error ||
        std::filesystem::file_size(path, error) > kMaxIcdBytes || error) {
        return std::nullopt;
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return std::nullopt;
    }
    std::string contents(
        (std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    if (input.bad() || contents.size() > kMaxIcdBytes) {
        return std::nullopt;
    }
    return contents;
}

std::optional<std::string> IcdStore::read_current() const {
    if (const std::optional<std::string> override = read_file(override_path_)) {
        return override;
    }
    return read_file(packaged_path_);
}

std::optional<std::string> IcdStore::read_previous() const {
    return read_file(previous_path_);
}

IcdStatus IcdStore::status() const {
    IcdStatus result;
    const std::optional<std::string> current = read_current();
    if (current) {
        result.available = true;
        result.override_active = read_file(override_path_).has_value();
        result.size = current->size();
        result.sha256 = sha256(*current);
        (void)validate(*current, result.ied_name);
    }
    result.previous_available = read_previous().has_value();
    return result;
}

bool IcdStore::write_atomic(
    const std::filesystem::path& path, std::string_view contents) const noexcept {
    const std::filesystem::path temporary = path.string() + ".tmp";
    const int file_descriptor = ::open(
        temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, kIcdFileMode);
    if (file_descriptor < 0) {
        return false;
    }
    bool open = true;
    bool complete = false;
    do {
        if (::fchmod(file_descriptor, kIcdFileMode) < 0) {
            break;
        }
        std::size_t written = 0U;
        while (written < contents.size()) {
            const ssize_t result = ::write(
                file_descriptor, contents.data() + written, contents.size() - written);
            if (result <= 0) {
                break;
            }
            written += static_cast<std::size_t>(result);
        }
        if (written != contents.size() || ::fsync(file_descriptor) < 0 || ::close(file_descriptor) < 0) {
            open = false;
            break;
        }
        open = false;
        if (::rename(temporary.c_str(), path.c_str()) < 0) {
            break;
        }
        const std::filesystem::path parent = path.parent_path().empty() ? "." : path.parent_path();
        const int directory_descriptor = ::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (directory_descriptor < 0 || ::fsync(directory_descriptor) < 0) {
            if (directory_descriptor >= 0) {
                ::close(directory_descriptor);
            }
            break;
        }
        ::close(directory_descriptor);
        complete = true;
    } while (false);
    if (open) {
        ::close(file_descriptor);
    }
    if (!complete) {
        ::unlink(temporary.c_str());
    }
    return complete;
}

IcdReplaceResult IcdStore::replace(std::string_view contents) const {
    std::string ignored_ied_name;
    if (!validate(contents, ignored_ied_name)) {
        return IcdReplaceResult::invalid;
    }
    const std::optional<std::string> current_override = read_file(override_path_);
    if (current_override && !write_atomic(previous_path_, *current_override)) {
        return IcdReplaceResult::storage_error;
    }
    if (!write_atomic(override_path_, contents)) {
        return IcdReplaceResult::storage_error;
    }
    return IcdReplaceResult::replaced;
}

IcdRestoreResult IcdStore::restore() const {
    const std::optional<std::string> current_override = read_file(override_path_);
    if (!current_override) {
        return IcdRestoreResult::no_override;
    }
    const std::optional<std::string> previous = read_previous();
    if (previous) {
        if (!write_atomic(previous_path_, *current_override) ||
            !write_atomic(override_path_, *previous)) {
            return IcdRestoreResult::storage_error;
        }
        return IcdRestoreResult::restored;
    }
    if (!std::filesystem::remove(override_path_)) {
        return IcdRestoreResult::storage_error;
    }
    std::error_code error;
    std::filesystem::remove(previous_path_, error);
    return error ? IcdRestoreResult::storage_error : IcdRestoreResult::restored;
}

bool IcdStore::discard_override() const noexcept {
    std::error_code error;
    const bool removed = std::filesystem::remove(override_path_, error);
    return !error && (removed || !std::filesystem::exists(override_path_, error));
}

bool IcdStore::validate(std::string_view contents, std::string& ied_name) noexcept {
    ied_name.clear();
    if (!valid_xml(contents, ied_name)) {
        return false;
    }
    uhf::iec61850::SclModelDefinition definition;
    std::string error;
    if (!uhf::iec61850::parse_scl_model(contents, definition, error)) {
        ied_name.clear();
        return false;
    }
    ied_name = definition.ied_name;
    return true;
}

std::string IcdStore::sha256(std::string_view contents) {
    EVP_MD_CTX* context = EVP_MD_CTX_new();
    if (context == nullptr) {
        return {};
    }
    unsigned char digest[EVP_MAX_MD_SIZE]{};
    unsigned int digest_size = 0U;
    const bool valid = EVP_DigestInit_ex(context, EVP_sha256(), nullptr) == 1 &&
        EVP_DigestUpdate(context, contents.data(), contents.size()) == 1 &&
        EVP_DigestFinal_ex(context, digest, &digest_size) == 1;
    EVP_MD_CTX_free(context);
    return valid ? hash_hex(digest, digest_size) : std::string{};
}

}  // namespace uhf::web
