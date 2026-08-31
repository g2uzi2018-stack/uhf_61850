// SPDX-License-Identifier: GPL-3.0-only
#include "config/config_store.hpp"

#include <arpa/inet.h>
#include <cctype>
#include <cerrno>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <fcntl.h>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>
#include <unordered_set>
#include <utility>

namespace {

constexpr std::size_t kMaxConfigBytes = 16U * 1024U;
constexpr mode_t kConfigDirectoryMode = S_IRWXU;
constexpr mode_t kConfigFileMode = S_IRUSR | S_IWUSR;

class FlatJsonParser {
public:
    explicit FlatJsonParser(std::string_view input) : input_(input) {}

    bool parse(uhf::config::Values& values) {
        skip_space();
        if (!consume('{')) {
            return false;
        }
        skip_space();
        if (consume('}')) {
            return false;
        }
        while (position_ < input_.size()) {
            std::string key;
            if (!parse_string(key)) {
                return false;
            }
            skip_space();
            if (!consume(':')) {
                return false;
            }
            skip_space();
            if (!parse_field(key, values)) {
                return false;
            }
            skip_space();
            if (consume('}')) {
                skip_space();
                return position_ == input_.size() &&
                    (keys_.size() == 18U || keys_.size() == 19U);
            }
            if (!consume(',')) {
                return false;
            }
            skip_space();
        }
        return false;
    }

private:
    void skip_space() noexcept {
        while (position_ < input_.size()) {
            const unsigned char character = static_cast<unsigned char>(input_[position_]);
            if (character != ' ' && character != '\t' && character != '\r' && character != '\n') {
                break;
            }
            ++position_;
        }
    }

    bool consume(char expected) noexcept {
        if (position_ >= input_.size() || input_[position_] != expected) {
            return false;
        }
        ++position_;
        return true;
    }

    bool parse_string(std::string& result) {
        if (!consume('"')) {
            return false;
        }
        result.clear();
        while (position_ < input_.size()) {
            const char character = input_[position_++];
            if (character == '"') {
                return result.size() <= 256U;
            }
            if (character == '\\' || static_cast<unsigned char>(character) < 0x20U) {
                return false;
            }
            result.push_back(character);
        }
        return false;
    }

    bool parse_unsigned(std::uint64_t& result) {
        const std::size_t begin = position_;
        while (position_ < input_.size() && input_[position_] >= '0' &&
               input_[position_] <= '9') {
            ++position_;
        }
        if (begin == position_) {
            return false;
        }
        const auto parsed = std::from_chars(
            input_.data() + begin, input_.data() + position_, result);
        return parsed.ec == std::errc{} && parsed.ptr == input_.data() + position_;
    }

    bool parse_boolean(bool& result) {
        if (input_.substr(position_, 4U) == "true") {
            position_ += 4U;
            result = true;
            return true;
        }
        if (input_.substr(position_, 5U) == "false") {
            position_ += 5U;
            result = false;
            return true;
        }
        return false;
    }

    template <typename Integer>
    static bool assign_unsigned(std::uint64_t value, Integer minimum, Integer maximum, Integer& output) {
        const std::uint64_t lower = static_cast<std::uint64_t>(minimum);
        const std::uint64_t upper = static_cast<std::uint64_t>(maximum);
        if (value < lower || value > upper) {
            return false;
        }
        output = static_cast<Integer>(value);
        return true;
    }

