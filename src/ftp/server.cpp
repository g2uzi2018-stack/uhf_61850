// SPDX-License-Identifier: GPL-3.0-only
#include "ftp/server.hpp"

#include <algorithm>
#include <arpa/inet.h>
#include <array>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <fcntl.h>
#include <filesystem>
#include <iomanip>
#include <netinet/in.h>
#include <optional>
#include <poll.h>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
#include <utility>

namespace {

constexpr std::size_t kMaximumControlBuffer = 4096U;
constexpr std::chrono::seconds kDataTimeout{30};

struct Listener {
    int file_descriptor{-1};
    std::uint16_t port{0U};
};

std::optional<Listener> open_listener(std::string_view bind_address, std::uint16_t port) {
    const int file_descriptor = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (file_descriptor < 0) {
        return std::nullopt;
    }
    const int reuse = 1;
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    if (::setsockopt(
            file_descriptor, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0 ||
        ::inet_pton(AF_INET, std::string(bind_address).c_str(), &address.sin_addr) != 1 ||
        ::bind(
            file_descriptor,
            reinterpret_cast<const sockaddr*>(&address),
            sizeof(address)) < 0 ||
        ::listen(file_descriptor, 8) < 0) {
        ::close(file_descriptor);
        return std::nullopt;
    }
    sockaddr_in bound{};
    socklen_t bound_size = sizeof(bound);
    if (::getsockname(
            file_descriptor,
            reinterpret_cast<sockaddr*>(&bound),
            &bound_size) < 0) {
        ::close(file_descriptor);
        return std::nullopt;
    }
    return Listener{file_descriptor, ntohs(bound.sin_port)};
}

bool send_all(int file_descriptor, std::string_view bytes) {
    std::size_t sent = 0U;
    while (sent < bytes.size()) {
        const ssize_t result = ::send(
            file_descriptor,
            bytes.data() + sent,
            bytes.size() - sent,
            MSG_NOSIGNAL);
        if (result > 0) {
            sent += static_cast<std::size_t>(result);
            continue;
        }
        if (result < 0 && errno == EINTR) {
            continue;
        }
        return false;
    }
    return true;
}

bool wait_readable(
    int file_descriptor,
    std::chrono::steady_clock::time_point deadline,
    const std::atomic<bool>& stop_requested) {
    while (!stop_requested.load()) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            return false;
        }
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - now);
        const int timeout = static_cast<int>(std::min<std::int64_t>(remaining.count(), 250));
        pollfd descriptor{file_descriptor, POLLIN, 0};
        const int result = ::poll(&descriptor, 1, std::max(timeout, 1));
        if (result > 0) {
            return (descriptor.revents & (POLLIN | POLLHUP)) != 0 &&
                (descriptor.revents & (POLLERR | POLLNVAL)) == 0;
        }
        if (result < 0 && errno != EINTR) {
            return false;
        }
    }
    return false;
}

bool read_line(
    int file_descriptor,
    std::string& buffered,
    std::string& line,
    const std::atomic<bool>& stop_requested) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    while (!stop_requested.load()) {
        const std::size_t end = buffered.find("\r\n");
        if (end != std::string::npos) {
            line = buffered.substr(0U, end);
            buffered.erase(0U, end + 2U);
            return line.size() <= 1024U;
        }
        if (buffered.size() >= kMaximumControlBuffer ||
            !wait_readable(file_descriptor, deadline, stop_requested)) {
            return false;
        }
        std::array<char, 512U> bytes{};
        const ssize_t received = ::recv(file_descriptor, bytes.data(), bytes.size(), 0);
        if (received > 0) {
            buffered.append(bytes.data(), static_cast<std::size_t>(received));
        } else if (received == 0 || errno != EINTR) {
            return false;
        }
    }
    return false;
}

std::string uppercase(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
        return static_cast<char>(std::toupper(character));
    });
    return value;
}

bool valid_filename(std::string_view filename) {
    if (filename.empty() || filename.size() > 128U || filename == "." || filename == ".." ||
        filename.front() == '.' || filename.find('/') != std::string_view::npos ||
        filename.find('\\') != std::string_view::npos) {
        return false;
    }
    return std::none_of(filename.begin(), filename.end(), [](unsigned char character) {
        return character < 0x20U || character == 0x7FU;
    });
}

