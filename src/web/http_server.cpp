// SPDX-License-Identifier: GPL-3.0-only
#include "web/http_server.hpp"

#include "app/build_info.hpp"
#include "web/crypto.hpp"

#include <algorithm>
#include <array>
#include <arpa/inet.h>
#include <cerrno>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <limits>
#include <netinet/in.h>
#include <optional>
#include <openssl/evp.h>
#include <openssl/ssl.h>
#include <poll.h>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/types.h>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

constexpr std::size_t kMaxHeaderBytes = 16U * 1024U;
constexpr std::size_t kMaxBodyBytes = 64U * 1024U;
constexpr std::size_t kMaxRequestBytes = kMaxHeaderBytes + kMaxBodyBytes;
constexpr std::uint16_t kListenBacklog = 16;
constexpr std::uintmax_t kMaxResponseBytes = 512U * 1024U;
constexpr std::size_t kMaxSessions = 64;
constexpr std::size_t kMaxLoginFailureRecords = 64;
constexpr std::size_t kMaxPasswordJsonBytes = 256;
constexpr std::size_t kMaxWebSocketConnections = 4;
constexpr std::size_t kMaxWebSocketPayloadBytes = 128U * 1024U;
constexpr std::size_t kMaxWebSocketInputBytes = 8U * 1024U;
constexpr auto kSessionLifetime = std::chrono::minutes(30);
constexpr auto kLoginFailureWindow = std::chrono::seconds(60);
constexpr auto kLoginBlockTime = std::chrono::seconds(30);

struct ParsedRequest {
    std::string method;
    std::string target;
    std::string version;
    std::unordered_map<std::string, std::string> headers;
    std::string body;
};