    bool parse_field(const std::string& key, uhf::config::Values& values) {
        if (!keys_.insert(key).second) {
            return false;
        }
        if (key == "version") {
            std::uint64_t version = 0U;
            return parse_unsigned(version) && version != 0U;
        }
        if (key == "acquisition_device" || key == "rtu_device" || key == "modbus_tcp_bind" ||
            key == "iec_ied_name") {
            std::string value;
            if (!parse_string(value)) {
                return false;
            }
            if (key == "acquisition_device") {
                values.acquisition_device = std::move(value);
            } else if (key == "rtu_device") {
                values.rtu_device = std::move(value);
            } else if (key == "modbus_tcp_bind") {
                values.modbus_tcp_bind = std::move(value);
            } else {
                values.iec_ied_name = std::move(value);
            }
            return true;
        }

        std::uint64_t value = 0U;
        if (key == "tls_enabled") {
            return parse_boolean(values.tls_enabled);
        }
        if (key == "iec_enabled") {
            return parse_boolean(values.iec_enabled);
        }
        if (!parse_unsigned(value)) {
            return false;
        }
        if (key == "acquisition_slave_id") {
            return assign_unsigned(value, std::uint8_t{1U}, std::uint8_t{247U}, values.acquisition_slave_id);
        }
        if (key == "acquisition_period_ms") {
            return assign_unsigned(value, std::uint32_t{6000U}, std::uint32_t{60000U}, values.acquisition_period_ms);
        }
        if (key == "acquisition_response_timeout_ms") {
            return assign_unsigned(value, std::uint32_t{50U}, std::uint32_t{180U}, values.acquisition_response_timeout_ms);
        }
        if (key == "acquisition_max_retries") {
            return assign_unsigned(value, std::uint8_t{0U}, std::uint8_t{3U}, values.acquisition_max_retries);
        }
        if (key == "rtu_unit_id") {
            return assign_unsigned(value, std::uint8_t{1U}, std::uint8_t{247U}, values.rtu_unit_id);
        }
        if (key == "modbus_tcp_unit_id") {
            return assign_unsigned(
                value, std::uint8_t{1U}, std::uint8_t{247U}, values.modbus_tcp_unit_id);
        }
        if (key == "modbus_tcp_port") {
            return assign_unsigned(value, std::uint16_t{1U}, std::uint16_t{65535U}, values.modbus_tcp_port);
        }
        if (key == "web_port") {
            return assign_unsigned(value, std::uint16_t{1024U}, std::uint16_t{65535U}, values.web_port);
        }
        if (key == "iec_port") {
            return assign_unsigned(value, std::uint16_t{1U}, std::uint16_t{65535U}, values.iec_port);
        }
        if (key == "storage_period_seconds") {
            return assign_unsigned(value, std::uint32_t{60U}, std::uint32_t{86400U}, values.storage_period_seconds);
        }
        if (key == "storage_retention_days") {
            return assign_unsigned(value, std::uint32_t{1U}, std::uint32_t{30U}, values.storage_retention_days);
        }
        if (key == "storage_min_free_bytes") {
            return assign_unsigned(
                value, std::uint64_t{268435456U}, std::numeric_limits<std::uint64_t>::max(),
                values.storage_min_free_bytes);
        }
        return false;
    }

    std::string_view input_;
    std::size_t position_{0U};
    std::unordered_set<std::string> keys_;
};

bool valid_device(std::string_view value, std::string_view expected) noexcept {
    return value == expected;
}

bool valid_ied_name(std::string_view value) noexcept {
    if (value.empty() || value.size() > 32U ||
        !(std::isalpha(static_cast<unsigned char>(value.front())) != 0)) {
        return false;
    }
    for (const char character : value) {
        const unsigned char unsigned_character = static_cast<unsigned char>(character);
        if (!(std::isalnum(unsigned_character) != 0 || character == '_')) {
            return false;
        }
    }
    return true;
}

std::string json_escape(std::string_view value) {
    std::string result;
    result.reserve(value.size());
    for (const char character : value) {
        if (character == '"' || character == '\\') {
            result.push_back('\\');
        }
        result.push_back(character);
    }
    return result;
}

}  // namespace

namespace uhf::config {

ConfigStore::ConfigStore(std::filesystem::path file) : file_(std::move(file)) {
    const std::filesystem::path directory = file_.parent_path().empty() ? "." : file_.parent_path();
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error || !std::filesystem::is_directory(directory, error) || error ||
        ::chmod(directory.c_str(), kConfigDirectoryMode) < 0) {
        throw std::runtime_error("unable to prepare configuration directory");
    }

