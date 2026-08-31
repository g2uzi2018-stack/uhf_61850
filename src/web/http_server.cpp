// SPDX-License-Identifier: GPL-3.0-only
#include "web/http_server.hpp"

#include "app/build_info.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <netinet/in.h>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include <utility>

namespace {

constexpr std::size_t kMaxRequestBytes = 8192;
constexpr std::uint16_t kListenBacklog = 16;
constexpr std::uintmax_t kMaxResponseBytes = 512U * 1024U;

struct ParsedRequest {
    std::string method;
    std::string target;
    std::string version;
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
    while (request.find("\r\n\r\n") == std::string::npos) {
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
    const std::size_t line_end = request.find("\r\n");
    if (line_end == std::string_view::npos) {
        return false;
    }

    std::istringstream request_line{std::string(request.substr(0, line_end))};
    return static_cast<bool>(request_line >> parsed.method >> parsed.target >> parsed.version);
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

        if (index + 2 >= encoded_path.size()) {
            return false;
        }
        const int high = hex_value(encoded_path[index + 1]);
        const int low = hex_value(encoded_path[index + 2]);
        if (high < 0 || low < 0) {
            return false;
        }
        path.push_back(static_cast<char>((high << 4) | low));
        index += 2;
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
        begin = end + 1;
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
    case 400:
        return "Bad Request";
    case 404:
        return "Not Found";
    case 405:
        return "Method Not Allowed";
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
        "\r\nConnection: close\r\nX-Content-Type-Options: nosniff\r\n";
    headers.append(extra_headers);
    headers.append("\r\n");
    send_all(client_fd, headers);
    send_all(client_fd, body);
}

}  // namespace

namespace uhf::web {

HttpServer::HttpServer(
    std::filesystem::path document_root, std::string bind_address, std::uint16_t port)
    : document_root_(std::move(document_root)),
      bind_address_(std::move(bind_address)),
      port_(port) {
    std::error_code error;
    document_root_ = std::filesystem::weakly_canonical(document_root_, error);
    if (error || !std::filesystem::is_directory(document_root_, error) || error) {
        throw std::invalid_argument("web root is not a directory");
    }
}

int HttpServer::run() const {
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
              << ntohs(bound_address.sin_port) << "/\n"
              << std::flush;

    while (true) {
        const int client_fd = ::accept(server_fd, nullptr, nullptr);
        if (client_fd < 0) {
            if (errno == EINTR) {
                continue;
            }
            std::perror("accept");
            ::close(server_fd);
            return 1;
        }
        handle_client(client_fd);
        ::close(client_fd);
    }
}

void HttpServer::handle_client(int client_fd) const {
    std::string request;
    if (!read_request(client_fd, request)) {
        send_response(client_fd, 400, "text/plain; charset=utf-8", "bad request\n");
        return;
    }

    ParsedRequest parsed;
    if (!parse_request(request, parsed) ||
        (parsed.version != "HTTP/1.0" && parsed.version != "HTTP/1.1")) {
        send_response(client_fd, 400, "text/plain; charset=utf-8", "bad request\n");
        return;
    }
    if (parsed.method != "GET") {
        send_response(
            client_fd, 405, "text/plain; charset=utf-8", "method not allowed\n", "Allow: GET\r\n");
        return;
    }

    std::string request_path;
    if (!decode_path(parsed.target, request_path)) {
        send_response(client_fd, 400, "text/plain; charset=utf-8", "bad path\n");
        return;
    }
    if (request_path == "/healthz") {
        std::string body{"{\"status\":\"degraded\",\"version\":\""};
        body.append(uhf::app::kVersion.data(), uhf::app::kVersion.size());
        body.append("\",\"web_auth\":\"pending\"}\n");
        send_response(client_fd, 200, "application/json; charset=utf-8", body);
        return;
    }

    std::filesystem::path file;
    if (!resolve_file(document_root_, request_path, file)) {
        send_response(client_fd, 404, "text/plain; charset=utf-8", "not found\n");
        return;
    }

    std::ifstream input(file, std::ios::binary);
    if (!input) {
        send_response(client_fd, 500, "text/plain; charset=utf-8", "internal server error\n");
        return;
    }
    std::ostringstream contents;
    contents << input.rdbuf();
    if (input.bad()) {
        send_response(client_fd, 500, "text/plain; charset=utf-8", "internal server error\n");
        return;
    }

    const std::string body = contents.str();
    send_response(client_fd, 200, content_type(file), body);
}

}  // namespace uhf::web