int hex_value(char value) {
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

std::string lower_ascii(std::string_view value) {
    std::string result;
    result.reserve(value.size());
    for (const char character : value) {
        if (character >= 'A' && character <= 'Z') {
            result.push_back(static_cast<char>(character - 'A' + 'a'));
        } else {
            result.push_back(character);
        }
    }
    return result;
}

std::string_view trim_ows(std::string_view value) {
    std::size_t begin = 0;
    while (begin < value.size() && (value[begin] == ' ' || value[begin] == '\t')) {
        ++begin;
    }
    std::size_t end = value.size();
    while (end > begin && (value[end - 1U] == ' ' || value[end - 1U] == '\t')) {
        --end;
    }
    return value.substr(begin, end - begin);
}

bool parse_decimal(std::string_view value, std::size_t& result) {
    if (value.empty()) {
        return false;
    }
    result = 0;
    for (const char character : value) {
        if (character < '0' || character > '9') {
            return false;
        }
        const std::size_t digit = static_cast<std::size_t>(character - '0');
        if (result > (std::numeric_limits<std::size_t>::max() - digit) / 10U) {
            return false;
        }
        result = result * 10U + digit;
    }
    return true;
}

bool parse_if_match(std::string_view value, std::uint64_t& version) {
    if (value.size() < 3U || value.front() != '"' || value.back() != '"') {
        return false;
    }
    const std::string_view number = value.substr(1U, value.size() - 2U);
    if (number.empty()) {
        return false;
    }
    const auto parsed = std::from_chars(
        number.data(), number.data() + number.size(), version);
    return parsed.ec == std::errc{} && parsed.ptr == number.data() + number.size() && version != 0U;
}

bool parse_header_block(
    std::string_view header_block,
    std::unordered_map<std::string, std::string>& headers,
    std::size_t& content_length) {
    const std::size_t request_line_end = header_block.find("\r\n");
    if (request_line_end == std::string_view::npos) {
        return false;
    }

    headers.clear();
    content_length = 0;
    bool content_length_seen = false;
    std::size_t position = request_line_end + 2U;
    while (position < header_block.size()) {
        std::size_t line_end = header_block.find("\r\n", position);
        if (line_end == std::string_view::npos) {
            line_end = header_block.size();
        }
        const std::string_view line = header_block.substr(position, line_end - position);
        const std::size_t separator = line.find(':');
        if (separator == std::string_view::npos || separator == 0) {
            return false;
        }

        const std::string_view name = trim_ows(line.substr(0, separator));
        const std::string_view value = trim_ows(line.substr(separator + 1U));
        if (name.empty()) {
            return false;
        }
        const std::string lower_name = lower_ascii(name);
        if (lower_name == "transfer-encoding" && !value.empty()) {
            return false;
        }
        if (lower_name == "content-length") {
            std::size_t parsed_length = 0;
            if (!parse_decimal(value, parsed_length) || parsed_length > kMaxBodyBytes) {
                return false;
            }
            if (content_length_seen && parsed_length != content_length) {
                return false;
            }
            content_length = parsed_length;
            content_length_seen = true;
        }
        headers[lower_name] = std::string(value);

        if (line_end == header_block.size()) {
            break;
        }
        position = line_end + 2U;
    }
    return true;
}

ssize_t receive_bytes(int client_fd, SSL* tls, void* buffer, std::size_t size) {
    if (tls == nullptr) {
        return ::recv(client_fd, buffer, size, 0);
    }
    const int result = SSL_read(tls, buffer, static_cast<int>(size));
    if (result > 0) {
        return result;
    }
    const int error = SSL_get_error(tls, result);
    errno = (error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE)
        ? EAGAIN
        : ECONNRESET;
    return -1;
}

ssize_t send_bytes(int client_fd, SSL* tls, const void* buffer, std::size_t size) {
    if (tls == nullptr) {
        return ::send(client_fd, buffer, size, MSG_NOSIGNAL);
    }
    const int result = SSL_write(tls, buffer, static_cast<int>(size));
    if (result > 0) {
        return result;
    }
    const int error = SSL_get_error(tls, result);
    errno = (error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE)
        ? EAGAIN
        : ECONNRESET;
    return -1;
}

bool send_all(int client_fd, SSL* tls, std::string_view data) {
    std::size_t sent = 0;
    while (sent < data.size()) {
        const ssize_t result = send_bytes(client_fd, tls, data.data() + sent, data.size() - sent);
        if (result <= 0) {
            return false;
        }
        sent += static_cast<std::size_t>(result);
    }
    return true;
}

bool set_nonblocking(int file_descriptor) {
    const int flags = ::fcntl(file_descriptor, F_GETFL, 0);
    return flags >= 0 && ::fcntl(file_descriptor, F_SETFL, flags | O_NONBLOCK) == 0;
}

bool header_has_token(std::string_view value, std::string_view token) {
    const std::string expected = lower_ascii(token);
    std::size_t begin = 0U;
    while (begin <= value.size()) {
        const std::size_t end = value.find(',', begin);
        if (lower_ascii(trim_ows(value.substr(
                begin, end == std::string_view::npos ? value.size() - begin : end - begin))) ==
            expected) {
            return true;
        }
        if (end == std::string_view::npos) {
            break;
        }
        begin = end + 1U;
    }
    return false;
}

int base64_value(char value) {
    if (value >= 'A' && value <= 'Z') {
        return value - 'A';
    }
    if (value >= 'a' && value <= 'z') {
        return value - 'a' + 26;
    }
    if (value >= '0' && value <= '9') {
        return value - '0' + 52;
    }
    if (value == '+') {
        return 62;
    }
    if (value == '/') {
        return 63;
    }
    return -1;
}

bool valid_websocket_key(std::string_view value) {
    if (value.size() != 24U || value[22U] != '=' || value[23U] != '=') {
        return false;
    }
    std::array<std::uint8_t, 16U> decoded{};
    std::size_t decoded_size = 0U;
    for (std::size_t index = 0; index < 20U; index += 4U) {
        const int first = base64_value(value[index]);
        const int second = base64_value(value[index + 1U]);
        const int third = base64_value(value[index + 2U]);
        const int fourth = base64_value(value[index + 3U]);
        if (first < 0 || second < 0 || third < 0 || fourth < 0) {
            return false;
        }
        const std::uint32_t combined = static_cast<std::uint32_t>(first) << 18U |
            static_cast<std::uint32_t>(second) << 12U |
            static_cast<std::uint32_t>(third) << 6U |
            static_cast<std::uint32_t>(fourth);
        if (decoded_size + 3U > decoded.size()) {
            return false;
        }
        decoded[decoded_size++] = static_cast<std::uint8_t>(combined >> 16U);
        decoded[decoded_size++] = static_cast<std::uint8_t>(combined >> 8U);
        decoded[decoded_size++] = static_cast<std::uint8_t>(combined);
    }
    const int final_first = base64_value(value[20U]);
    const int final_second = base64_value(value[21U]);
    if (final_first < 0 || final_second < 0 || final_second % 16 != 0) {
        return false;
    }
    decoded[decoded_size++] = static_cast<std::uint8_t>(
        (static_cast<std::uint32_t>(final_first) << 2U) |
        static_cast<std::uint32_t>(final_second >> 4));
    return decoded_size == decoded.size();
}

std::string base64_encode(const unsigned char* bytes, std::size_t size) {
    constexpr char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string result;
    result.reserve((size + 2U) / 3U * 4U);
    for (std::size_t index = 0; index < size; index += 3U) {
        const std::size_t remaining = size - index;
        const std::uint32_t first = bytes[index];
        const std::uint32_t second = remaining > 1U ? bytes[index + 1U] : 0U;
        const std::uint32_t third = remaining > 2U ? bytes[index + 2U] : 0U;
        const std::uint32_t combined = first << 16U | second << 8U | third;
        result.push_back(alphabet[(combined >> 18U) & 0x3FU]);
        result.push_back(alphabet[(combined >> 12U) & 0x3FU]);
        result.push_back(remaining > 1U ? alphabet[(combined >> 6U) & 0x3FU] : '=');
        result.push_back(remaining > 2U ? alphabet[combined & 0x3FU] : '=');
    }
    return result;
}

std::string websocket_accept(std::string_view key) {
    constexpr std::string_view magic = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    const std::string input = std::string(key) + std::string(magic);
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int digest_size = 0U;
    if (EVP_Digest(
            input.data(), input.size(), digest.data(), &digest_size, EVP_sha1(), nullptr) != 1) {
        return {};
    }
    return base64_encode(digest.data(), digest_size);
}

std::vector<std::uint8_t> websocket_frame(
    std::uint8_t opcode, const std::vector<std::uint8_t>& payload) {
    if (payload.size() > kMaxWebSocketPayloadBytes) {
        return {};
    }
    std::vector<std::uint8_t> frame;
    frame.reserve(payload.size() + 10U);
    frame.push_back(static_cast<std::uint8_t>(0x80U | (opcode & 0x0FU)));
    if (payload.size() <= 125U) {
        frame.push_back(static_cast<std::uint8_t>(payload.size()));
    } else if (payload.size() <= 0xFFFFU) {
        frame.push_back(126U);
        frame.push_back(static_cast<std::uint8_t>(payload.size() >> 8U));
        frame.push_back(static_cast<std::uint8_t>(payload.size() & 0xFFU));
    } else {
        frame.push_back(127U);
        const std::uint64_t size = static_cast<std::uint64_t>(payload.size());
        for (int shift = 56; shift >= 0; shift -= 8) {
            frame.push_back(static_cast<std::uint8_t>(size >> shift));
        }
    }
    frame.insert(frame.end(), payload.begin(), payload.end());
    return frame;
}

std::vector<std::uint8_t> websocket_text(std::string_view text) {
    return websocket_frame(
        0x1U,
        std::vector<std::uint8_t>(
            reinterpret_cast<const std::uint8_t*>(text.data()),
            reinterpret_cast<const std::uint8_t*>(text.data()) + text.size()));
}

bool consume_websocket_input(
    std::vector<std::uint8_t>& input,
    std::vector<std::uint8_t>& output,
    bool& close_requested) {
    while (input.size() >= 2U) {
        const std::uint8_t first = input[0];
        const std::uint8_t second = input[1];
        const bool final_frame = (first & 0x80U) != 0U;
        const std::uint8_t opcode = static_cast<std::uint8_t>(first & 0x0FU);
        const bool masked = (second & 0x80U) != 0U;
        std::uint64_t payload_size = second & 0x7FU;
        std::size_t header_size = 2U;
        if (!final_frame || (first & 0x70U) != 0U || !masked) {
            return false;
        }
        if (payload_size == 126U) {
            if (input.size() < 4U) {
                return true;
            }
            payload_size = static_cast<std::uint64_t>(input[2]) << 8U | input[3];
            header_size = 4U;
        } else if (payload_size == 127U) {
            return false;
        }
        if ((opcode & 0x08U) != 0U && payload_size > 125U) {
            return false;
        }
        if (payload_size > kMaxWebSocketPayloadBytes ||
            payload_size > std::numeric_limits<std::size_t>::max() - header_size - 6U) {
            return false;
        }
        const std::size_t frame_size = header_size + 4U + static_cast<std::size_t>(payload_size);
        if (input.size() < frame_size) {
            return true;
        }
        const std::size_t mask_offset = header_size;
        const std::size_t payload_offset = header_size + 4U;
        std::vector<std::uint8_t> payload(
            input.begin() + static_cast<std::ptrdiff_t>(payload_offset),
            input.begin() + static_cast<std::ptrdiff_t>(frame_size));
        for (std::size_t index = 0; index < payload.size(); ++index) {
            payload[index] = static_cast<std::uint8_t>(
                payload[index] ^ input[mask_offset + index % 4U]);
        }
        input.erase(input.begin(), input.begin() + static_cast<std::ptrdiff_t>(frame_size));
        if (opcode == 0x8U) {
            close_requested = true;
            output = websocket_frame(0x8U, payload);
            return true;
        }
        if (opcode == 0x9U) {
            output = websocket_frame(0xAU, payload);
        } else if (opcode != 0xAU && opcode != 0x1U) {
            return false;
        }
    }
    return true;
}

bool read_request(int client_fd, SSL* tls, std::string& request) {
    char buffer[1024];
    std::size_t header_end = std::string::npos;
    while ((header_end = request.find("\r\n\r\n")) == std::string::npos) {
        const ssize_t received = receive_bytes(client_fd, tls, buffer, sizeof(buffer));
        if (received <= 0) {
            return false;
        }

        const std::size_t count = static_cast<std::size_t>(received);
        if (count > kMaxRequestBytes - request.size()) {
            return false;
        }
        request.append(buffer, count);
        if (request.size() > kMaxHeaderBytes) {
            return false;
        }
    }

    std::unordered_map<std::string, std::string> ignored_headers;
    std::size_t content_length = 0;
    if (header_end > kMaxHeaderBytes ||
        !parse_header_block(request.substr(0, header_end), ignored_headers, content_length)) {
        return false;
    }
    const std::size_t body_start = header_end + 4U;
    if (content_length > kMaxRequestBytes - body_start) {
        return false;
    }
    const std::size_t required_size = body_start + content_length;
    while (request.size() < required_size) {
        const ssize_t received = receive_bytes(client_fd, tls, buffer, sizeof(buffer));
        if (received <= 0) {
            return false;
        }
        const std::size_t count = static_cast<std::size_t>(received);
        if (count > kMaxRequestBytes - request.size()) {
            return false;
        }
        request.append(buffer, count);
    }
    return true;
}

bool parse_request(std::string_view request, ParsedRequest& parsed) {
    const std::size_t header_end = request.find("\r\n\r\n");
    const std::size_t line_end = request.find("\r\n");
    if (header_end == std::string_view::npos || line_end == std::string_view::npos ||
        line_end > header_end) {
        return false;
    }

    std::istringstream request_line{std::string(request.substr(0, line_end))};
    std::string extra;
    if (!(request_line >> parsed.method >> parsed.target >> parsed.version) ||
        (request_line >> extra)) {
        return false;
    }
    std::size_t content_length = 0;
    if (!parse_header_block(request.substr(0, header_end), parsed.headers, content_length)) {
        return false;
    }
    const std::size_t body_start = header_end + 4U;
    if (content_length > request.size() - body_start) {
        return false;
    }
    parsed.body.assign(request.data() + body_start, content_length);
    return true;
}

std::string_view header_value(const ParsedRequest& request, std::string_view name) {
    const auto iterator = request.headers.find(lower_ascii(name));
    if (iterator == request.headers.end()) {
        return {};
    }
    return iterator->second;
}

bool decode_path(std::string_view target, std::string& path) {
    const std::size_t query_start = target.find('?');
    const std::string_view encoded_path = target.substr(0, query_start);
    if (encoded_path.empty() || encoded_path.front() != '/') {
        return false;
    }

    path.clear();
    path.reserve(encoded_path.size());
    for (std::size_t index = 0; index < encoded_path.size(); ++index) {
        if (encoded_path[index] != '%') {
            path.push_back(encoded_path[index]);
            continue;
        }

        if (index + 2U >= encoded_path.size()) {
            return false;
        }
        const int high = hex_value(encoded_path[index + 1U]);
        const int low = hex_value(encoded_path[index + 2U]);
        if (high < 0 || low < 0) {
            return false;
        }
        path.push_back(static_cast<char>((high << 4) | low));
        index += 2U;
    }

    if (path.find('\\') != std::string::npos || path.find('\0') != std::string::npos) {
        return false;
    }

    for (std::size_t begin = 1; begin <= path.size();) {
        const std::size_t end = path.find('/', begin);
        const std::size_t length =
            end == std::string::npos ? path.size() - begin : end - begin;
        if (path.compare(begin, length, "..") == 0) {
            return false;
        }
        if (end == std::string::npos) {
            break;
        }
        begin = end + 1U;
    }

    if (path == "/") {
        path = "/index.html";
    }
    return true;
}

bool resolve_file(
    const std::filesystem::path& document_root,
    std::string_view request_path,
    std::filesystem::path& resolved_file) {
    std::error_code error;
    const std::filesystem::path root =
        std::filesystem::weakly_canonical(document_root, error);
    if (error) {
        return false;
    }

    const std::filesystem::path relative_path{std::string(request_path.substr(1))};
    const std::filesystem::path candidate =
        std::filesystem::weakly_canonical(root / relative_path, error);
    if (error) {
        return false;
    }

    const std::string root_text = root.generic_string();
    const std::string candidate_text = candidate.generic_string();
    const bool is_root = candidate == root;
    const bool is_child =
        candidate_text.size() > root_text.size() &&
        candidate_text.compare(0, root_text.size(), root_text) == 0 &&
        candidate_text[root_text.size()] == '/';
    if (!is_root && !is_child) {
        return false;
    }

    if (!std::filesystem::is_regular_file(candidate, error) || error) {
        return false;
    }
    const std::uintmax_t file_size = std::filesystem::file_size(candidate, error);
    if (error || file_size > kMaxResponseBytes) {
        return false;
    }

    resolved_file = candidate;
    return true;
}

std::string_view content_type(const std::filesystem::path& file) {
    const std::string extension = file.extension().string();
    if (extension == ".html") {
        return "text/html; charset=utf-8";
    }
    if (extension == ".css") {
        return "text/css; charset=utf-8";
    }
    if (extension == ".js") {
        return "text/javascript; charset=utf-8";
    }
    if (extension == ".json") {
        return "application/json; charset=utf-8";
    }
    return "application/octet-stream";
}

std::string_view status_text(int status) {
    switch (status) {
    case 200:
        return "OK";
    case 204:
        return "No Content";
    case 302:
        return "Found";
    case 400:
        return "Bad Request";
    case 401:
        return "Unauthorized";
    case 403:
        return "Forbidden";
    case 404:
        return "Not Found";
    case 405:
        return "Method Not Allowed";
    case 409:
        return "Conflict";
    case 413:
        return "Payload Too Large";
    case 429:
        return "Too Many Requests";
    case 500:
        return "Internal Server Error";
    case 503:
        return "Service Unavailable";
    default:
        return "Error";
    }
}

void send_response(
    int client_fd,
    SSL* tls,
    int status,
    std::string_view type,
    std::string_view body,
    std::string_view extra_headers = {}) {
    std::string headers = "HTTP/1.1 " + std::to_string(status) + " " +
        std::string(status_text(status)) + "\r\nContent-Type: " + std::string(type) +
        "\r\nContent-Length: " + std::to_string(body.size()) +
        "\r\nConnection: close\r\nX-Content-Type-Options: nosniff\r\n"
        "Referrer-Policy: no-referrer\r\nX-Frame-Options: DENY\r\n";
    headers.append(extra_headers);
    headers.append("\r\n");
    send_all(client_fd, tls, headers);
    send_all(client_fd, tls, body);
}

void send_json(
    int client_fd,
    SSL* tls,
    int status,
    std::string_view body,
    std::string_view extra_headers = {}) {
    send_response(client_fd, tls, status, "application/json; charset=utf-8", body, extra_headers);
}

void send_error(
    int client_fd,
    SSL* tls,
    int status,
    std::string_view message,
    std::string_view extra_headers = {}) {
    const std::string body = "{\"error\":\"" + std::string(message) + "\"}\n";
    send_json(client_fd, tls, status, body, extra_headers);
}

void send_method_not_allowed(int client_fd, SSL* tls, std::string_view allowed_methods) {
    const std::string headers = "Allow: " + std::string(allowed_methods) + "\r\n";
    send_error(client_fd, tls, 405, "method not allowed", headers);
}

void send_redirect(int client_fd, SSL* tls, std::string_view location) {
    const std::string headers = "Location: " + std::string(location) + "\r\n";
    send_response(client_fd, tls, 302, "text/plain; charset=utf-8", "redirecting\n", headers);
}

void append_utf8(std::string& value, unsigned int code_point) {
    if (code_point <= 0x7FU) {
        value.push_back(static_cast<char>(code_point));
    } else if (code_point <= 0x7FFU) {
        value.push_back(static_cast<char>(0xC0U | (code_point >> 6U)));
        value.push_back(static_cast<char>(0x80U | (code_point & 0x3FU)));
    } else if (code_point <= 0xFFFFU) {
        value.push_back(static_cast<char>(0xE0U | (code_point >> 12U)));
        value.push_back(static_cast<char>(0x80U | ((code_point >> 6U) & 0x3FU)));
        value.push_back(static_cast<char>(0x80U | (code_point & 0x3FU)));
    } else {
        value.push_back(static_cast<char>(0xF0U | (code_point >> 18U)));
        value.push_back(static_cast<char>(0x80U | ((code_point >> 12U) & 0x3FU)));
        value.push_back(static_cast<char>(0x80U | ((code_point >> 6U) & 0x3FU)));
        value.push_back(static_cast<char>(0x80U | (code_point & 0x3FU)));
    }
}

bool json_string_field(
    std::string_view json, std::string_view key, std::string& value, std::size_t max_bytes) {
    const std::string marker = "\"" + std::string(key) + "\"";
    const std::size_t marker_position = json.find(marker);
    if (marker_position == std::string_view::npos) {
        return false;
    }

    std::size_t position = marker_position + marker.size();
    while (position < json.size() &&
           (json[position] == ' ' || json[position] == '\t' || json[position] == '\r' ||
            json[position] == '\n')) {
        ++position;
    }
    if (position >= json.size() || json[position] != ':') {
        return false;
    }
    ++position;
    while (position < json.size() &&
           (json[position] == ' ' || json[position] == '\t' || json[position] == '\r' ||
            json[position] == '\n')) {
        ++position;
    }
    if (position >= json.size() || json[position] != '"') {
        return false;
    }
    ++position;

    value.clear();
    while (position < json.size()) {
        const char character = json[position++];
        if (character == '"') {
            return value.size() <= max_bytes;
        }
        if (character != '\\') {
            if (static_cast<unsigned char>(character) < 0x20U) {
                return false;
            }
            value.push_back(character);
        } else {
            if (position >= json.size()) {
                return false;
            }
            const char escaped = json[position++];
            switch (escaped) {
            case '"':
            case '\\':
            case '/':
                value.push_back(escaped);
                break;
            case 'b':
                value.push_back('\b');
                break;
            case 'f':
                value.push_back('\f');
                break;
            case 'n':
                value.push_back('\n');
                break;
            case 'r':
                value.push_back('\r');
                break;
            case 't':
                value.push_back('\t');
                break;
            case 'u': {
                if (position + 4U > json.size()) {
                    return false;
                }
                unsigned int code_point = 0;
                for (std::size_t index = 0; index < 4U; ++index) {
                    const int digit = hex_value(json[position + index]);
                    if (digit < 0) {
                        return false;
                    }
                    code_point = (code_point << 4U) | static_cast<unsigned int>(digit);
                }
                position += 4U;
                if (code_point >= 0xD800U && code_point <= 0xDFFFU) {
                    return false;
                }
                append_utf8(value, code_point);
                break;
            }
            default:
                return false;
            }
        }
        if (value.size() > max_bytes) {
            return false;
        }
    }
    return false;
}

std::string session_cookie(const ParsedRequest& request) {
    const std::string_view cookie_header = header_value(request, "cookie");
    std::size_t position = 0;
    while (position < cookie_header.size()) {
        const std::size_t separator = cookie_header.find(';', position);
        const std::string_view item = trim_ows(cookie_header.substr(
            position,
            separator == std::string_view::npos ? cookie_header.size() - position
                                                  : separator - position));
        const std::size_t equals = item.find('=');
        if (equals != std::string_view::npos && trim_ows(item.substr(0, equals)) == "uhf_session") {
            const std::string_view value = trim_ows(item.substr(equals + 1U));
            if (value.size() <= 128U) {
                return std::string(value);
            }
            return {};
        }
        if (separator == std::string_view::npos) {
            break;
        }
        position = separator + 1U;
    }
    return {};
}

std::string session_json(std::string_view csrf_token, bool must_change) {
    return "{\"authenticated\":true,\"username\":\"admin\",\"must_change\":" +
        std::string(must_change ? "true" : "false") + ",\"csrf_token\":\"" +
        std::string(csrf_token) + "\"}\n";
}

std::string session_cookie_header(std::string_view token, bool secure) {
    return "Set-Cookie: uhf_session=" + std::string(token) +
        "; Max-Age=1800; Path=/; HttpOnly; SameSite=Strict" +
        std::string(secure ? "; Secure" : "") + "\r\n";
}

std::string expired_session_cookie_header(bool secure) {
    return "Set-Cookie: uhf_session=; Max-Age=0; Path=/; HttpOnly; SameSite=Strict" +
        std::string(secure ? "; Secure" : "") + "\r\n";
}

std::string_view payload_status_name(uhf::domain::PayloadStatus status) {
    switch (status) {
    case uhf::domain::PayloadStatus::good:
        return "good";
    case uhf::domain::PayloadStatus::degraded:
        return "degraded";
    case uhf::domain::PayloadStatus::not_refreshed:
        return "not_refreshed";
    }
    return "degraded";
}

std::string json_escape(std::string_view value) {
    std::string result;
    result.reserve(value.size());
    for (const char character : value) {
        switch (character) {
        case '"':
            result.append("\\\"");
            break;
        case '\\':
            result.append("\\\\");
            break;
        case '\n':
            result.append("\\n");
            break;
        case '\r':
            result.append("\\r");
            break;
        case '\t':
            result.append("\\t");
            break;
        default:
            if (static_cast<unsigned char>(character) < 0x20U) {
                result.push_back('?');
            } else {
                result.push_back(character);
            }
            break;
        }
    }
    return result;
}

constexpr std::string_view kConfigSchemaJson = R"json({
  "$schema":"https://json-schema.org/draft/2020-12/schema",
  "title":"UHF 61850 gateway configuration",
  "type":"object",
  "additionalProperties":false,
  "properties":{
    "version":{"type":"integer","minimum":1,"readOnly":true},
    "acquisition_device":{"type":"string","const":"/dev/ttyS1"},
    "acquisition_slave_id":{"type":"integer","minimum":1,"maximum":247},
    "acquisition_period_ms":{"type":"integer","minimum":6000,"maximum":60000},
    "acquisition_response_timeout_ms":{"type":"integer","minimum":50,"maximum":180},
    "acquisition_max_retries":{"type":"integer","minimum":0,"maximum":3},
    "rtu_device":{"type":"string","const":"/dev/ttyS4"},
    "rtu_unit_id":{"type":"integer","minimum":1,"maximum":247},
    "modbus_tcp_bind":{"type":"string","format":"ipv4"},
    "modbus_tcp_unit_id":{"type":"integer","minimum":1,"maximum":247},
    "modbus_tcp_port":{"type":"integer","minimum":1,"maximum":65535},
    "web_port":{"type":"integer","minimum":1024,"maximum":65535},
    "tls_enabled":{"type":"boolean","const":true},
    "iec_enabled":{"type":"boolean"},
    "iec_port":{"type":"integer","minimum":1,"maximum":65535},
    "iec_ied_name":{"type":"string","pattern":"^[A-Za-z][A-Za-z0-9_]{0,31}$"},
    "storage_period_seconds":{"type":"integer","minimum":60,"maximum":86400},
    "storage_retention_days":{"type":"integer","minimum":1,"maximum":30},
    "storage_min_free_bytes":{"type":"integer","minimum":268435456}
  },
  "required":["acquisition_device","acquisition_slave_id","acquisition_period_ms","acquisition_response_timeout_ms","acquisition_max_retries","rtu_device","rtu_unit_id","modbus_tcp_bind","modbus_tcp_unit_id","modbus_tcp_port","web_port","tls_enabled","iec_enabled","iec_port","iec_ied_name","storage_period_seconds","storage_retention_days","storage_min_free_bytes"]
})json";

}  // namespace