    std::error_code file_error;
    const bool file_exists = std::filesystem::exists(file_, file_error);
    if (file_error) {
        throw std::runtime_error("unable to inspect configuration file");
    }
    const std::optional<std::string> contents = file_exists ? read_file(file_) : std::nullopt;
    if (file_exists && !contents) {
        throw std::runtime_error("unable to read configuration file");
    }
    if (contents) {
        Values loaded;
        if (!parse_values(*contents, loaded) || !validate(loaded)) {
            throw std::runtime_error("configuration file is invalid");
        }
        snapshot_.values = std::move(loaded);
        std::size_t version_position = contents->find("\"version\"");
        if (version_position == std::string::npos) {
            throw std::runtime_error("configuration version is missing");
        }
        version_position = contents->find(':', version_position);
        if (version_position == std::string::npos) {
            throw std::runtime_error("configuration version is invalid");
        }
        ++version_position;
        while (version_position < contents->size() &&
               std::isspace(static_cast<unsigned char>((*contents)[version_position])) != 0) {
            ++version_position;
        }
        std::uint64_t version = 0U;
        const char* begin = contents->data() + version_position;
        const auto parsed = std::from_chars(begin, contents->data() + contents->size(), version);
        if (parsed.ec != std::errc{} || version == 0U) {
            throw std::runtime_error("configuration version is invalid");
        }
        snapshot_.version = version;
    } else if (!write_atomic(file_, serialize(snapshot_))) {
        throw std::runtime_error("unable to initialize configuration file");
    }
}

Snapshot ConfigStore::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return snapshot_;
}

std::string ConfigStore::to_json() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return serialize(snapshot_);
}

UpdateResult ConfigStore::update(std::uint64_t expected_version, std::string_view object_json) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (expected_version != snapshot_.version) {
        return UpdateResult::conflict;
    }
    Values values;
    if (!parse_values(object_json, values) || !validate(values)) {
        return UpdateResult::invalid;
    }
    Snapshot candidate{snapshot_.version + 1U, std::move(values)};
    if (!write_atomic(file_, serialize(candidate))) {
        return UpdateResult::storage_error;
    }
    snapshot_ = std::move(candidate);
    return UpdateResult::updated;
}

const std::filesystem::path& ConfigStore::path() const noexcept {
    return file_;
}

bool ConfigStore::parse_values(std::string_view json, Values& values) {
    if (json.size() > kMaxConfigBytes) {
        return false;
    }
    Values defaults;
    values = defaults;
    FlatJsonParser parser(json);
    return parser.parse(values);
}

bool ConfigStore::validate(const Values& values) noexcept {
    in_addr address{};
    return values.tls_enabled && valid_device(values.acquisition_device, "/dev/ttyS1") &&
        valid_device(values.rtu_device, "/dev/ttyS4") &&
        ::inet_pton(AF_INET, values.modbus_tcp_bind.c_str(), &address) == 1 &&
        valid_ied_name(values.iec_ied_name) && values.acquisition_slave_id >= 1U &&
        values.acquisition_slave_id <= 247U && values.rtu_unit_id >= 1U &&
        values.rtu_unit_id <= 247U && values.modbus_tcp_unit_id >= 1U &&
        values.modbus_tcp_unit_id <= 247U && values.modbus_tcp_port != 0U &&
        values.web_port >= 1024U && values.iec_port != 0U &&
        values.storage_period_seconds >= 60U &&
        values.storage_retention_days >= 1U && values.storage_retention_days <= 30U &&
        values.storage_min_free_bytes >= 268435456U;
}

