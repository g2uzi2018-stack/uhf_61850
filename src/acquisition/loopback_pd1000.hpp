// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "acquisition/acquisition.hpp"

#include <array>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>

namespace uhf::acquisition {

class LoopbackPd1000Port final : public ISerialPort {
public:
    LoopbackPd1000Port();

    bool write_all(const std::uint8_t* data, std::size_t size) override;
    bool read_some(
        std::uint8_t* data,
        std::size_t capacity,
        std::chrono::milliseconds timeout,
        std::size_t& received) override;

private:
    std::array<std::uint16_t, domain::kPd1000RegisterCount> registers_{};
    std::deque<std::uint8_t> pending_response_;
    std::size_t next_request_index_{0U};
    std::mutex mutex_;
    std::condition_variable response_ready_;
};

}  // namespace uhf::acquisition
