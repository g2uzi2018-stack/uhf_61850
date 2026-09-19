// SPDX-License-Identifier: GPL-3.0-only
#include "test_support.hpp"
#include "v3/packet_trace.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

int main() {
    try {
        bool rejected_zero_limit = false;
        try {
            uhf::v3::PacketTraceBuffer invalid(0U, 1U);
        } catch (const std::invalid_argument&) {
            rejected_zero_limit = true;
        }
        check(rejected_zero_limit, "packet cache rejects an unbounded zero-entry policy");

        uhf::v3::PacketTraceBuffer buffer(3U, 10U);
        const std::array<std::uint8_t, 4U> first{1U, 3U, 0U, 1U};
        const std::array<std::uint8_t, 4U> second{1U, 3U, 2U, 3U};
        const std::array<std::uint8_t, 4U> third{1U, 3U, 4U, 5U};
        const auto timestamp = std::chrono::system_clock::time_point(
            std::chrono::milliseconds(123456));
        buffer.record(
            uhf::v3::PacketSource::pd, uhf::v3::PacketDirection::transmit,
            first.data(), first.size(), timestamp);
        buffer.record(
            uhf::v3::PacketSource::current, uhf::v3::PacketDirection::receive,
            second.data(), second.size(), timestamp);
        const auto held_snapshot = buffer.snapshot();
        buffer.record(
            uhf::v3::PacketSource::temperature, uhf::v3::PacketDirection::receive,
            third.data(), third.size(), timestamp);
        const auto bounded = buffer.snapshot();
        check(held_snapshot.entries.size() == 2U,
              "a Web reader owns a copy and does not retain the producer lock");
        check(bounded.entries.size() == 2U && bounded.retained_bytes == 8U &&
                  bounded.dropped_entries == 1U,
              "byte cap evicts the oldest entry");
        check(bounded.entries.front().sequence == 2U &&
                  bounded.entries.back().bytes == std::vector<std::uint8_t>(
                      third.begin(), third.end()),
              "bounded trace preserves sequence and exact payload");

        const std::array<std::uint8_t, 11U> oversized{};
        buffer.record(
            uhf::v3::PacketSource::pd, uhf::v3::PacketDirection::receive,
            oversized.data(), oversized.size(), timestamp);
        check(buffer.snapshot().dropped_entries == 2U,
              "oversized packet is counted and never retained");

        uhf::v3::PacketTraceBuffer concurrent(64U, 4096U);
        std::vector<std::thread> producers;
        for (std::size_t producer = 0U; producer < 4U; ++producer) {
            producers.emplace_back([producer, &concurrent] {
                for (std::size_t index = 0U; index < 1000U; ++index) {
                    const std::array<std::uint8_t, 2U> bytes{
                        static_cast<std::uint8_t>(producer),
                        static_cast<std::uint8_t>(index & 0xFFU)};
                    concurrent.record(
                        uhf::v3::PacketSource::current,
                        uhf::v3::PacketDirection::receive,
                        bytes.data(), bytes.size());
                }
            });
        }
        for (std::thread& producer : producers) producer.join();
        const auto concurrent_snapshot = concurrent.snapshot();
        check(concurrent_snapshot.entries.size() == 64U &&
                  concurrent_snapshot.retained_bytes == 128U &&
                  concurrent_snapshot.dropped_entries == 3936U,
              "concurrent producers remain within both hard limits");
        for (std::size_t index = 1U; index < concurrent_snapshot.entries.size(); ++index) {
            check(concurrent_snapshot.entries[index - 1U].sequence <
                      concurrent_snapshot.entries[index].sequence,
                  "retained packet sequences are strictly ordered");
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
