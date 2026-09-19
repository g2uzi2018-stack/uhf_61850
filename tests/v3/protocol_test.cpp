// SPDX-License-Identifier: GPL-3.0-only
#include "v3/protocol.hpp"
#include "test_support.hpp"
#include <limits>
using namespace uhf::v3;
namespace {
std::vector<std::uint8_t> with_crc(std::vector<std::uint8_t> frame) {
    const auto crc = uhf::domain::modbus_crc16(frame.data(), frame.size());
    frame.push_back(static_cast<std::uint8_t>(crc & 255U));
    frame.push_back(static_cast<std::uint8_t>(crc >> 8U));
    return frame;
}
}
int main() {
    try {
        for (std::size_t c = 1; c <= 3; ++c) {
            const auto plan = pd_request_plan(c);
            std::size_t address = kChannelStarts[c - 1];
            std::size_t total = 0;
            for (const auto& request : plan) {
                check(request.start_address == address, "plan gap or address shifted");
                check(request.function == 4 && request.register_count <= 125, "PD request format");
                const auto crc = uhf::domain::modbus_crc16(request.wire_frame.data(), 6);
                check(request.wire_frame[6] == (crc & 255U) && request.wire_frame[7] == (crc >> 8U), "CRC");
                total += request.register_count;
                address += request.register_count;
            }
            check(total == 3615, "full channel coverage");
        }
        check(current_request().wire_frame == std::array<std::uint8_t, 8>{1,3,0x20,1,0,8,0x1e,0x0c}, "current golden request");
        check(temperature_request().wire_frame == std::array<std::uint8_t, 8>{1,3,0,1,0,6,0x94,8}, "temperature golden request");
        rejects([] { pd_request_plan(0); }, "channel zero");
        rejects([] { pd_request_plan(4); }, "channel four");
        rejects([] { make_read_request(0, 4, 0, 1); }, "broadcast read");
        rejects([] { make_read_request(255, 4, 0, 1); }, "invalid slave");
        rejects([] { make_read_request(1, 6, 0, 1); }, "write function");
        rejects([] { make_read_request(1, 4, 0, 126); }, "125-register maximum");
        rejects([] { make_read_request(1, 4, 65535, 2); }, "address overflow");
        check(make_read_request(254, 4, 65535, 1).slave_id == 254, "vendor address range");
        ChannelRegisters words{};
        RegisterValidity valid; valid.set();
        words[0] = 123; words[1] = 65535; words[2] = 4096; words[3] = 360;
        words[4] = 4096; words[5] = 1234; words[6] = 5678;
        words[15] = 0xffba; words[16] = 0xffbc; words.back() = 0x8000;
        auto pd = decode_pd_channel(words, valid);
        near(pd.features[0].value, 123, "mV, not dBm");
        near(pd.features[5].value, 12.34F, "50 Hz percent scaling");
        near(pd.features[6].value, 56.78F, "100 Hz percent scaling");
        check(pd.spectrum_raw[0] == -70 && pd.spectrum_raw[1] == -68, "signed spectrum words");
        check(pd.spectrum_received.all(), "old sentinel meanings must not leak into v3");
        check(pd.spectrum_raw.back() == -32768, "last of 3600 points");
        words[0] = 0xffff; words[2] = 4097; words[3] = 361;
        valid.reset(4); valid.reset(15);
        pd = decode_pd_channel(words, valid);
        check(!pd.features[0].valid && !pd.features[2].valid && !pd.features[3].valid && !pd.features[4].valid, "range and receive quality");
        check(!pd.spectrum_received[0] && pd.spectrum_received[1], "per-field receive quality");
        const std::array<std::uint16_t, 6> t{0xfff6, 99, 140, 88, 151, 77};
        check(temperature_words(t) == std::array<std::int16_t,3>{-10,140,151}, "temperature odd-register selection");
        near(temperature_values(t, {0.1F,0})[0], -1, "explicit temperature scale");
        std::array<std::uint16_t,8> currents{}; currents[0] = 0xffff;
        near(current_values(currents, WordEncoding::unsigned16, {1,0})[0],65535,"unsigned current");
        near(current_values(currents, WordEncoding::signed16, {1,0})[0],-1,"signed current");
        rejects([&] { temperature_values(t, {0,0}); }, "zero scale");
        rejects([&] { temperature_values(t, {1,std::numeric_limits<float>::infinity()}); }, "nonfinite scale");
        const auto request = make_read_request(1,3,0x2001,2);
        auto frame = with_crc({1,3,4,0x12,0x34,0xab,0xcd});
        auto reply = decode_read_reply(request,frame.data(),frame.size());
        check(reply.ok() && reply.registers == std::vector<std::uint16_t>{0x1234,0xabcd},"reply golden");
        frame[4] ^= 1;
        check(decode_read_reply(request,frame.data(),frame.size()).error == ReplyError::crc,"bad CRC");
        frame = with_crc({2,3,4,0,1,0,2});
        check(decode_read_reply(request,frame.data(),frame.size()).error == ReplyError::slave,"wrong slave");
        frame = with_crc({1,4,4,0,1,0,2});
        check(decode_read_reply(request,frame.data(),frame.size()).error == ReplyError::function,"wrong function");
        frame = with_crc({1,3,2,0,1});
        check(decode_read_reply(request,frame.data(),frame.size()).error == ReplyError::byte_count,"short data");
        frame = with_crc({1,3,4,0,1,0,2,0});
        check(!decode_read_reply(request,frame.data(),frame.size()).ok(),"extra data");
        frame = with_crc({1,0x83,2});
        reply = decode_read_reply(request,frame.data(),frame.size());
        check(reply.error == ReplyError::exception && reply.exception_code == 2 && reply.registers.empty(),"Modbus exception");
        check(!decode_read_reply(request,nullptr,20).ok(),"null frame");
        std::cout << "v3 protocol: all checks passed\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