namespace uhf::web {

HttpServer::HttpServer(
    std::filesystem::path document_root,
    std::string bind_address,
    std::uint16_t port,
    std::filesystem::path state_directory,
    const acquisition::SnapshotStore* snapshot_store,
    HealthInputProvider health_input_provider,
    config::ConfigStore* config_store,
    bool tls_enabled,
    TlsFiles tls_files,
    logging::Logger* logger,
    std::filesystem::path data_root)
    : document_root_(std::move(document_root)),
      bind_address_(std::move(bind_address)),
      port_(port),
      auth_store_(std::move(state_directory)),
      snapshot_store_(snapshot_store),
      health_input_provider_(std::move(health_input_provider)),
      config_store_(config_store),
      tls_enabled_(tls_enabled),
      logger_(logger),
      data_root_(std::move(data_root)) {
    std::error_code error;
    document_root_ = std::filesystem::weakly_canonical(document_root_, error);
    if (error || !std::filesystem::is_directory(document_root_, error) || error) {
        throw std::invalid_argument("web root is not a directory");
    }
    if (tls_enabled_) {
        tls_context_ = std::make_unique<TlsContext>(std::move(tls_files));
    }
}

health::Report HttpServer::health_report(std::chrono::steady_clock::time_point now) const {
    health::Input input;
    if (health_input_provider_) {
        try {
            input = health_input_provider_();
        } catch (...) {
            input.storage_writable = false;
        }
    } else {
        input.storage_writable = true;
        if (snapshot_store_) {
            const std::optional<acquisition::PublishedSnapshot> latest = snapshot_store_->latest();
            if (latest) {
                input.last_acquisition_success = latest->completed_at;
                input.acquisition_last_cycle_ok = true;
            }
        }
    }
    return health_aggregator_.evaluate(input, now);
}

std::optional<std::string> HttpServer::snapshot_json() const {
    if (!snapshot_store_) {
        return std::nullopt;
    }
    const std::optional<acquisition::PublishedSnapshot> latest = snapshot_store_->latest();
    if (!latest) {
        return std::nullopt;
    }

    const domain::ParsedSnapshot& payload = latest->payload;
    constexpr std::array<std::string_view, 5U> measurement_names = {
        "average", "frequency", "peak", "phase", "noise"};
    std::string body = "{\"schema_version\":1,\"generation\":" +
        std::to_string(latest->generation) + ",\"payload_status\":\"" +
        std::string(payload_status_name(payload.payload_status)) +
        "\",\"poll_duration_ms\":" + std::to_string(latest->poll_duration.count()) +
        ",\"measurements\":[";
    for (std::size_t index = 0; index < payload.measurements.size(); ++index) {
        if (index > 0U) {
            body.append(",");
        }
        const domain::Measurement& measurement = payload.measurements[index];
        body.append("{\"name\":\"");
        body.append(measurement_names[index]);
        body.append("\",\"raw\":");
        body.append(std::to_string(measurement.raw));
        body.append(",\"value\":");
        body.append(std::to_string(measurement.value));
        body.append(",\"valid\":");
        body.append(measurement.valid ? "true" : "false");
        body.append("}");
    }
    body.append("],\"spectrum\":[");
    for (std::size_t index = 0; index < payload.spectrum.size(); ++index) {
        if (index > 0U) {
            body.append(",");
        }
        if (payload.spectrum_valid.test(index)) {
            body.append(std::to_string(payload.spectrum[index]));
        } else {
            body.append("null");
        }
    }
    body.append("],\"spectrum_valid\":[");
    for (std::size_t index = 0; index < payload.spectrum_valid.size(); ++index) {
        if (index > 0U) {
            body.append(",");
        }
        body.append(payload.spectrum_valid.test(index) ? "true" : "false");
    }
    body.append("]}\n");
    return body;
}

std::string HttpServer::logs_json(std::size_t limit) const {
    const std::size_t bounded_limit = std::min(limit, std::size_t{100U});
    const std::vector<logging::Entry> entries = logger_ == nullptr
        ? std::vector<logging::Entry>{}
        : logger_->recent(bounded_limit);
    std::string body = "{\"schema_version\":1,\"entries\":[";
    for (std::size_t index = 0U; index < entries.size(); ++index) {
        if (index > 0U) {
            body.push_back(',');
        }
        const logging::Entry& entry = entries[index];
        const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
            entry.timestamp_utc.time_since_epoch());
        body.append("{\"sequence\":");
        body.append(std::to_string(entry.sequence));
        body.append(",\"timestamp_ms\":");
        body.append(std::to_string(milliseconds.count()));
        body.append(",\"level\":\"");
        body.append(json_escape(logging::Logger::level_name(entry.level)));
        body.append("\",\"component\":\"");
        body.append(json_escape(logging::Logger::component_name(entry.component)));
        body.append("\",\"event\":\"");
        body.append(json_escape(entry.event_code));
        body.append("\",\"message\":\"");
        body.append(json_escape(entry.message));
        body.append("\",\"fields\":{");
        for (std::size_t field_index = 0U; field_index < entry.fields.size(); ++field_index) {
            if (field_index > 0U) {
                body.push_back(',');
            }
            body.push_back('"');
            body.append(json_escape(entry.fields[field_index].key));
            body.append("\":\"");
            body.append(json_escape(entry.fields[field_index].value));
            body.push_back('"');
        }
        body.append("}}");
    }
    body.append("],\"suppressed_count\":");
    body.append(std::to_string(logger_ == nullptr ? 0U : logger_->suppressed_count()));
    body.append(",\"evicted_count\":");
    body.append(std::to_string(logger_ == nullptr ? 0U : logger_->evicted_recent_count()));
    body.append("}\n");
    return body;
}

