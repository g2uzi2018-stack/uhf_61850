// SPDX-License-Identifier: GPL-3.0-only
#include "web/http_server.hpp"

#include "app/build_info.hpp"
#include "web/crypto.hpp"

#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <netinet/in.h>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include <unordered_map>
#include <utility>

namespace {

constexpr std::size_t kMaxHeaderBytes = 16U * 1024U;
constexpr std::size_t kMaxBodyBytes = 64U * 1024U;
constexpr std::size_t kMaxRequestBytes = kMaxHeaderBytes + kMaxBodyBytes;
constexpr std::uint16_t kListenBacklog = 16;
constexpr std::uintmax_t kMaxResponseBytes = 512U * 1024U;
constexpr std::size_t kMaxSessions = 64;
constexpr std::size_t kMaxLoginFailureRecords = 64;
constexpr std::size_t kMaxPasswordJsonBytes = 256;
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

bool send_all(int client_fd, std::string_view data) {
    std::size_t sent = 0;
    while (sent < data.size()) {
        const ssize_t result =
            ::send(client_fd, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
        if (result <= 0) {
            return false;
        }
        sent += static_cast<std::size_t>(result);
    }
    return true;
}

bool read_request(int client_fd, std::string& request) {
    char buffer[1024];
    std::size_t header_end = std::string::npos;
    while ((header_end = request.find("\r\n\r\n")) == std::string::npos) {
        const ssize_t received = ::recv(client_fd, buffer, sizeof(buffer), 0);
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
        const ssize_t received = ::recv(client_fd, buffer, sizeof(buffer), 0);
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
    default:
        return "Error";
    }
}

void send_response(
    int client_fd,
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
    send_all(client_fd, headers);
    send_all(client_fd, body);
}

void send_json(
    int client_fd,
    int status,
    std::string_view body,
    std::string_view extra_headers = {}) {
    send_response(client_fd, status, "application/json; charset=utf-8", body, extra_headers);
}

void send_error(
    int client_fd,
    int status,
    std::string_view message,
    std::string_view extra_headers = {}) {
    const std::string body = "{\"error\":\"" + std::string(message) + "\"}\n";
    send_json(client_fd, status, body, extra_headers);
}

void send_method_not_allowed(int client_fd, std::string_view allowed_methods) {
    const std::string headers = "Allow: " + std::string(allowed_methods) + "\r\n";
    send_error(client_fd, 405, "method not allowed", headers);
}

void send_redirect(int client_fd, std::string_view location) {
    const std::string headers = "Location: " + std::string(location) + "\r\n";
    send_response(client_fd, 302, "text/plain; charset=utf-8", "redirecting\n", headers);
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

std::string session_cookie_header(std::string_view token) {
    return "Set-Cookie: uhf_session=" + std::string(token) +
        "; Max-Age=1800; Path=/; HttpOnly; SameSite=Strict\r\n";
}

std::string expired_session_cookie_header() {
    return "Set-Cookie: uhf_session=; Max-Age=0; Path=/; HttpOnly; SameSite=Strict\r\n";
}

}  // namespace

namespace uhf::web {

HttpServer::HttpServer(
    std::filesystem::path document_root,
    std::string bind_address,
    std::uint16_t port,
    std::filesystem::path state_directory)
    : document_root_(std::move(document_root)),
      bind_address_(std::move(bind_address)),
      port_(port),
      auth_store_(std::move(state_directory)) {
    std::error_code error;
    document_root_ = std::filesystem::weakly_canonical(document_root_, error);
    if (error || !std::filesystem::is_directory(document_root_, error) || error) {
        throw std::invalid_argument("web root is not a directory");
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

    std::cout << uhf::app::kProductName << " web listening on http://" << bind_address_ << ":"
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

        char remote_address[INET_ADDRSTRLEN]{};
        const char* converted = ::inet_ntop(
            AF_INET, &client_address.sin_addr, remote_address, sizeof(remote_address));
        handle_client(client_fd, converted == nullptr ? "unknown" : std::string(converted));
        ::close(client_fd);
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

void HttpServer::handle_client(int client_fd, std::string remote_address) {
    std::string request;
    if (!read_request(client_fd, request)) {
        send_error(client_fd, 400, "bad request");
        return;
    }

    ParsedRequest parsed;
    if (!parse_request(request, parsed) ||
        (parsed.version != "HTTP/1.0" && parsed.version != "HTTP/1.1")) {
        send_error(client_fd, 400, "bad request");
        return;
    }

    std::string request_path;
    if (!decode_path(parsed.target, request_path)) {
        send_error(client_fd, 400, "bad path");
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    cleanup_sessions(now);

    if (request_path == "/healthz") {
        if (parsed.method != "GET") {
            send_method_not_allowed(client_fd, "GET");
            return;
        }
        std::string body{"{\"status\":\"degraded\",\"version\":\""};
        body.append(uhf::app::kVersion.data(), uhf::app::kVersion.size());
        body.append("\",\"web_auth\":\"ready\"}\n");
        send_json(client_fd, 200, body);
        return;
    }

    if (request_path == "/api/v1/session" || request_path == "/api/v1/password" ||
        request_path == "/api/v1/session/revoke-others") {
        if (request_path == "/api/v1/session" && parsed.method == "POST") {
            const std::string_view origin = header_value(parsed, "origin");
            const std::string_view host = header_value(parsed, "host");
            if (origin.empty() || host.empty() || origin != "http://" + std::string(host)) {
                send_error(client_fd, 403, "same-origin request required");
                return;
            }

            const auto failure_iterator = login_failures_.find(remote_address);
            if (failure_iterator != login_failures_.end() &&
                failure_iterator->second.blocked_until > now) {
                send_error(
                    client_fd,
                    429,
                    "too many login attempts",
                    "Retry-After: 30\r\nCache-Control: no-store\r\n");
                return;
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
                send_error(client_fd, 400, "invalid login request");
                return;
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
                send_error(client_fd, 401, "invalid username or password");
                return;
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
            const std::string headers = session_cookie_header(session_token) +
                "Cache-Control: no-store\r\n";
            send_json(client_fd, 200, session_json(csrf_token, auth_store_.must_change()), headers);
            return;
        }

        if (request_path == "/api/v1/session" && parsed.method == "GET") {
            const std::string token = session_cookie(parsed);
            const auto iterator = sessions_.find(token);
            if (token.empty() || iterator == sessions_.end() || iterator->second.expires_at <= now) {
                if (iterator != sessions_.end()) {
                    sessions_.erase(iterator);
                }
                send_error(client_fd, 401, "authentication required");
                return;
            }
            iterator->second.expires_at = now + kSessionLifetime;
            send_json(client_fd, 200, session_json(iterator->second.csrf_token, auth_store_.must_change()));
            return;
        }

        if (request_path == "/api/v1/session" && parsed.method == "DELETE") {
            const std::string token = session_cookie(parsed);
            const auto iterator = sessions_.find(token);
            if (token.empty() || iterator == sessions_.end() || iterator->second.expires_at <= now) {
                if (iterator != sessions_.end()) {
                    sessions_.erase(iterator);
                }
                send_error(client_fd, 401, "authentication required");
                return;
            }
            const std::string_view csrf = header_value(parsed, "x-csrf-token");
            if (!constant_time_equal(csrf, iterator->second.csrf_token)) {
                send_error(client_fd, 403, "CSRF token required");
                return;
            }
            sessions_.erase(iterator);
            send_response(
                client_fd,
                204,
                "application/json; charset=utf-8",
                {},
                expired_session_cookie_header());
            return;
        }

        if (request_path == "/api/v1/password" && parsed.method == "PUT") {
            const std::string token = session_cookie(parsed);
            const auto iterator = sessions_.find(token);
            if (token.empty() || iterator == sessions_.end() || iterator->second.expires_at <= now) {
                if (iterator != sessions_.end()) {
                    sessions_.erase(iterator);
                }
                send_error(client_fd, 401, "authentication required");
                return;
            }
            const std::string_view csrf = header_value(parsed, "x-csrf-token");
            if (!constant_time_equal(csrf, iterator->second.csrf_token)) {
                send_error(client_fd, 403, "CSRF token required");
                return;
            }

            std::string current_password;
            std::string new_password;
            if (!json_string_field(
                    parsed.body, "current_password", current_password, kMaxPasswordJsonBytes) ||
                !json_string_field(
                    parsed.body, "new_password", new_password, kMaxPasswordJsonBytes)) {
                send_error(client_fd, 400, "invalid password request");
                return;
            }
            const PasswordChangeResult result =
                auth_store_.change_password(current_password, new_password);
            if (result == PasswordChangeResult::invalid_current_password) {
                send_error(client_fd, 401, "current password is incorrect");
                return;
            }
            if (result == PasswordChangeResult::invalid_new_password) {
                send_error(client_fd, 400, "new password does not meet policy");
                return;
            }
            if (result == PasswordChangeResult::storage_error) {
                send_error(client_fd, 500, "unable to save password");
                return;
            }

            sessions_.clear();
            send_json(
                client_fd,
                200,
                "{\"changed\":true,\"reauthenticate\":true}\n",
                expired_session_cookie_header() + "Cache-Control: no-store\r\n");
            return;
        }

        if (request_path == "/api/v1/session/revoke-others" && parsed.method == "POST") {
            const std::string token = session_cookie(parsed);
            const auto iterator = sessions_.find(token);
            if (token.empty() || iterator == sessions_.end() || iterator->second.expires_at <= now) {
                if (iterator != sessions_.end()) {
                    sessions_.erase(iterator);
                }
                send_error(client_fd, 401, "authentication required");
                return;
            }
            const std::string_view csrf = header_value(parsed, "x-csrf-token");
            if (!constant_time_equal(csrf, iterator->second.csrf_token)) {
                send_error(client_fd, 403, "CSRF token required");
                return;
            }
            for (auto session_iterator = sessions_.begin(); session_iterator != sessions_.end();) {
                if (session_iterator->first == token) {
                    ++session_iterator;
                } else {
                    session_iterator = sessions_.erase(session_iterator);
                }
            }
            send_json(client_fd, 200, "{\"revoked\":true}\n");
            return;
        }

        if (request_path == "/api/v1/session") {
            send_method_not_allowed(client_fd, "GET, POST, DELETE");
        } else if (request_path == "/api/v1/password") {
            send_method_not_allowed(client_fd, "PUT");
        } else {
            send_method_not_allowed(client_fd, "POST");
        }
        return;
    }

    if (request_path.rfind("/api/", 0) == 0) {
        send_error(client_fd, 404, "not found");
        return;
    }

    if (request_path == "/login" || request_path == "/login.html") {
        if (parsed.method != "GET") {
            send_method_not_allowed(client_fd, "GET");
            return;
        }
        request_path = "/login.html";
    } else if (request_path == "/index.html") {
        if (parsed.method != "GET") {
            send_method_not_allowed(client_fd, "GET");
            return;
        }
        const std::string token = session_cookie(parsed);
        const auto iterator = sessions_.find(token);
        if (token.empty() || iterator == sessions_.end() || iterator->second.expires_at <= now) {
            if (iterator != sessions_.end()) {
                sessions_.erase(iterator);
            }
            send_redirect(client_fd, "/login");
            return;
        }
        iterator->second.expires_at = now + kSessionLifetime;
    }

    if (parsed.method != "GET") {
        send_method_not_allowed(client_fd, "GET");
        return;
    }

    std::filesystem::path file;
    if (!resolve_file(document_root_, request_path, file)) {
        send_response(client_fd, 404, "text/plain; charset=utf-8", "not found\n");
        return;
    }

    std::ifstream input(file, std::ios::binary);
    if (!input) {
        send_error(client_fd, 500, "internal server error");
        return;
    }
    std::ostringstream contents;
    contents << input.rdbuf();
    if (input.bad()) {
        send_error(client_fd, 500, "internal server error");
        return;
    }

    const std::string body = contents.str();
    send_response(client_fd, 200, content_type(file), body);
}

}  // namespace uhf::web
