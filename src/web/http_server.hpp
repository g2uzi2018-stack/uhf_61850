// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace uhf::web {

class HttpServer {
public:
    HttpServer(std::filesystem::path document_root, std::string bind_address, std::uint16_t port);

    int run() const;

private:
    void handle_client(int client_fd) const;

    std::filesystem::path document_root_;
    std::string bind_address_;
    std::uint16_t port_;
};

}  // namespace uhf::web