std::optional<std::string> HttpServer::frames_json() const {
    try {
        storage::FrameStore store(data_root_ / "frames");
        const std::vector<std::filesystem::path> paths = store.list(100U);
        std::string body = "{\"schema_version\":1,\"entries\":[";
        bool first = true;
        for (const std::filesystem::path& path : paths) {
            const std::optional<storage::FrameRecord> record = store.read(path);
            if (!record) {
                continue;
            }
            if (!first) {
                body.push_back(',');
            }
            first = false;
            const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
                record->timestamp.time_since_epoch());
            body.append("{\"name\":\"");
            body.append(json_escape(path.filename().string()));
            body.append("\",\"generation\":");
            body.append(std::to_string(record->generation));
            body.append(",\"timestamp_ms\":");
            body.append(std::to_string(milliseconds.count()));
            body.append(",\"payload_status\":\"");
            body.append(payload_status_name(record->payload_status));
            body.append("\"}");
        }
        body.append("]}\n");
        return body;
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<std::string> HttpServer::events_json() const {
    try {
        storage::EventBundleStore store(data_root_ / "events");
        const std::vector<std::filesystem::path> paths = store.list(100U);
        std::string body = "{\"schema_version\":1,\"entries\":[";
        bool first = true;
        for (const std::filesystem::path& path : paths) {
            const std::optional<storage::EventBundle> bundle = store.read(path);
            if (!bundle) {
                continue;
            }
            if (!first) {
                body.push_back(',');
            }
            first = false;
            const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
                bundle->first_triggered_at_utc.time_since_epoch());
            body.append("{\"name\":\"");
            body.append(json_escape(path.filename().string()));
            body.append("\",\"id\":");
            body.append(std::to_string(bundle->id));
            body.append(",\"timestamp_ms\":");
            body.append(std::to_string(milliseconds.count()));
            body.append(",\"reason_mask\":");
            body.append(std::to_string(bundle->reason_mask));
            body.append(",\"partial\":");
            body.append(bundle->partial ? "true" : "false");
            body.append(",\"frame_count\":");
            body.append(std::to_string(bundle->frames.size()));
            body.append(",\"strong_count\":");
            body.append(std::to_string(bundle->strong.trigger_count));
            body.append(",\"sudden_count\":");
            body.append(std::to_string(bundle->sudden.trigger_count));
            body.append("}");
        }
        body.append("]}\n");
        return body;
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<std::string> HttpServer::latest_frame_csv() const {
    try {
        storage::FrameStore store(data_root_ / "frames");
        const std::vector<std::filesystem::path> paths = store.list(1U);
        if (paths.empty()) {
            return std::nullopt;
        }
        const std::optional<storage::FrameRecord> record = store.read(paths.front());
        return record ? std::optional<std::string>{storage::FrameStore::to_csv(*record)} :
                        std::nullopt;
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<std::string> HttpServer::latest_event_csv() const {
    try {
        storage::EventBundleStore store(data_root_ / "events");
        const std::vector<std::filesystem::path> paths = store.list(1U);
        if (paths.empty()) {
            return std::nullopt;
        }
        const std::optional<storage::EventBundle> bundle = store.read(paths.front());
        return bundle ? std::optional<std::string>{storage::EventBundleStore::to_csv(*bundle)} :
                        std::nullopt;
    } catch (...) {
        return std::nullopt;
    }
}

int HttpServer::run() {
    const int server_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        std::perror("socket");
        return 1;
    }

    const int reuse = 1;
    if (::setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
        std::perror("setsockopt");
        ::close(server_fd);
        return 1;
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port_);
    if (::inet_pton(AF_INET, bind_address_.c_str(), &address.sin_addr) != 1) {
        std::fprintf(stderr, "bind address is not a valid IPv4 address: %s\n", bind_address_.c_str());
        ::close(server_fd);
        return 2;
    }
    if (::bind(server_fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) < 0) {
        std::perror("bind");
        ::close(server_fd);
        return 1;
    }
    if (::listen(server_fd, kListenBacklog) < 0) {
        std::perror("listen");
        ::close(server_fd);
        return 1;
    }

    sockaddr_in bound_address{};
    socklen_t bound_length = sizeof(bound_address);
    if (::getsockname(
            server_fd, reinterpret_cast<sockaddr*>(&bound_address), &bound_length) < 0) {
        std::perror("getsockname");
        ::close(server_fd);
        return 1;
    }

    std::cout << uhf::app::kProductName << " web listening on "
              << (tls_enabled_ ? "https://" : "http://") << bind_address_ << ":"
              << ntohs(bound_address.sin_port) << "/\n";
    std::error_code error;
    if (std::filesystem::exists(auth_store_.bootstrap_password_path(), error) && !error) {
        std::cout << "bootstrap password file: " << auth_store_.bootstrap_password_path() << "\n";
    }
    std::cout << std::flush;

    while (true) {
        sockaddr_in client_address{};
        socklen_t client_length = sizeof(client_address);
        const int client_fd = ::accept(
            server_fd, reinterpret_cast<sockaddr*>(&client_address), &client_length);
        if (client_fd < 0) {
            if (errno == EINTR) {
                continue;
            }
            std::perror("accept");
            ::close(server_fd);
            return 1;
        }

        SSL* tls = nullptr;
        if (tls_enabled_) {
            tls = SSL_new(tls_context_->native());
            if (tls == nullptr || SSL_set_fd(tls, client_fd) != 1) {
                SSL_free(tls);
                ::close(client_fd);
                continue;
            }
            SSL_set_accept_state(tls);
            if (SSL_accept(tls) != 1) {
                SSL_free(tls);
                ::close(client_fd);
                continue;
            }
        }

        char remote_address[INET_ADDRSTRLEN]{};
        const char* converted = ::inet_ntop(
            AF_INET, &client_address.sin_addr, remote_address, sizeof(remote_address));
        const bool handed_off = handle_client(
            client_fd, tls, converted == nullptr ? "unknown" : std::string(converted));
        if (!handed_off) {
            SSL_free(tls);
            ::close(client_fd);
        }
    }
}

void HttpServer::cleanup_sessions(std::chrono::steady_clock::time_point now) {
    for (auto iterator = sessions_.begin(); iterator != sessions_.end();) {
        if (iterator->second.expires_at <= now) {
            iterator = sessions_.erase(iterator);
        } else {
            ++iterator;
        }
    }
}

void HttpServer::run_websocket(int client_fd, SSL* tls) {
    if (!set_nonblocking(client_fd)) {
        SSL_free(tls);
        ::close(client_fd);
        active_websocket_count_.fetch_sub(1U);
        return;
    }

    std::vector<std::uint8_t> input;
    std::vector<std::uint8_t> output;
    input.reserve(kMaxWebSocketInputBytes);
    std::uint64_t last_generation = 0U;
    bool sent_health = false;
    bool close_requested = false;
    while (true) {
        pollfd descriptor{client_fd, POLLIN, 0};
        if (!output.empty()) {
            descriptor.events = static_cast<short>(descriptor.events | POLLOUT);
        }
        const int poll_result = ::poll(&descriptor, 1, 100);
        if (poll_result < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        if ((descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
            break;
        }
        if ((descriptor.revents & POLLIN) != 0) {
            std::uint8_t buffer[2048];
            const ssize_t received = receive_bytes(client_fd, tls, buffer, sizeof(buffer));
            if (received <= 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                    continue;
                }
                break;
            }
            const std::size_t count = static_cast<std::size_t>(received);
            if (count > kMaxWebSocketInputBytes - input.size()) {
                break;
            }
            input.insert(input.end(), buffer, buffer + count);
            if (!consume_websocket_input(input, output, close_requested)) {
                break;
            }
        }

        if (!close_requested) {
            const auto now = std::chrono::steady_clock::now();
            const std::optional<acquisition::PublishedSnapshot> latest =
                snapshot_store_ == nullptr ? std::nullopt : snapshot_store_->latest();
            if (latest && latest->generation != 0U && latest->generation != last_generation) {
                const std::optional<std::string> snapshot = snapshot_json();
                if (snapshot) {
                    std::string message = "{\"type\":\"telemetry\",\"health\":";
                    message.append(health_report(now).to_json());
                    message.append(",\"snapshot\":");
                    message.append(*snapshot);
                    message.append("}\n");
                    const std::vector<std::uint8_t> frame = websocket_text(message);
                    if (frame.empty()) {
                        break;
                    }
                    output = frame;
                    last_generation = latest->generation;
                    sent_health = true;
                }
            } else if (!sent_health) {
                std::string message = "{\"type\":\"health\",\"data\":";
                message.append(health_report(now).to_json());
                message.append("}\n");
                output = websocket_text(message);
                if (output.empty()) {
                    break;
                }
                sent_health = true;
            }
        }

        if (!output.empty()) {
            const ssize_t sent = send_bytes(client_fd, tls, output.data(), output.size());
            if (sent > 0) {
                output.erase(output.begin(), output.begin() + static_cast<std::ptrdiff_t>(sent));
            } else if (sent < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                break;
            }
        }
        if (close_requested && output.empty()) {
            break;
        }
    }
    SSL_free(tls);
    ::close(client_fd);
    active_websocket_count_.fetch_sub(1U);
}

bool HttpServer::handle_client(int client_fd, SSL* tls, std::string remote_address) {
    std::string request;
    if (!read_request(client_fd, tls, request)) {
        send_error(client_fd, tls, 400, "bad request");
        return false;
    }

    ParsedRequest parsed;
    if (!parse_request(request, parsed) ||
        (parsed.version != "HTTP/1.0" && parsed.version != "HTTP/1.1")) {
        send_error(client_fd, tls, 400, "bad request");
        return false;
    }

    std::string request_path;
    if (!decode_path(parsed.target, request_path)) {
        send_error(client_fd, tls, 400, "bad path");
        return false;
    }

    const auto now = std::chrono::steady_clock::now();
    cleanup_sessions(now);

    if (request_path == "/ws/v1/telemetry") {
        if (parsed.method != "GET" ||
            !header_has_token(header_value(parsed, "connection"), "upgrade") ||
            lower_ascii(header_value(parsed, "upgrade")) != "websocket") {
            send_error(client_fd, tls, 400, "websocket upgrade required");
            return false;
        }
        const std::string token = session_cookie(parsed);
        const auto iterator = sessions_.find(token);
        if (token.empty() || iterator == sessions_.end() || iterator->second.expires_at <= now) {
            if (iterator != sessions_.end()) {
                sessions_.erase(iterator);
            }
            send_error(client_fd, tls, 401, "authentication required");
            return false;
        }
        const std::string_view origin = header_value(parsed, "origin");
        const std::string_view host = header_value(parsed, "host");
        const std::string expected_origin =
            (tls == nullptr ? "http://" : "https://") + std::string(host);
        if (origin.empty() || host.empty() || origin != expected_origin) {
            send_error(client_fd, tls, 403, "same-origin request required");
            return false;
        }
        if (header_value(parsed, "sec-websocket-version") != "13") {
            send_error(client_fd, tls, 400, "unsupported websocket version");
            return false;
        }
        const std::string_view key = header_value(parsed, "sec-websocket-key");
        if (!valid_websocket_key(key)) {
            send_error(client_fd, tls, 400, "invalid websocket key");
            return false;
        }
        std::size_t active = active_websocket_count_.load();
        while (active < kMaxWebSocketConnections &&
               !active_websocket_count_.compare_exchange_weak(active, active + 1U)) {
        }
        if (active >= kMaxWebSocketConnections) {
            send_error(client_fd, tls, 503, "websocket connection limit reached", "Retry-After: 1\r\n");
            return false;
        }
        const int websocket_fd = client_fd;
        if (websocket_fd < 0) {
            active_websocket_count_.fetch_sub(1U);
            send_error(client_fd, tls, 503, "unable to open websocket");
            return false;
        }
        const std::string handshake =
            "HTTP/1.1 101 Switching Protocols\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Accept: " + websocket_accept(key) + "\r\n"
            "X-Content-Type-Options: nosniff\r\n\r\n";
        if (!send_all(client_fd, tls, handshake)) {
            active_websocket_count_.fetch_sub(1U);
            return false;
        }
        try {
            std::thread(&HttpServer::run_websocket, this, websocket_fd, tls).detach();
        } catch (...) {
            SSL_free(tls);
            ::close(websocket_fd);
            active_websocket_count_.fetch_sub(1U);
        }
        return true;
    }

    if (request_path == "/healthz") {
        if (parsed.method != "GET") {
            send_method_not_allowed(client_fd, tls, "GET");
            return false;
        }
        const health::Report report = health_report(now);
        std::string body{"{\"status\":\""};
        body.append(health::state_name(report.overall));
        body.append("\",\"version\":\"");
        body.append(uhf::app::kVersion.data(), uhf::app::kVersion.size());
        body.append("\",\"web_auth\":\"ready\"}\n");
        send_json(client_fd, tls, 200, body);
        return false;
    }

    if (request_path == "/api/v1/health" || request_path == "/api/v1/snapshot/latest") {
        if (parsed.method != "GET") {
            send_method_not_allowed(client_fd, tls, "GET");
            return false;
        }
        const std::string token = session_cookie(parsed);
        const auto iterator = sessions_.find(token);
        if (token.empty() || iterator == sessions_.end() || iterator->second.expires_at <= now) {
            if (iterator != sessions_.end()) {
                sessions_.erase(iterator);
            }
            send_error(client_fd, tls, 401, "authentication required");
            return false;
        }
        iterator->second.expires_at = now + kSessionLifetime;
        if (request_path == "/api/v1/health") {
            send_json(client_fd, tls, 200, health_report(now).to_json());
            return false;
        }
        const std::optional<std::string> body = snapshot_json();
        if (!body) {
            send_error(client_fd, tls, 503, "no snapshot available");
            return false;
        }
        if (body->size() > kMaxBodyBytes) {
            send_error(client_fd, tls, 500, "snapshot response too large");
            return false;
        }
        send_json(client_fd, tls, 200, *body);
        return false;
    }

    if (request_path == "/api/v1/tls") {
        if (parsed.method != "GET" && parsed.method != "PUT") {
            send_method_not_allowed(client_fd, tls, "GET, PUT");
            return false;
        }
        const std::string token = session_cookie(parsed);
        const auto iterator = sessions_.find(token);
        if (token.empty() || iterator == sessions_.end() || iterator->second.expires_at <= now) {
            if (iterator != sessions_.end()) {
                sessions_.erase(iterator);
            }
            send_error(client_fd, tls, 401, "authentication required");
            return false;
        }
        iterator->second.expires_at = now + kSessionLifetime;
        if (parsed.method == "GET") {
            const std::string body =
                "{\"enabled\":" + std::string(tls_context_ == nullptr ? "false" : "true") +
                ",\"certificate_ready\":" +
                std::string(tls_context_ == nullptr ? "false" : "true") + "}\n";
            send_json(client_fd, tls, 200, body);
            return false;
        }
        const std::string_view csrf = header_value(parsed, "x-csrf-token");
        if (!constant_time_equal(csrf, iterator->second.csrf_token)) {
            send_error(client_fd, tls, 403, "CSRF token required");
            return false;
        }
        if (tls_context_ == nullptr) {
            send_error(client_fd, tls, 400, "TLS is disabled in recovery mode");
            return false;
        }
        std::string current_password;
        std::string certificate_pem;
        std::string private_key_pem;
        if (!json_string_field(
                parsed.body, "current_password", current_password, kMaxPasswordJsonBytes) ||
            !json_string_field(parsed.body, "certificate_pem", certificate_pem, 32768U) ||
            !json_string_field(parsed.body, "private_key_pem", private_key_pem, 32768U) ||
            !auth_store_.verify_password("admin", current_password)) {
            send_error(client_fd, tls, 401, "current password is incorrect");
            return false;
        }
        const TlsReplaceResult result = tls_context_->replace(certificate_pem, private_key_pem);
        if (result == TlsReplaceResult::invalid) {
            send_error(client_fd, tls, 400, "invalid TLS certificate or private key");
            return false;
        }
        if (result == TlsReplaceResult::storage_error) {
            send_error(client_fd, tls, 500, "unable to save TLS credentials");
            return false;
        }
        sessions_.clear();
        send_json(
            client_fd,
            tls,
            200,
            "{\"updated\":true,\"reauthenticate\":true}\n",
            expired_session_cookie_header(tls != nullptr) + "Cache-Control: no-store\r\n");
        return false;
    }

    if (request_path == "/api/v1/logs") {
        if (parsed.method != "GET") {
            send_method_not_allowed(client_fd, tls, "GET");
            return false;
        }
        const std::string token = session_cookie(parsed);
        const auto iterator = sessions_.find(token);
        if (token.empty() || iterator == sessions_.end() || iterator->second.expires_at <= now) {
            if (iterator != sessions_.end()) {
                sessions_.erase(iterator);
            }
            send_error(client_fd, tls, 401, "authentication required");
            return false;
        }
        iterator->second.expires_at = now + kSessionLifetime;
        send_json(client_fd, tls, 200, logs_json(100U), "Cache-Control: no-store\r\n");
        return false;
    }

    if (request_path == "/api/v1/frames" || request_path == "/api/v1/events" ||
        request_path == "/api/v1/frames/export.csv" ||
        request_path == "/api/v1/events/export.csv") {
        if (parsed.method != "GET") {
            send_method_not_allowed(client_fd, tls, "GET");
            return false;
        }
        const std::string token = session_cookie(parsed);
        const auto iterator = sessions_.find(token);
        if (token.empty() || iterator == sessions_.end() || iterator->second.expires_at <= now) {
            if (iterator != sessions_.end()) {
                sessions_.erase(iterator);
            }
            send_error(client_fd, tls, 401, "authentication required");
            return false;
        }
        iterator->second.expires_at = now + kSessionLifetime;
        if (request_path == "/api/v1/frames") {
            const std::optional<std::string> body = frames_json();
            if (!body) {
                send_error(client_fd, tls, 503, "frame storage unavailable");
                return false;
            }
            send_json(client_fd, tls, 200, *body, "Cache-Control: no-store\r\n");
            return false;
        }
        if (request_path == "/api/v1/events") {
            const std::optional<std::string> body = events_json();
            if (!body) {
                send_error(client_fd, tls, 503, "event storage unavailable");
                return false;
            }
            send_json(client_fd, tls, 200, *body, "Cache-Control: no-store\r\n");
            return false;
        }
        const bool event_export = request_path == "/api/v1/events/export.csv";
        const std::optional<std::string> body = event_export
            ? latest_event_csv()
            : latest_frame_csv();
        if (!body) {
            send_error(client_fd, tls, 404, "no export available");
            return false;
        }
        if (body->size() > kMaxResponseBytes) {
            send_error(client_fd, tls, 500, "export response too large");
            return false;
        }
        const std::string headers =
            "Content-Disposition: attachment; filename=\"" +
            std::string(event_export ? "latest-event.csv" : "latest-frame.csv") +
            "\r\nCache-Control: no-store\r\n";
        send_response(client_fd, tls, 200, "text/csv; charset=utf-8", *body, headers);
        return false;
    }

    if (request_path == "/api/v1/config/schema") {
        if (parsed.method != "GET") {
            send_method_not_allowed(client_fd, tls, "GET");
            return false;
        }
        const std::string token = session_cookie(parsed);
        const auto iterator = sessions_.find(token);
        if (token.empty() || iterator == sessions_.end() || iterator->second.expires_at <= now) {
            if (iterator != sessions_.end()) {
                sessions_.erase(iterator);
            }
            send_error(client_fd, tls, 401, "authentication required");
            return false;
        }
        iterator->second.expires_at = now + kSessionLifetime;
        send_json(client_fd, tls, 200, kConfigSchemaJson, "Cache-Control: no-store\r\n");
        return false;
    }

    if (request_path == "/api/v1/config") {
        if (parsed.method != "GET" && parsed.method != "PUT") {
            send_method_not_allowed(client_fd, tls, "GET, PUT");
            return false;
        }
        const std::string token = session_cookie(parsed);
        const auto iterator = sessions_.find(token);
        if (token.empty() || iterator == sessions_.end() || iterator->second.expires_at <= now) {
            if (iterator != sessions_.end()) {
                sessions_.erase(iterator);
            }
            send_error(client_fd, tls, 401, "authentication required");
            return false;
        }
        iterator->second.expires_at = now + kSessionLifetime;
        if (config_store_ == nullptr) {
            send_error(client_fd, tls, 404, "not found");
            return false;
        }
        if (parsed.method == "GET") {
            send_json(client_fd, tls, 200, config_store_->to_json());
            return false;
        }
        const std::string_view csrf = header_value(parsed, "x-csrf-token");
        if (!constant_time_equal(csrf, iterator->second.csrf_token)) {
            send_error(client_fd, tls, 403, "CSRF token required");
            return false;
        }
        std::uint64_t expected_version = 0U;
        if (!parse_if_match(header_value(parsed, "if-match"), expected_version)) {
            send_error(client_fd, tls, 409, "configuration version required");
            return false;
        }
        const config::UpdateResult result = config_store_->update(expected_version, parsed.body);
        if (result == config::UpdateResult::conflict) {
            send_error(client_fd, tls, 409, "configuration version conflict");
            return false;
        }
        if (result == config::UpdateResult::invalid) {
            send_error(client_fd, tls, 400, "invalid configuration");
            return false;
        }
        if (result == config::UpdateResult::storage_error) {
            send_error(client_fd, tls, 500, "unable to save configuration");
            return false;
        }
        send_json(
            client_fd,
            tls,
            200,
            "{\"updated\":true,\"version\":" +
                std::to_string(config_store_->snapshot().version) +
                ",\"restart_required\":true}\n");
        return false;
    }

    if (request_path == "/api/v1/session" || request_path == "/api/v1/password" ||
        request_path == "/api/v1/session/revoke-others") {
        if (request_path == "/api/v1/session" && parsed.method == "POST") {
            const std::string_view origin = header_value(parsed, "origin");
            const std::string_view host = header_value(parsed, "host");
            const std::string expected_origin =
                (tls == nullptr ? "http://" : "https://") + std::string(host);
            if (origin.empty() || host.empty() || origin != expected_origin) {
                send_error(client_fd, tls, 403, "same-origin request required");
                return false;
            }

            const auto failure_iterator = login_failures_.find(remote_address);
            if (failure_iterator != login_failures_.end() &&
                failure_iterator->second.blocked_until > now) {
                send_error(
                    client_fd,
                    tls,
                    429,
                    "too many login attempts",
                    "Retry-After: 30\r\nCache-Control: no-store\r\n");
                return false;
            }

            std::string username;
            std::string password;
            if (parsed.body.empty() || !json_string_field(
                                            parsed.body,
                                            "username",
                                            username,
                                            kMaxPasswordJsonBytes) ||
                !json_string_field(
                    parsed.body, "password", password, kMaxPasswordJsonBytes)) {
                send_error(client_fd, tls, 400, "invalid login request");
                return false;
            }
            if (!auth_store_.verify_password(username, password)) {
                if (login_failures_.find(remote_address) == login_failures_.end() &&
                    login_failures_.size() >= kMaxLoginFailureRecords) {
                    login_failures_.erase(login_failures_.begin());
                }
                LoginFailures& failures = login_failures_[remote_address];
                if (failures.window_started.time_since_epoch().count() == 0 ||
                    now - failures.window_started >= kLoginFailureWindow) {
                    failures.count = 0;
                    failures.window_started = now;
                    failures.blocked_until = {};
                }
                ++failures.count;
                if (failures.count >= 5U) {
                    failures.blocked_until = now + kLoginBlockTime;
                    failures.count = 0;
                }
                send_error(client_fd, tls, 401, "invalid username or password");
                return false;
            }

            login_failures_.erase(remote_address);
            if (sessions_.size() >= kMaxSessions) {
                sessions_.erase(sessions_.begin());
            }
            const std::string session_token = random_hex(32U);
            const std::string csrf_token = random_hex(32U);
            sessions_.emplace(
                session_token,
                Session{csrf_token, std::move(remote_address), now + kSessionLifetime});
            const std::string headers = session_cookie_header(session_token, tls != nullptr) +
                "Cache-Control: no-store\r\n";
            send_json(client_fd, tls, 200, session_json(csrf_token, auth_store_.must_change()), headers);
            return false;
        }

        if (request_path == "/api/v1/session" && parsed.method == "GET") {
            const std::string token = session_cookie(parsed);
            const auto iterator = sessions_.find(token);
            if (token.empty() || iterator == sessions_.end() || iterator->second.expires_at <= now) {
                if (iterator != sessions_.end()) {
                    sessions_.erase(iterator);
                }
                send_error(client_fd, tls, 401, "authentication required");
                return false;
            }
            iterator->second.expires_at = now + kSessionLifetime;
            send_json(client_fd, tls, 200, session_json(iterator->second.csrf_token, auth_store_.must_change()));
            return false;
        }

        if (request_path == "/api/v1/session" && parsed.method == "DELETE") {
            const std::string token = session_cookie(parsed);
            const auto iterator = sessions_.find(token);
            if (token.empty() || iterator == sessions_.end() || iterator->second.expires_at <= now) {
                if (iterator != sessions_.end()) {
                    sessions_.erase(iterator);
                }
                send_error(client_fd, tls, 401, "authentication required");
                return false;
            }
            const std::string_view csrf = header_value(parsed, "x-csrf-token");
            if (!constant_time_equal(csrf, iterator->second.csrf_token)) {
                send_error(client_fd, tls, 403, "CSRF token required");
                return false;
            }
            sessions_.erase(iterator);
            send_response(
                client_fd,
                tls,
                204,
                "application/json; charset=utf-8",
                {},
                expired_session_cookie_header(tls != nullptr));
            return false;
        }

        if (request_path == "/api/v1/password" && parsed.method == "PUT") {
            const std::string token = session_cookie(parsed);
            const auto iterator = sessions_.find(token);
            if (token.empty() || iterator == sessions_.end() || iterator->second.expires_at <= now) {
                if (iterator != sessions_.end()) {
                    sessions_.erase(iterator);
                }
                send_error(client_fd, tls, 401, "authentication required");
                return false;
            }
            const std::string_view csrf = header_value(parsed, "x-csrf-token");
            if (!constant_time_equal(csrf, iterator->second.csrf_token)) {
                send_error(client_fd, tls, 403, "CSRF token required");
                return false;
            }

            std::string current_password;
            std::string new_password;
            if (!json_string_field(
                    parsed.body, "current_password", current_password, kMaxPasswordJsonBytes) ||
                !json_string_field(
                    parsed.body, "new_password", new_password, kMaxPasswordJsonBytes)) {
                send_error(client_fd, tls, 400, "invalid password request");
                return false;
            }
            const PasswordChangeResult result =
                auth_store_.change_password(current_password, new_password);
            if (result == PasswordChangeResult::invalid_current_password) {
                send_error(client_fd, tls, 401, "current password is incorrect");
                return false;
            }
            if (result == PasswordChangeResult::invalid_new_password) {
                send_error(client_fd, tls, 400, "new password does not meet policy");
                return false;
            }
            if (result == PasswordChangeResult::storage_error) {
                send_error(client_fd, tls, 500, "unable to save password");
                return false;
            }

            sessions_.clear();
            send_json(
                client_fd,
                tls,
                200,
                "{\"changed\":true,\"reauthenticate\":true}\n",
                expired_session_cookie_header(tls != nullptr) + "Cache-Control: no-store\r\n");
            return false;
        }

        if (request_path == "/api/v1/session/revoke-others" && parsed.method == "POST") {
            const std::string token = session_cookie(parsed);
            const auto iterator = sessions_.find(token);
            if (token.empty() || iterator == sessions_.end() || iterator->second.expires_at <= now) {
                if (iterator != sessions_.end()) {
                    sessions_.erase(iterator);
                }
                send_error(client_fd, tls, 401, "authentication required");
                return false;
            }
            const std::string_view csrf = header_value(parsed, "x-csrf-token");
            if (!constant_time_equal(csrf, iterator->second.csrf_token)) {
                send_error(client_fd, tls, 403, "CSRF token required");
                return false;
            }
            for (auto session_iterator = sessions_.begin(); session_iterator != sessions_.end();) {
                if (session_iterator->first == token) {
                    ++session_iterator;
                } else {
                    session_iterator = sessions_.erase(session_iterator);
                }
            }
            send_json(client_fd, tls, 200, "{\"revoked\":true}\n");
            return false;
        }

        if (request_path == "/api/v1/session") {
            send_method_not_allowed(client_fd, tls, "GET, POST, DELETE");
        } else if (request_path == "/api/v1/password") {
            send_method_not_allowed(client_fd, tls, "PUT");
        } else {
            send_method_not_allowed(client_fd, tls, "POST");
        }
        return false;
    }

    if (request_path.rfind("/api/", 0) == 0) {
        send_error(client_fd, tls, 404, "not found");
        return false;
    }

    if (request_path == "/login" || request_path == "/login.html") {
        if (parsed.method != "GET") {
            send_method_not_allowed(client_fd, tls, "GET");
            return false;
        }
        request_path = "/login.html";
    } else if (request_path == "/index.html" || request_path == "/overview" ||
               request_path == "/overview.html" || request_path == "/settings.html" ||
               request_path == "/logs.html" || request_path == "/storage.html") {
        if (parsed.method != "GET") {
            send_method_not_allowed(client_fd, tls, "GET");
            return false;
        }
        const std::string token = session_cookie(parsed);
        const auto iterator = sessions_.find(token);
        if (token.empty() || iterator == sessions_.end() || iterator->second.expires_at <= now) {
            if (iterator != sessions_.end()) {
                sessions_.erase(iterator);
            }
            send_redirect(client_fd, tls, "/login");
            return false;
        }
        iterator->second.expires_at = now + kSessionLifetime;
        if (request_path == "/overview") {
            request_path = "/overview.html";
        }
    }

    if (parsed.method != "GET") {
        send_method_not_allowed(client_fd, tls, "GET");
        return false;
    }

    std::filesystem::path file;
    if (!resolve_file(document_root_, request_path, file)) {
        send_response(client_fd, tls, 404, "text/plain; charset=utf-8", "not found\n");
        return false;
    }

    std::ifstream input(file, std::ios::binary);
    if (!input) {
        send_error(client_fd, tls, 500, "internal server error");
        return false;
    }
    std::ostringstream contents;
    contents << input.rdbuf();
    if (input.bad()) {
        send_error(client_fd, tls, 500, "internal server error");
        return false;
    }

    const std::string body = contents.str();
    send_response(client_fd, tls, 200, content_type(file), body);
    return false;
}

}  // namespace uhf::web
