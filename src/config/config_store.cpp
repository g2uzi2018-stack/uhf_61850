// SPDX-License-Identifier: GPL-3.0-only
#include "config/config_store.hpp"

#include "acquisition/serial_settings.hpp"

#include <arpa/inet.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
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
                    (keys_.size() == 18U || keys_.size() == 19U ||
                     keys_.size() == 20U || keys_.size() == 22U ||
                     keys_.size() == 23U || keys_.size() == 24U ||
                     keys_.size() == 25U ||
                     keys_.size() == 27U || keys_.size() == 28U ||
                     keys_.size() == 29U || keys_.size() == 30U ||
                     keys_.size() == 31U || keys_.size() == 32U ||
                     keys_.size() == 33U || keys_.size() == 34U ||
                     keys_.size() == 35U || keys_.size() == 36U ||
                     keys_.size() == 37U || keys_.size() == 38U);
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
            if (static_cast<unsigned char>(character) < 0x20U) {
                return false;
            }
            if (character != '\\') {
                result.push_back(character);
                continue;
            }
            if (position_ >= input_.size()) {
                return false;
            }
            const char escaped = input_[position_++];
            switch (escaped) {
            case '"':
            case '\\':
            case '/':
                result.push_back(escaped);
                break;
            case 'b':
                result.push_back('\b');
                break;
            case 'f':
                result.push_back('\f');
                break;
            case 'n':
                result.push_back('\n');
                break;
            case 'r':
                result.push_back('\r');
                break;
            case 't':
                result.push_back('\t');
                break;
            case 'u': {
                if (position_ + 4U > input_.size()) {
                    return false;
                }
                unsigned int code_unit = 0U;
                for (std::size_t index = 0U; index < 4U; ++index) {
                    const int digit = hex_value(input_[position_++]);
                    if (digit < 0) {
                        return false;
                    }
                    code_unit = (code_unit << 4U) | static_cast<unsigned int>(digit);
                }
                unsigned int code_point = code_unit;
                if (code_unit >= 0xD800U && code_unit <= 0xDBFFU) {
                    if (position_ + 6U > input_.size() || input_[position_] != '\\' ||
                        input_[position_ + 1U] != 'u') {
                        return false;
                    }
                    position_ += 2U;
                    unsigned int low_surrogate = 0U;
                    for (std::size_t index = 0U; index < 4U; ++index) {
                        const int digit = hex_value(input_[position_++]);
                        if (digit < 0) {
                            return false;
                        }
                        low_surrogate = (low_surrogate << 4U) | static_cast<unsigned int>(digit);
                    }
                    if (low_surrogate < 0xDC00U || low_surrogate > 0xDFFFU) {
                        return false;
                    }
                    code_point = 0x10000U + ((code_unit - 0xD800U) << 10U) +
                        (low_surrogate - 0xDC00U);
                } else if (code_unit >= 0xDC00U && code_unit <= 0xDFFFU) {
                    return false;
                }
                if (code_point < 0x20U || code_point > 0x10FFFFU) {
                    return false;
                }
                append_utf8(result, code_point);
                break;
            }
            default:
                return false;
            }
        }
        return false;
    }

    static int hex_value(char value) noexcept {
        if (value >= '0' && value <= '9') {
            return value - '0';
        }
        if (value >= 'a' && value <= 'f') {
            return value - 'a' + 10;
        }
        if (value >= 'A' && value <= 'F') {
            return value - 'A' + 10;
        }
        return -1;
    }

    static void append_utf8(std::string& result, unsigned int code_point) {
        if (code_point <= 0x7FU) {
            result.push_back(static_cast<char>(code_point));
        } else if (code_point <= 0x7FFU) {
            result.push_back(static_cast<char>(0xC0U | (code_point >> 6U)));
            result.push_back(static_cast<char>(0x80U | (code_point & 0x3FU)));
        } else if (code_point <= 0xFFFFU) {
            result.push_back(static_cast<char>(0xE0U | (code_point >> 12U)));
            result.push_back(static_cast<char>(0x80U | ((code_point >> 6U) & 0x3FU)));
            result.push_back(static_cast<char>(0x80U | (code_point & 0x3FU)));
        } else {
            result.push_back(static_cast<char>(0xF0U | (code_point >> 18U)));
            result.push_back(static_cast<char>(0x80U | ((code_point >> 12U) & 0x3FU)));
            result.push_back(static_cast<char>(0x80U | ((code_point >> 6U) & 0x3FU)));
            result.push_back(static_cast<char>(0x80U | (code_point & 0x3FU)));
        }
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

    bool parse_signed(std::int64_t& result) {
        const std::size_t begin = position_;
        if (position_ < input_.size() && input_[position_] == '-') {
            ++position_;
        }
        const std::size_t digits_begin = position_;
        while (position_ < input_.size() && input_[position_] >= '0' &&
               input_[position_] <= '9') {
            ++position_;
        }
        if (digits_begin == position_) {
            position_ = begin;
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

    bool parse_nullable_float(std::optional<float>& result) {
        if (input_.substr(position_, 4U) == "null") {
            position_ += 4U;
            result.reset();
            return true;
        }
        const std::size_t begin = position_;
        if (position_ < input_.size() && input_[position_] == '-') {
            ++position_;
        }
        bool have_digit = false;
        while (position_ < input_.size() && input_[position_] >= '0' &&
               input_[position_] <= '9') {
            have_digit = true;
            ++position_;
        }
        if (position_ < input_.size() && input_[position_] == '.') {
            ++position_;
            bool fractional_digit = false;
            while (position_ < input_.size() && input_[position_] >= '0' &&
                   input_[position_] <= '9') {
                fractional_digit = true;
                ++position_;
            }
            if (!fractional_digit) {
                position_ = begin;
                return false;
            }
        }
        if (!have_digit) {
            position_ = begin;
            return false;
        }
        if (position_ < input_.size() &&
            (input_[position_] == 'e' || input_[position_] == 'E')) {
            ++position_;
            if (position_ < input_.size() &&
                (input_[position_] == '+' || input_[position_] == '-')) {
                ++position_;
            }
            const std::size_t exponent_begin = position_;
            while (position_ < input_.size() && input_[position_] >= '0' &&
                   input_[position_] <= '9') {
                ++position_;
            }
            if (exponent_begin == position_) {
                position_ = begin;
                return false;
            }
        }
        const std::string text(input_.substr(begin, position_ - begin));
        char* end = nullptr;
        errno = 0;
        const float parsed = std::strtof(text.c_str(), &end);
        if (errno != 0 || end != text.c_str() + text.size() || !std::isfinite(parsed)) {
            position_ = begin;
            return false;
        }
        result = parsed;
        return true;
    }

    bool parse_alarm_thresholds(
        std::array<std::optional<float>, uhf::config::kV3AlarmThresholdCount>& values) {
        if (!consume('[')) {
            return false;
        }
        for (std::size_t index = 0U; index < values.size(); ++index) {
            skip_space();
            if (!parse_nullable_float(values[index])) {
                return false;
            }
            skip_space();
            if (index + 1U < values.size()) {
                if (!consume(',')) {
                    return false;
                }
            } else if (!consume(']')) {
                return false;
            }
        }
        return true;
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

    template <typename Integer>
    static bool assign_signed(
        std::int64_t value, Integer minimum, Integer maximum, Integer& output) {
        if (value < static_cast<std::int64_t>(minimum) ||
            value > static_cast<std::int64_t>(maximum)) {
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
            key == "iec_ied_name" || key == "overview_title" || key == "overview_device" ||
            key == "sntp_server" || key == "v3_current_encoding" ||
            key == "v3_current_serial" || key == "v3_temperature_serial") {
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
            } else if (key == "iec_ied_name") {
                values.iec_ied_name = std::move(value);
            } else if (key == "overview_title") {
                values.overview_title = std::move(value);
            } else if (key == "overview_device") {
                values.overview_device = std::move(value);
            } else if (key == "v3_current_encoding") {
                values.v3_current_encoding = std::move(value);
            } else if (key == "v3_current_serial") {
                values.v3_current_serial = std::move(value);
            } else if (key == "v3_temperature_serial") {
                values.v3_temperature_serial = std::move(value);
            } else {
                values.sntp_server = std::move(value);
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
        if (key == "ftp_enabled") {
            return parse_boolean(values.ftp_enabled);
        }
        if (key == "time_sync_enabled") {
            return parse_boolean(values.time_sync_enabled);
        }
        if (key == "storage_event_threshold_dbm") {
            std::int64_t threshold_value = 0;
            return parse_signed(threshold_value) &&
                assign_signed(threshold_value, std::int32_t{-70}, std::int32_t{15},
                    values.storage_event_threshold_dbm);
        }
        if (key == "storage_event_rearm_dbm") {
            std::int64_t rearm_value = 0;
            return parse_signed(rearm_value) &&
                assign_signed(rearm_value, std::int32_t{-70}, std::int32_t{15},
                    values.storage_event_rearm_dbm);
        }
        if (key == "v3_alarm_thresholds") {
            return parse_alarm_thresholds(values.v3_alarm_thresholds);
        }
        if (key == "v3_current_multiplier") {
            return parse_nullable_float(values.v3_current_multiplier);
        }
        if (key == "v3_current_offset") {
            return parse_nullable_float(values.v3_current_offset);
        }
        if (key == "v3_temperature_multiplier") {
            return parse_nullable_float(values.v3_temperature_multiplier);
        }
        if (key == "v3_temperature_offset") {
            return parse_nullable_float(values.v3_temperature_offset);
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
        if (key == "ftp_port") {
            return assign_unsigned(value, std::uint16_t{1U}, std::uint16_t{65535U}, values.ftp_port);
        }
        if (key == "phase_start_degree") {
            return assign_unsigned(
                value, std::uint16_t{0U}, std::uint16_t{360U}, values.phase_start_degree);
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
        if (key == "storage_event_delta_db") {
            return assign_unsigned(value, std::uint32_t{1U}, std::uint32_t{85U},
                values.storage_event_delta_db);
        }
        if (key == "storage_event_merge_seconds") {
            return assign_unsigned(value, std::uint32_t{0U}, std::uint32_t{3600U},
                values.storage_event_merge_seconds);
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

bool valid_config_text(std::string_view value, std::size_t maximum) noexcept {
    if (value.empty() || value.size() > maximum) {
        return false;
    }
    for (const char character : value) {
        const unsigned char unsigned_character = static_cast<unsigned char>(character);
        if (unsigned_character < 0x20U || unsigned_character == 0x7FU) {
            return false;
        }
    }
    return true;
}

bool valid_sntp_server(std::string_view value) noexcept {
    if (!valid_config_text(value, 253U) || value.front() == '.' || value.back() == '.') {
        return false;
    }
    char previous = '\0';
    for (const char character : value) {
        const unsigned char unsigned_character = static_cast<unsigned char>(character);
        if (!(std::isalnum(unsigned_character) != 0 || character == '.' || character == '-')) {
            return false;
        }
        if (character == '.' && previous == '.') {
            return false;
        }
        previous = character;
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

std::string alarm_thresholds_json(
    const std::array<std::optional<float>, uhf::config::kV3AlarmThresholdCount>& values) {
    std::string result{"["};
    for (std::size_t index = 0U; index < values.size(); ++index) {
        if (index != 0U) {
            result.push_back(',');
        }
        if (!values[index]) {
            result.append("null");
            continue;
        }
        std::array<char, 64U> buffer{};
        const auto converted = std::to_chars(
            buffer.data(), buffer.data() + buffer.size(), *values[index],
            std::chars_format::general, std::numeric_limits<float>::max_digits10);
        if (converted.ec != std::errc{}) {
            result.append("null");
        } else {
            result.append(buffer.data(), converted.ptr);
        }
    }
    result.push_back(']');
    return result;
}

std::string nullable_float_json(const std::optional<float>& value) {
    if (!value) {
        return "null";
    }
    std::array<char, 64U> buffer{};
    const auto converted = std::to_chars(
        buffer.data(), buffer.data() + buffer.size(), *value,
        std::chars_format::general, std::numeric_limits<float>::max_digits10);
    if (converted.ec != std::errc{}) {
        return "null";
    }
    return std::string(buffer.data(), converted.ptr);
}

bool parse_version(std::string_view contents, std::uint64_t& version) {
    std::size_t position = contents.find("\"version\"");
    if (position == std::string_view::npos) {
        return false;
    }
    position = contents.find(':', position);
    if (position == std::string_view::npos) {
        return false;
    }
    ++position;
    while (position < contents.size() &&
           std::isspace(static_cast<unsigned char>(contents[position])) != 0) {
        ++position;
    }
    if (position >= contents.size()) {
        return false;
    }
    const char* begin = contents.data() + position;
    const auto parsed = std::from_chars(begin, contents.data() + contents.size(), version);
    return parsed.ec == std::errc{} && version != 0U;
}

}  // namespace

namespace uhf::config {

ConfigStore::ConfigStore(
    std::filesystem::path file, std::filesystem::path defaults_file)
    : file_(std::move(file)) {
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
        if (!parse_version(*contents, snapshot_.version)) {
            throw std::runtime_error("configuration version is invalid");
        }
    } else {
        std::optional<std::string> defaults;
        if (!defaults_file.empty()) {
            std::error_code defaults_error;
            const bool defaults_exists = std::filesystem::exists(defaults_file, defaults_error);
            if (defaults_error) {
                throw std::runtime_error("unable to inspect default configuration");
            }
            if (defaults_exists) {
                defaults = read_file(defaults_file);
                if (!defaults) {
                    throw std::runtime_error("unable to read default configuration");
                }
            }
        }
        if (defaults) {
            Values loaded;
            if (defaults->empty() || !parse_values(*defaults, loaded) || !validate(loaded) ||
                !parse_version(*defaults, snapshot_.version)) {
                throw std::runtime_error("default configuration is invalid");
            }
            snapshot_.values = std::move(loaded);
        }
        if (!write_atomic(file_, serialize(snapshot_))) {
            throw std::runtime_error("unable to initialize configuration file");
        }
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

bool ConfigStore::set_iec_ied_name(std::string_view ied_name) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!valid_ied_name(ied_name)) {
        return false;
    }
    if (snapshot_.values.iec_ied_name == ied_name) {
        return true;
    }
    Snapshot candidate = snapshot_;
    ++candidate.version;
    candidate.values.iec_ied_name.assign(ied_name);
    if (!write_atomic(file_, serialize(candidate))) {
        return false;
    }
    snapshot_ = std::move(candidate);
    return true;
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
    const bool web_port_conflicts_with_modbus =
        values.web_port == values.modbus_tcp_port;
    const bool web_port_conflicts_with_iec =
        values.iec_enabled && values.web_port == values.iec_port;
    const bool modbus_port_conflicts_with_iec =
        values.iec_enabled && values.modbus_tcp_port == values.iec_port;
    const bool ftp_port_conflicts = values.ftp_enabled &&
        (values.ftp_port == values.web_port || values.ftp_port == values.modbus_tcp_port ||
         (values.iec_enabled && values.ftp_port == values.iec_port));
    const bool current_unconfigured = values.v3_current_encoding == "unconfigured" &&
        !values.v3_current_multiplier && !values.v3_current_offset;
    const bool current_configured =
        (values.v3_current_encoding == "unsigned16" ||
         values.v3_current_encoding == "signed16") &&
        values.v3_current_multiplier && values.v3_current_offset &&
        std::isfinite(*values.v3_current_multiplier) &&
        *values.v3_current_multiplier > 0.0F &&
        std::isfinite(*values.v3_current_offset);
    const bool temperature_unconfigured =
        !values.v3_temperature_multiplier && !values.v3_temperature_offset;
    const bool temperature_configured =
        values.v3_temperature_multiplier && values.v3_temperature_offset &&
        std::isfinite(*values.v3_temperature_multiplier) &&
        *values.v3_temperature_multiplier > 0.0F &&
        std::isfinite(*values.v3_temperature_offset);
    bool current_serial_valid = false;
    bool temperature_serial_valid = false;
    (void)acquisition::parse_serial_profile(
        values.v3_current_serial, current_serial_valid);
    (void)acquisition::parse_serial_profile(
        values.v3_temperature_serial, temperature_serial_valid);
    return values.tls_enabled && !web_port_conflicts_with_modbus &&
        !web_port_conflicts_with_iec && !modbus_port_conflicts_with_iec &&
        !ftp_port_conflicts &&
        valid_device(values.acquisition_device, "/dev/ttyS1") &&
        valid_device(values.rtu_device, "/dev/ttyS4") &&
        ::inet_pton(AF_INET, values.modbus_tcp_bind.c_str(), &address) == 1 &&
        valid_ied_name(values.iec_ied_name) &&
        valid_config_text(values.overview_title, 64U) &&
        valid_config_text(values.overview_device, 64U) &&
        values.phase_start_degree <= 360U && valid_sntp_server(values.sntp_server) &&
        values.acquisition_slave_id >= 1U &&
        values.acquisition_slave_id <= 247U && values.rtu_unit_id >= 1U &&
        values.rtu_unit_id <= 247U && values.modbus_tcp_unit_id >= 1U &&
        values.modbus_tcp_unit_id <= 247U && values.modbus_tcp_port != 0U &&
        values.web_port >= 1024U && values.iec_port != 0U &&
        values.storage_period_seconds >= 60U &&
        values.storage_retention_days >= 1U && values.storage_retention_days <= 30U &&
        values.storage_min_free_bytes >= 268435456U &&
        values.storage_event_threshold_dbm >= -70 &&
        values.storage_event_threshold_dbm <= 15 &&
        values.storage_event_rearm_dbm >= -70 &&
        values.storage_event_rearm_dbm <= 15 &&
        values.storage_event_rearm_dbm < values.storage_event_threshold_dbm &&
        values.storage_event_delta_db >= 1U && values.storage_event_delta_db <= 85U &&
        values.storage_event_merge_seconds <= 3600U &&
        std::all_of(
            values.v3_alarm_thresholds.begin(), values.v3_alarm_thresholds.end(),
            [](const std::optional<float>& threshold) {
                return !threshold || std::isfinite(*threshold);
            }) &&
        (current_unconfigured || current_configured) &&
        (temperature_unconfigured || temperature_configured) &&
        current_serial_valid && temperature_serial_valid;
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
        "  \"ftp_enabled\": " + std::string(values.ftp_enabled ? "true" : "false") + ",\n"
        "  \"ftp_port\": " + std::to_string(values.ftp_port) + ",\n"
        "  \"overview_title\": \"" + json_escape(values.overview_title) + "\",\n"
        "  \"overview_device\": \"" + json_escape(values.overview_device) + "\",\n"
        "  \"phase_start_degree\": " + std::to_string(values.phase_start_degree) + ",\n"
        "  \"time_sync_enabled\": " +
            std::string(values.time_sync_enabled ? "true" : "false") + ",\n"
        "  \"sntp_server\": \"" + json_escape(values.sntp_server) + "\",\n"
        "  \"storage_period_seconds\": " + std::to_string(values.storage_period_seconds) + ",\n"
        "  \"storage_retention_days\": " + std::to_string(values.storage_retention_days) + ",\n"
        "  \"storage_min_free_bytes\": " + std::to_string(values.storage_min_free_bytes) + ",\n"
        "  \"storage_event_threshold_dbm\": " + std::to_string(values.storage_event_threshold_dbm) + ",\n"
        "  \"storage_event_rearm_dbm\": " + std::to_string(values.storage_event_rearm_dbm) + ",\n"
        "  \"storage_event_delta_db\": " + std::to_string(values.storage_event_delta_db) + ",\n"
        "  \"storage_event_merge_seconds\": " + std::to_string(values.storage_event_merge_seconds) + ",\n"
        "  \"v3_alarm_thresholds\": " + alarm_thresholds_json(values.v3_alarm_thresholds) + ",\n"
        "  \"v3_current_encoding\": \"" + json_escape(values.v3_current_encoding) + "\",\n"
        "  \"v3_current_multiplier\": " + nullable_float_json(values.v3_current_multiplier) + ",\n"
        "  \"v3_current_offset\": " + nullable_float_json(values.v3_current_offset) + ",\n"
        "  \"v3_temperature_multiplier\": " + nullable_float_json(values.v3_temperature_multiplier) + ",\n"
        "  \"v3_temperature_offset\": " + nullable_float_json(values.v3_temperature_offset) + ",\n"
        "  \"v3_current_serial\": \"" + json_escape(values.v3_current_serial) + "\",\n"
        "  \"v3_temperature_serial\": \"" + json_escape(values.v3_temperature_serial) + "\"\n"
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