std::string ConfigStore::serialize(const Snapshot& snapshot) {
    const Values& values = snapshot.values;
    return "{\n"
        "  \"version\": " + std::to_string(snapshot.version) + ",\n"
        "  \"acquisition_device\": \"" + json_escape(values.acquisition_device) + "\",\n"
        "  \"acquisition_slave_id\": " + std::to_string(values.acquisition_slave_id) + ",\n"
        "  \"acquisition_period_ms\": " + std::to_string(values.acquisition_period_ms) + ",\n"
        "  \"acquisition_response_timeout_ms\": " + std::to_string(values.acquisition_response_timeout_ms) + ",\n"
        "  \"acquisition_max_retries\": " + std::to_string(values.acquisition_max_retries) + ",\n"
        "  \"rtu_device\": \"" + json_escape(values.rtu_device) + "\",\n"
        "  \"rtu_unit_id\": " + std::to_string(values.rtu_unit_id) + ",\n"
        "  \"modbus_tcp_bind\": \"" + json_escape(values.modbus_tcp_bind) + "\",\n"
        "  \"modbus_tcp_unit_id\": " + std::to_string(values.modbus_tcp_unit_id) + ",\n"
        "  \"modbus_tcp_port\": " + std::to_string(values.modbus_tcp_port) + ",\n"
        "  \"web_port\": " + std::to_string(values.web_port) + ",\n"
        "  \"tls_enabled\": " + std::string(values.tls_enabled ? "true" : "false") + ",\n"
        "  \"iec_enabled\": " + std::string(values.iec_enabled ? "true" : "false") + ",\n"
        "  \"iec_port\": " + std::to_string(values.iec_port) + ",\n"
        "  \"iec_ied_name\": \"" + json_escape(values.iec_ied_name) + "\",\n"
        "  \"storage_period_seconds\": " + std::to_string(values.storage_period_seconds) + ",\n"
        "  \"storage_retention_days\": " + std::to_string(values.storage_retention_days) + ",\n"
        "  \"storage_min_free_bytes\": " + std::to_string(values.storage_min_free_bytes) + "\n"
        "}\n";
}

bool ConfigStore::write_atomic(
    const std::filesystem::path& path, std::string_view contents) noexcept {
    const std::filesystem::path temporary = path.string() + ".tmp";
    const int file_descriptor = ::open(
        temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, kConfigFileMode);
    if (file_descriptor < 0) {
        return false;
    }
    std::size_t written = 0U;
    while (written < contents.size()) {
        const ssize_t result = ::write(
            file_descriptor, contents.data() + written, contents.size() - written);
        if (result <= 0) {
            ::close(file_descriptor);
            ::unlink(temporary.c_str());
            return false;
        }
        written += static_cast<std::size_t>(result);
    }
    if (::fchmod(file_descriptor, kConfigFileMode) < 0 || ::fsync(file_descriptor) < 0 ||
        ::close(file_descriptor) < 0 || ::rename(temporary.c_str(), path.c_str()) < 0) {
        ::close(file_descriptor);
        ::unlink(temporary.c_str());
        return false;
    }
    const std::filesystem::path parent = path.parent_path().empty() ? "." : path.parent_path();
    const int directory_descriptor = ::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (directory_descriptor < 0) {
        return false;
    }
    const int result = ::fsync(directory_descriptor);
    ::close(directory_descriptor);
    return result == 0;
}

std::optional<std::string> ConfigStore::read_file(const std::filesystem::path& path) {
    std::error_code error;
    if (!std::filesystem::exists(path, error)) {
        return error ? std::nullopt : std::optional<std::string>{};
    }
    if (error || !std::filesystem::is_regular_file(path, error) || error ||
        std::filesystem::file_size(path, error) > kMaxConfigBytes || error) {
        return std::nullopt;
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return std::nullopt;
    }
    std::string contents{
        std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    if (input.bad() || contents.size() > kMaxConfigBytes) {
        return std::nullopt;
    }
    return contents;
}

}  // namespace uhf::config
