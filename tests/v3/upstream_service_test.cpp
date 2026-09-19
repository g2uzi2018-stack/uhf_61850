// SPDX-License-Identifier: GPL-3.0-only
#include "modbus/tcp_server.hpp"
#include "test_support.hpp"
#include "v3/acquisition.hpp"
#include "v3/protocol.hpp"
#include "web/v3_snapshot_json.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace {

std::vector<std::uint8_t> request(std::uint8_t function, std::uint16_t start,
                                  std::uint16_t count) {
    return {0U, 1U, 0U, 0U, 0U, 6U, 1U, function,
            static_cast<std::uint8_t>(start >> 8U), static_cast<std::uint8_t>(start & 255U),
            static_cast<std::uint8_t>(count >> 8U), static_cast<std::uint8_t>(count & 255U)};
}

}  // namespace

int main() {
    try {
        uhf::v3::SnapshotStore v3_snapshots;
        const auto now = std::chrono::steady_clock::now();
        uhf::v3::CurrentValues current{};
        for (std::size_t i = 0U; i < current.size(); ++i) {
            current[i] = uhf::v3::valid_value(static_cast<float>(i + 1U));
        }
        uhf::v3::TemperatureValues temperature{
            uhf::v3::valid_value(20.0F), uhf::v3::valid_value(30.0F),
            uhf::v3::valid_value(40.0F)};
        v3_snapshots.publish_current(current, now);
        v3_snapshots.publish_temperature(temperature, now);
        uhf::v3::ChannelRegisters words{};
        uhf::v3::RegisterValidity received;
        received.set();
        words[0] = 123U;
        words[2] = 456U;
        const uhf::v3::PdChannel pd = uhf::v3::decode_pd_channel(words, received);
        v3_snapshots.publish_pd_channel(0U, pd, now);

        uhf::acquisition::SnapshotStore legacy;
        uhf::modbus::ModbusTcpServer server(
            legacy, uhf::modbus::ModbusTcpOptions{"127.0.0.1", 1502U, 1U, 2U},
            &v3_snapshots);
        const auto holding = server.handle_request(request(0x03U, 1U, 2U));
        check(holding.size() == 13U, "v3 Modbus TCP holding response size");
        check(holding[7] == 0x03U && holding[8] == 4U, "v3 Modbus TCP holding PDU");
        check(holding[9] == 0x3fU && holding[10] == 0x80U,
              "v3 Modbus TCP float high word");
        const auto mirror = server.handle_request(request(0x04U, 10001U, 3U));
        check(mirror.size() == 15U && mirror[7] == 0x04U && mirror[8] == 6U,
              "v3 Modbus TCP PD mirror");
        check(mirror[9] == 0U && mirror[10] == 123U,
              "v3 Modbus TCP PD raw value");
        const auto invalid = server.handle_request(request(0x06U, 1U, 1U));
        check(invalid.size() == 9U && invalid[7] == 0x86U && invalid[8] == 1U,
              "v3 Modbus TCP illegal function");

        const std::string json = uhf::web::render_v3_snapshot_json(v3_snapshots.snapshot());
        check(json.find("\"schema_version\":3") != std::string::npos,
              "v3 Web schema");
        check(json.find("\"name\":\"Ia\"") != std::string::npos,
              "v3 Web current value");
        check(json.find("\"spectrum\"") != std::string::npos,
              "v3 Web spectrum data");
        std::cout << "v3 upstream: formal Modbus TCP mapping and Web JSON passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