std::optional<int> accept_data(
    int& passive_fd,
    in_addr_t expected_address,
    const std::atomic<bool>& stop_requested) {
    if (passive_fd < 0) {
        return std::nullopt;
    }
    const int listener = std::exchange(passive_fd, -1);
    const bool ready = wait_readable(
        listener, std::chrono::steady_clock::now() + std::chrono::seconds(10), stop_requested);
    if (!ready) {
        ::close(listener);
        return std::nullopt;
    }
    sockaddr_in peer{};
    socklen_t peer_size = sizeof(peer);
    const int result = ::accept4(
        listener, reinterpret_cast<sockaddr*>(&peer), &peer_size, SOCK_CLOEXEC);
    ::close(listener);
    if (result < 0 || peer.sin_family != AF_INET || peer.sin_addr.s_addr != expected_address) {
        if (result >= 0) {
            ::close(result);
        }
        return std::nullopt;
    }
    const timeval timeout{static_cast<time_t>(kDataTimeout.count()), 0};
    if (::setsockopt(result, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) < 0 ||
        ::setsockopt(result, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0) {
        ::close(result);
        return std::nullopt;
    }
    return result;
}

std::string list_files(
    const std::filesystem::path& root, bool names_only, std::size_t maximum_entries) {
    struct Entry {
        std::string name;
        std::uintmax_t size;
    };
    std::vector<Entry> entries;
    std::error_code error;
    for (std::filesystem::directory_iterator iterator(root, error), end;
         !error && iterator != end;
         iterator.increment(error)) {
        const std::filesystem::directory_entry& entry = *iterator;
        const std::string name = entry.path().filename().string();
        std::error_code status_error;
        if (!valid_filename(name) ||
            !std::filesystem::is_regular_file(entry.symlink_status(status_error)) || status_error) {
            continue;
        }
        const std::uintmax_t size = entry.file_size(status_error);
        if (!status_error) {
            entries.push_back(Entry{name, size});
            if (entries.size() >= maximum_entries) {
                break;
            }
        }
    }
    std::sort(entries.begin(), entries.end(), [](const Entry& left, const Entry& right) {
        return left.name < right.name;
    });
    std::ostringstream output;
    for (const Entry& entry : entries) {
        if (names_only) {
            output << entry.name << "\r\n";
        } else {
            output << "-rw------- 1 ftp ftp " << std::setw(12) << entry.size
                   << " Jan 01 00:00 " << entry.name << "\r\n";
        }
    }
    return output.str();
}

}  // namespace

