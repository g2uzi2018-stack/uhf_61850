// SPDX-License-Identifier: GPL-3.0-only
#include "acquisition/acquisition.hpp"
#include "domain/modbus.hpp"
#include "modbus/rtu_server.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <fcntl.h>
#include <iostream>
#include <pty.h>
#include <poll.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "Modbus RTU smoke failed: " << message << '\n';
        return false;
    }
    return true;
}

std::vector<std::uint8_t> request(
    std::uint8_t unit_id, std::uint8_t function, std::uint16_t start, std::uint16_t count) {
    std::vector<std::uint8_t> frame{
        unit_id,
        function,
        static_cast<std::uint8_t>(start >> 8U),
        static_cast<std::uint8_t>(start & 0x00FFU),
        static_cast<std::uint8_t>(count >> 8U),
        static_cast<std::uint8_t>(count & 0x00FFU),
    };
    const std::uint16_t crc = uhf::domain::modbus_crc16(frame.data(), frame.size());
    frame.push_back(static_cast<std::uint8_t>(crc & 0x00FFU));
    frame.push_back(static_cast<std::uint8_t>(crc >> 8U));
    return frame;
}

bool read_exact(int file_descriptor, std::uint8_t* data, std::size_t size) {
    std::size_t offset = 0;
    while (offset < size) {
        pollfd descriptor{file_descriptor, POLLIN, 0};
        if (::poll(&descriptor, 1, 1000) <= 0) {
            return false;
        }
        const ssize_t received = ::read(file_descriptor, data + offset, size - offset);
        if (received <= 0) {
            return false;
        }
        offset += static_cast<std::size_t>(received);
    }
    return true;
}

bool write_frame(int file_descriptor, const std::vector<std::uint8_t>& frame) {
    return ::write(file_descriptor, frame.data(), frame.size()) ==
        static_cast<ssize_t>(frame.size());
}

}  // namespace

int main() {
    int master_fd = -1;
    int slave_fd = -1;
    char slave_name[128]{};
    if (!expect(
            ::openpty(&master_fd, &slave_fd, slave_name, nullptr, nullptr) == 0,
            "unable to create PTY")) {
        return 1;
    }
    uhf::acquisition::PosixSerialPort serial_port(slave_name);
    ::close(slave_fd);

    uhf::acquisition::SnapshotStore store;
    uhf::domain::ParsedSnapshot payload;
    payload.raw_registers.fill(0x2468U);
    payload.raw_registers.front() = 0xFFCEU;
    store.publish(payload, std::chrono::steady_clock::now(), std::chrono::steady_clock::now());

    uhf::modbus::ModbusRtuOptions options;
    options.unit_id = 9U;
    uhf::modbus::ModbusRtuServer server(serial_port, store, options);
    std::thread server_thread([&server]() { server.run(); });

    const std::vector<std::uint8_t> valid = request(9U, 0x04U, 10001U, 5U);
    if (!expect(::write(master_fd, valid.data(), valid.size()) == 8, "valid request write")) {
        server.stop();
        server_thread.join();
        ::close(master_fd);
        return 1;
    }
    std::array<std::uint8_t, 15U> response{};
    if (!expect(read_exact(master_fd, response.data(), response.size()), "valid response read") ||
        !expect(response[0] == 9U && response[1] == 0x04U && response[2] == 10U,
            "valid response header") ||
        !expect(response[3] == 0xFFU && response[4] == 0xCEU, "valid first register") ||
        !expect(uhf::domain::modbus_crc16(response.data(), response.size() - 2U) ==
                    static_cast<std::uint16_t>(response[13] | response[14] << 8U),
            "valid response CRC")) {
        server.stop();
        server_thread.join();
        ::close(master_fd);
        return 1;
    }

    const std::vector<std::uint8_t> invalid_address = request(9U, 0x04U, 10000U, 1U);
    if (!expect(write_frame(master_fd, invalid_address), "address request write")) {
        server.stop();
        server_thread.join();
        ::close(master_fd);
        return 1;
    }
    std::array<std::uint8_t, 5U> exception{};
    if (!expect(read_exact(master_fd, exception.data(), exception.size()), "address exception read") ||
        !expect(exception[1] == 0x84U && exception[2] == 0x02U, "address exception")) {
        server.stop();
        server_thread.join();
        ::close(master_fd);
        return 1;
    }

    std::vector<std::uint8_t> bad_crc = valid;
    bad_crc.back() ^= 0x01U;
    if (!expect(write_frame(master_fd, bad_crc), "bad CRC request write")) {
        server.stop();
        server_thread.join();
        ::close(master_fd);
        return 1;
    }
    pollfd descriptor{master_fd, POLLIN, 0};
    const int poll_result = ::poll(&descriptor, 1, 100);
    if (!expect(poll_result == 0, "bad CRC was answered")) {
        server.stop();
        server_thread.join();
        ::close(master_fd);
        return 1;
    }

    const std::vector<std::uint8_t> no_unit = request(8U, 0x04U, 10001U, 1U);
    if (!expect(write_frame(master_fd, no_unit), "wrong unit request write")) {
        server.stop();
        server_thread.join();
        ::close(master_fd);
        return 1;
    }
    descriptor.revents = 0;
    if (!expect(::poll(&descriptor, 1, 100) == 0, "wrong unit was answered")) {
        server.stop();
        server_thread.join();
        ::close(master_fd);
        return 1;
    }

    const std::vector<std::uint8_t> empty_request = request(9U, 0x04U, 10001U, 1U);
    uhf::acquisition::SnapshotStore empty_store;
    uhf::modbus::ModbusRtuServer empty_server(serial_port, empty_store, options);
    const std::vector<std::uint8_t> empty_response = empty_server.handle_request(empty_request);
    if (!expect(empty_response.size() == 5U, "empty snapshot response size") ||
        !expect(empty_response[1] == 0x84U && empty_response[2] == 0x04U,
            "empty snapshot device failure")) {
        server.stop();
        server_thread.join();
        ::close(master_fd);
        return 1;
    }

    server.stop();
    server_thread.join();
    ::close(master_fd);
    std::cout << "Modbus RTU smoke: OK\n";
    return 0;
}
