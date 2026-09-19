// SPDX-License-Identifier: GPL-3.0-only
#include "acquisition/acquisition.hpp"

#include <array>
#include <chrono>
#include <filesystem>
#include <fcntl.h>
#include <iostream>
#include <poll.h>
#include <pty.h>
#include <stdexcept>
#include <string>
#include <termios.h>
#include <unistd.h>

namespace {

void check(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

class TemporaryDirectory {
public:
    TemporaryDirectory()
        : path_(std::filesystem::temp_directory_path() /
                ("uhf-serial-reconnect-" + std::to_string(::getpid()))) {
        check(std::filesystem::create_directory(path_), "create temporary directory");
    }

    ~TemporaryDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

void expect_byte(int file_descriptor, std::uint8_t expected, const char* message) {
    pollfd descriptor{file_descriptor, POLLIN, 0};
    check(::poll(&descriptor, 1, 500) == 1, message);
    std::uint8_t actual = 0U;
    check(::read(file_descriptor, &actual, 1U) == 1 && actual == expected, message);
}

}  // namespace

int main() {
    try {
        bool valid = false;
        const auto absent = uhf::acquisition::parse_serial_profile("unconfigured", valid);
        check(valid && !absent, "unconfigured profile");
        const auto parsed = uhf::acquisition::parse_serial_profile("9600/7E2", valid);
        check(valid && parsed && parsed->baud == 9600U && parsed->data_bits == 7U &&
                  parsed->parity == uhf::acquisition::SerialParity::even &&
                  parsed->stop_bits == 2U,
              "configured profile");
        (void)uhf::acquisition::parse_serial_profile("12000/8N1", valid);
        check(!valid, "unsupported baud rejected");
        (void)uhf::acquisition::parse_serial_profile("115200/8X1", valid);
        check(!valid, "invalid parity rejected");

        int master = -1;
        int slave = -1;
        char slave_name[128]{};
        check(::openpty(&master, &slave, slave_name, nullptr, nullptr) == 0,
              "open PTY");
        check(::close(slave) == 0, "close original PTY slave");

        {
            uhf::acquisition::PosixSerialPort port(slave_name);
            termios attributes{};
            check(::tcgetattr(master, &attributes) == 0, "read default termios");
            check(::cfgetispeed(&attributes) == B115200, "default input baud");
            check(::cfgetospeed(&attributes) == B115200, "default output baud");
            check((attributes.c_cflag & CSIZE) == CS8, "default data bits");
            check((attributes.c_cflag & PARENB) == 0, "default parity");
            check((attributes.c_cflag & CSTOPB) == 0, "default stop bits");
            check((attributes.c_cflag & CLOCAL) != 0, "local serial mode");
            check((attributes.c_cflag & CREAD) != 0, "serial receiver enabled");
#ifdef CRTSCTS
            check((attributes.c_cflag & CRTSCTS) == 0, "hardware flow control disabled");
#endif
            check(port.write_all(nullptr, 0U), "zero-length write");
        }

        {
            uhf::acquisition::SerialSettings settings;
            settings.baud = 9600U;
            settings.stop_bits = 2U;
            uhf::acquisition::PosixSerialPort port(slave_name, settings);
            termios attributes{};
            check(::tcgetattr(master, &attributes) == 0, "read custom termios");
            check(::cfgetispeed(&attributes) == B9600, "custom input baud");
            check(::cfgetospeed(&attributes) == B9600, "custom output baud");
            check((attributes.c_cflag & CSIZE) == CS8, "custom data bits");
            check((attributes.c_cflag & CSTOPB) != 0, "custom stop bits");
        }

        check(::close(master) == 0, "close PTY master");

        TemporaryDirectory temporary;
        const std::filesystem::path device = temporary.path() / "serial-device";
        uhf::acquisition::ReconnectingSerialPort reconnecting(device.string());
        const std::uint8_t first = 0x31U;
        check(!reconnecting.write_all(&first, 1U), "missing tty is isolated");
        check(!reconnecting.connected(), "missing tty reports disconnected");

        int reconnect_master = -1;
        int reconnect_slave = -1;
        char reconnect_name[128]{};
        check(::openpty(
                  &reconnect_master, &reconnect_slave, reconnect_name, nullptr, nullptr) == 0,
              "open first reconnect PTY");
        check(::close(reconnect_slave) == 0, "close first reconnect PTY slave");
        std::filesystem::create_symlink(reconnect_name, device);
        check(reconnecting.write_all(&first, 1U), "connect when tty appears");
        check(reconnecting.connected(), "appeared tty reports connected");
        expect_byte(reconnect_master, first, "first reconnect write");

        check(::close(reconnect_master) == 0, "disconnect first reconnect PTY");
        std::array<std::uint8_t, 1U> incoming{};
        std::size_t received = 0U;
        check(!reconnecting.read_some(
                  incoming.data(), incoming.size(), std::chrono::milliseconds(50), received),
              "detect disconnected tty");
        check(received == 0U, "disconnect does not publish bytes");
        check(!reconnecting.connected(), "failed tty reports disconnected");

        check(std::filesystem::remove(device), "remove stale tty link");
        reconnect_master = -1;
        reconnect_slave = -1;
        reconnect_name[0] = '\0';
        check(::openpty(
                  &reconnect_master, &reconnect_slave, reconnect_name, nullptr, nullptr) == 0,
              "open replacement reconnect PTY");
        check(::close(reconnect_slave) == 0, "close replacement reconnect PTY slave");
        std::filesystem::create_symlink(reconnect_name, device);
        const std::uint8_t second = 0x52U;
        check(reconnecting.write_all(&second, 1U), "reopen replacement tty");
        check(reconnecting.connected(), "replacement tty reports connected");
        expect_byte(reconnect_master, second, "replacement reconnect write");
        const std::uint8_t reply = 0x73U;
        check(::write(reconnect_master, &reply, 1U) == 1, "write reconnect reply");
        check(reconnecting.read_some(
                  incoming.data(), incoming.size(), std::chrono::milliseconds(250), received),
              "read replacement tty");
        check(received == 1U && incoming[0] == reply, "replacement reconnect reply");
        check(::close(reconnect_master) == 0, "close replacement reconnect PTY master");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "serial port smoke failed: " << error.what() << '\n';
        return 1;
    }
}