namespace uhf::ftp {

struct Server::Worker {
    std::thread thread;
    std::shared_ptr<std::atomic<bool>> done;
};

Server::Server(ServerOptions options, PasswordVerifier password_verifier)
    : options_(std::move(options)), password_verifier_(std::move(password_verifier)) {
    if (options_.root.empty() || options_.max_connections == 0U ||
        options_.max_files == 0U || options_.max_file_bytes == 0U ||
        options_.max_total_bytes < options_.max_file_bytes || !password_verifier_) {
        throw std::invalid_argument("invalid FTP server options");
    }
    std::error_code error;
    const std::filesystem::file_status status =
        std::filesystem::symlink_status(options_.root, error);
    if (error && error != std::errc::no_such_file_or_directory) {
        throw std::runtime_error("unable to inspect FTP root");
    }
    if (std::filesystem::is_symlink(status)) {
        throw std::runtime_error("FTP root cannot be a symbolic link");
    }
    std::filesystem::create_directories(options_.root, error);
    if (error || !std::filesystem::is_directory(options_.root, error) || error ||
        ::chmod(options_.root.c_str(), S_IRWXU) < 0) {
        throw std::runtime_error("unable to prepare FTP root");
    }
    for (std::filesystem::directory_iterator iterator(options_.root, error), end;
         !error && iterator != end;
         iterator.increment(error)) {
        const std::string name = iterator->path().filename().string();
        if (name.rfind(".upload-", 0U) == 0U) {
            std::error_code remove_error;
            const std::filesystem::file_status upload_status =
                iterator->symlink_status(remove_error);
            if (!remove_error &&
                (std::filesystem::is_regular_file(upload_status) ||
                 std::filesystem::is_symlink(upload_status))) {
                std::filesystem::remove(iterator->path(), remove_error);
            }
        }
    }
}

Server::~Server() {
    stop();
}

bool Server::start() {
    if (listener_worker_.joinable()) {
        return running_.load();
    }
    const std::optional<Listener> listener =
        open_listener(options_.bind_address, options_.port);
    if (!listener) {
        return false;
    }
    listener_fd_ = listener->file_descriptor;
    bound_port_.store(listener->port);
    stop_requested_.store(false);
    running_.store(true);
    try {
        listener_worker_ = std::thread(&Server::run, this);
    } catch (...) {
        ::close(listener_fd_);
        listener_fd_ = -1;
        bound_port_.store(0U);
        running_.store(false);
        return false;
    }
    return true;
}

void Server::stop() noexcept {
    stop_requested_.store(true);
    if (listener_fd_ >= 0) {
        ::shutdown(listener_fd_, SHUT_RDWR);
    }
    if (listener_worker_.joinable()) {
        listener_worker_.join();
    }
    if (listener_fd_ >= 0) {
        ::close(listener_fd_);
    }
    listener_fd_ = -1;
    {
        std::lock_guard<std::mutex> lock(clients_mutex_);
        for (const int client_fd : client_fds_) {
            ::shutdown(client_fd, SHUT_RDWR);
        }
    }
    reap_workers(true);
    running_.store(false);
    bound_port_.store(0U);
}

bool Server::running() const noexcept {
    return running_.load();
}

std::uint16_t Server::bound_port() const noexcept {
    return bound_port_.load();
}

void Server::track_client(int client_fd) {
    std::lock_guard<std::mutex> lock(clients_mutex_);
    client_fds_.insert(client_fd);
}

void Server::untrack_client(int client_fd) {
    std::lock_guard<std::mutex> lock(clients_mutex_);
    client_fds_.erase(client_fd);
}

void Server::reap_workers(bool all) {
    for (auto iterator = workers_.begin(); iterator != workers_.end();) {
        if (all || (*iterator)->done->load()) {
            if ((*iterator)->thread.joinable()) {
                (*iterator)->thread.join();
            }
            iterator = workers_.erase(iterator);
        } else {
            ++iterator;
        }
    }
}

void Server::run() {
    while (!stop_requested_.load()) {
        reap_workers(false);
        pollfd descriptor{listener_fd_, POLLIN, 0};
        const int poll_result = ::poll(&descriptor, 1, 100);
        if (poll_result < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        if (poll_result == 0 || (descriptor.revents & POLLIN) == 0) {
            continue;
        }
        const int client_fd = ::accept4(listener_fd_, nullptr, nullptr, SOCK_CLOEXEC);
        if (client_fd < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (!stop_requested_.load()) {
                continue;
            }
            break;
        }
        if (workers_.size() >= options_.max_connections) {
            (void)send_all(client_fd, "421 Too many connections\r\n");
            ::close(client_fd);
            continue;
        }
        auto done = std::make_shared<std::atomic<bool>>(false);
        auto worker = std::make_unique<Worker>();
        worker->done = done;
        track_client(client_fd);
        try {
            worker->thread = std::thread([this, client_fd, done] {
                serve_client(client_fd);
                untrack_client(client_fd);
                ::close(client_fd);
                done->store(true);
            });
            workers_.push_back(std::move(worker));
        } catch (...) {
            untrack_client(client_fd);
            ::close(client_fd);
        }
    }
    running_.store(false);
}

void Server::serve_client(int client_fd) {
    if (!send_all(client_fd, "220 UHF gateway FTP ready\r\n")) {
        return;
    }
    std::string buffered;
    std::string username;
    bool authenticated = false;
    unsigned int failed_passwords = 0U;
    int passive_fd = -1;
    sockaddr_in control_peer{};
    socklen_t control_peer_size = sizeof(control_peer);
    if (::getpeername(
            client_fd,
            reinterpret_cast<sockaddr*>(&control_peer),
            &control_peer_size) < 0 || control_peer.sin_family != AF_INET) {
        return;
    }
    const auto close_passive = [&passive_fd] {
        if (passive_fd >= 0) {
            ::close(passive_fd);
            passive_fd = -1;
        }
    };

    std::string line;
    while (read_line(client_fd, buffered, line, stop_requested_)) {
        const std::size_t separator = line.find(' ');
        const std::string command = uppercase(line.substr(0U, separator));
        const std::string argument = separator == std::string::npos
            ? std::string{}
            : line.substr(separator + 1U);

        if (command == "QUIT") {
            (void)send_all(client_fd, "221 Goodbye\r\n");
            break;
        }
        if (command == "NOOP") {
            (void)send_all(client_fd, "200 OK\r\n");
            continue;
        }
        if (command == "SYST") {
            (void)send_all(client_fd, "215 UNIX Type: L8\r\n");
            continue;
        }
        if (command == "FEAT") {
            (void)send_all(client_fd, "211-Features\r\n EPSV\r\n PASV\r\n SIZE\r\n UTF8\r\n211 End\r\n");
            continue;
        }
        if (command == "OPTS" && uppercase(argument) == "UTF8 ON") {
            (void)send_all(client_fd, "200 UTF8 enabled\r\n");
            continue;
        }
        if (command == "USER") {
            authenticated = false;
            username = argument;
            if (username != "admin") {
                (void)send_all(client_fd, "530 Login incorrect\r\n");
            } else {
                (void)send_all(client_fd, "331 Password required\r\n");
            }
            continue;
        }
        if (command == "PASS") {
            if (username == "admin" && password_verifier_(username, argument)) {
                authenticated = true;
                failed_passwords = 0U;
                (void)send_all(client_fd, "230 Login successful\r\n");
            } else {
                authenticated = false;
                ++failed_passwords;
                (void)send_all(client_fd, "530 Login incorrect\r\n");
                if (failed_passwords >= 3U) {
                    break;
                }
            }
            continue;
        }
        if (!authenticated) {
            (void)send_all(client_fd, "530 Please login with USER and PASS\r\n");
            continue;
        }
        if (command == "TYPE") {
            (void)send_all(
                client_fd, argument == "I" || argument == "A" ? "200 Type set\r\n"
                                                               : "504 Type not supported\r\n");
            continue;
        }
        if (command == "PWD") {
            (void)send_all(client_fd, "257 \"/\" is the current directory\r\n");
            continue;
        }
        if (command == "CWD" || command == "CDUP") {
            if (command == "CDUP" || argument == "/" || argument == "." || argument.empty()) {
                (void)send_all(client_fd, "250 Directory changed\r\n");
            } else {
                (void)send_all(client_fd, "550 Only the FTP root is available\r\n");
            }
            continue;
        }
        if (command == "PASV" || command == "EPSV") {
            close_passive();
            const std::optional<Listener> listener = open_listener("0.0.0.0", 0U);
            if (!listener) {
                (void)send_all(client_fd, "425 Cannot open passive connection\r\n");
                continue;
            }
            passive_fd = listener->file_descriptor;
            if (command == "EPSV") {
                (void)send_all(
                    client_fd,
                    "229 Entering Extended Passive Mode (|||" +
                        std::to_string(listener->port) + "|)\r\n");
            } else {
                sockaddr_in local{};
                socklen_t local_size = sizeof(local);
                if (::getsockname(
                        client_fd,
                        reinterpret_cast<sockaddr*>(&local),
                        &local_size) < 0) {
                    close_passive();
                    (void)send_all(client_fd, "425 Cannot determine passive address\r\n");
                    continue;
                }
                const std::uint32_t ip = ntohl(local.sin_addr.s_addr);
                std::ostringstream response;
                response << "227 Entering Passive Mode (" << ((ip >> 24U) & 0xFFU) << ','
                         << ((ip >> 16U) & 0xFFU) << ',' << ((ip >> 8U) & 0xFFU) << ','
                         << (ip & 0xFFU) << ',' << (listener->port / 256U) << ','
                         << (listener->port % 256U) << ")\r\n";
                (void)send_all(client_fd, response.str());
            }
            continue;
        }
        if (command == "SIZE") {
            if (!valid_filename(argument)) {
                (void)send_all(client_fd, "550 Invalid filename\r\n");
                continue;
            }
            const std::filesystem::path path = options_.root / argument;
            const int file_descriptor = ::open(
                path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
            struct stat status{};
            if (file_descriptor < 0 || ::fstat(file_descriptor, &status) < 0 ||
                !S_ISREG(status.st_mode) || status.st_size < 0 ||
                static_cast<std::uint64_t>(status.st_size) > options_.max_file_bytes) {
                if (file_descriptor >= 0) {
                    ::close(file_descriptor);
                }
                (void)send_all(client_fd, "550 File unavailable\r\n");
            } else {
                ::close(file_descriptor);
                (void)send_all(
                    client_fd, "213 " + std::to_string(status.st_size) + "\r\n");
            }
            continue;
        }
        if (command == "LIST" || command == "NLST") {
            if (passive_fd < 0) {
                (void)send_all(client_fd, "425 Use PASV or EPSV first\r\n");
                continue;
            }
            (void)send_all(client_fd, "150 Opening data connection\r\n");
            const std::optional<int> data_fd = accept_data(
                passive_fd, control_peer.sin_addr.s_addr, stop_requested_);
            if (!data_fd) {
                (void)send_all(client_fd, "426 Data connection failed\r\n");
                continue;
            }
            track_client(*data_fd);
            const bool sent = send_all(
                *data_fd,
                list_files(options_.root, command == "NLST", options_.max_files));
            untrack_client(*data_fd);
            ::close(*data_fd);
            (void)send_all(
                client_fd, sent ? "226 Transfer complete\r\n" : "426 Transfer failed\r\n");
            continue;
        }
        if (command == "RETR") {
            if (!valid_filename(argument) || passive_fd < 0) {
                (void)send_all(
                    client_fd,
                    passive_fd < 0 ? "425 Use PASV or EPSV first\r\n"
                                   : "550 Invalid filename\r\n");
                continue;
            }
            const std::filesystem::path path = options_.root / argument;
            const int file_descriptor = ::open(
                path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
            struct stat status{};
            if (file_descriptor < 0 || ::fstat(file_descriptor, &status) < 0 ||
                !S_ISREG(status.st_mode) || status.st_size < 0 ||
                static_cast<std::uint64_t>(status.st_size) > options_.max_file_bytes) {
                if (file_descriptor >= 0) {
                    ::close(file_descriptor);
                }
                (void)send_all(client_fd, "550 File unavailable\r\n");
                continue;
            }
            (void)send_all(client_fd, "150 Opening data connection\r\n");
            const std::optional<int> data_fd = accept_data(
                passive_fd, control_peer.sin_addr.s_addr, stop_requested_);
            bool sent = data_fd.has_value();
            if (data_fd) {
                track_client(*data_fd);
            }
            std::array<char, 8192U> bytes{};
            while (sent) {
                const ssize_t read_count = ::read(file_descriptor, bytes.data(), bytes.size());
                if (read_count == 0) {
                    break;
                }
                if (read_count < 0) {
                    if (errno == EINTR) {
                        continue;
                    }
                    sent = false;
                    break;
                }
                sent = send_all(
                    *data_fd,
                    std::string_view(bytes.data(), static_cast<std::size_t>(read_count)));
            }
            ::close(file_descriptor);
            if (data_fd) {
                untrack_client(*data_fd);
                ::close(*data_fd);
            }
            (void)send_all(
                client_fd, sent ? "226 Transfer complete\r\n" : "426 Transfer failed\r\n");
            continue;
        }
        if (command == "STOR") {
            if (!valid_filename(argument) || passive_fd < 0) {
                (void)send_all(
                    client_fd,
                    passive_fd < 0 ? "425 Use PASV or EPSV first\r\n"
                                   : "550 Invalid filename\r\n");
                continue;
            }
            std::unique_lock<std::mutex> storage_lock(storage_mutex_);
            std::uint64_t bytes_in_other_files = 0U;
            std::size_t other_file_count = 0U;
            std::error_code usage_error;
            for (std::filesystem::directory_iterator iterator(options_.root, usage_error), end;
                 !usage_error && iterator != end;
                 iterator.increment(usage_error)) {
                const std::string name = iterator->path().filename().string();
                std::error_code entry_error;
                if (name == argument || !valid_filename(name) ||
                    !std::filesystem::is_regular_file(iterator->symlink_status(entry_error)) ||
                    entry_error) {
                    continue;
                }
                const std::uintmax_t size = iterator->file_size(entry_error);
                if (entry_error || bytes_in_other_files > options_.max_total_bytes ||
                    size > options_.max_total_bytes - bytes_in_other_files) {
                    usage_error = std::make_error_code(std::errc::file_too_large);
                    break;
                }
                bytes_in_other_files += static_cast<std::uint64_t>(size);
                ++other_file_count;
            }
            if (usage_error || other_file_count >= options_.max_files ||
                bytes_in_other_files >= options_.max_total_bytes) {
                (void)send_all(client_fd, "552 FTP storage quota exceeded\r\n");
                continue;
            }
            const std::uint64_t upload_limit = std::min(
                options_.max_file_bytes, options_.max_total_bytes - bytes_in_other_files);
            const std::filesystem::path temporary = options_.root /
                (".upload-" + std::to_string(static_cast<long long>(::getpid())) + "-" +
                 std::to_string(upload_sequence_.fetch_add(1U)));
            const int file_descriptor = ::open(
                temporary.c_str(),
                O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
                S_IRUSR | S_IWUSR);
            if (file_descriptor < 0) {
                (void)send_all(client_fd, "550 Cannot create upload\r\n");
                continue;
            }
            (void)send_all(client_fd, "150 Opening data connection\r\n");
            const std::optional<int> data_fd = accept_data(
                passive_fd, control_peer.sin_addr.s_addr, stop_requested_);
            bool received_ok = data_fd.has_value();
            if (data_fd) {
                track_client(*data_fd);
            }
            std::uint64_t total = 0U;
            std::array<char, 8192U> bytes{};
            while (received_ok && !stop_requested_.load()) {
                if (!wait_readable(
                        *data_fd,
                        std::chrono::steady_clock::now() + kDataTimeout,
                        stop_requested_)) {
                    received_ok = false;
                    break;
                }
                const ssize_t received = ::recv(*data_fd, bytes.data(), bytes.size(), 0);
                if (received == 0) {
                    break;
                }
                if (received < 0) {
                    if (errno == EINTR) {
                        continue;
                    }
                    received_ok = false;
                    break;
                }
                total += static_cast<std::uint64_t>(received);
                if (total > upload_limit) {
                    received_ok = false;
                    break;
                }
                std::size_t written = 0U;
                while (written < static_cast<std::size_t>(received)) {
                    const ssize_t result = ::write(
                        file_descriptor,
                        bytes.data() + written,
                        static_cast<std::size_t>(received) - written);
                    if (result > 0) {
                        written += static_cast<std::size_t>(result);
                    } else if (result < 0 && errno == EINTR) {
                        continue;
                    } else {
                        received_ok = false;
                        break;
                    }
                }
            }
            if (data_fd) {
                untrack_client(*data_fd);
                ::close(*data_fd);
            }
            const bool persisted = received_ok && ::fsync(file_descriptor) == 0 &&
                ::close(file_descriptor) == 0 &&
                ::rename(temporary.c_str(), (options_.root / argument).c_str()) == 0;
            if (persisted) {
                const int directory_fd = ::open(
                    options_.root.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
                if (directory_fd >= 0) {
                    (void)::fsync(directory_fd);
                    ::close(directory_fd);
                }
            }
            if (!persisted) {
                ::close(file_descriptor);
                ::unlink(temporary.c_str());
            }
            (void)send_all(
                client_fd,
                persisted ? "226 Transfer complete\r\n" : "552 Upload failed or too large\r\n");
            continue;
        }
        (void)send_all(client_fd, "502 Command not implemented\r\n");
    }
    close_passive();
}

}  // namespace uhf::ftp
