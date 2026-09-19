// SPDX-License-Identifier: GPL-3.0-only
#include "acquisition/acquisition.hpp"

#include <fcntl.h>
#include <iostream>
#include <pty.h>
#include <stdexcept>
#include <termios.h>
#include <unistd.h>

namespace {

void check(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
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
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "serial port smoke failed: " << error.what() << '\n';
        return 1;
    }
}
