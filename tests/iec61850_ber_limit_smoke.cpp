#include "stack_config.h"

extern "C" {
#include "ber_decode.h"
}

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <vector>

namespace {

std::vector<std::uint8_t> nested_indefinite(std::size_t depth)
{
    std::vector<std::uint8_t> bytes;
    bytes.reserve(depth * 2U + 4U);
    bytes.push_back(static_cast<std::uint8_t>(0x80U));

    for (std::size_t level = 1U; level < depth; ++level) {
        bytes.push_back(static_cast<std::uint8_t>(0xa0U));
        bytes.push_back(static_cast<std::uint8_t>(0x80U));
    }

    bytes.push_back(static_cast<std::uint8_t>(0x04U));
    bytes.push_back(static_cast<std::uint8_t>(0x00U));
    for (std::size_t level = 0U; level < depth; ++level) {
        bytes.push_back(static_cast<std::uint8_t>(0x00U));
        bytes.push_back(static_cast<std::uint8_t>(0x00U));
    }
    return bytes;
}

bool expect(bool condition, const char* message)
{
    if (!condition)
        std::cerr << "FAIL: " << message << '\n';
    return condition;
}

} // namespace

int main()
{
    constexpr std::size_t kConfiguredLimit = static_cast<std::size_t>(
        CONFIG_MMS_MAX_DATA_STRUCTURE_NESTING_LEVEL);
    if (kConfiguredLimit == 0U) {
        std::cerr << "FAIL: configured nesting limit must be positive\n";
        return 1;
    }

    auto within_limit = nested_indefinite(kConfiguredLimit);
    int length = -1;
    int position = BerDecoder_decodeLength(
        within_limit.data(),
        &length,
        0,
        static_cast<int>(within_limit.size()));
    bool ok = expect(
        position >= 0 && length > 0,
        "BER at the configured nesting limit is accepted");

    auto above_limit = nested_indefinite(kConfiguredLimit + 1U);
    length = -1;
    position = BerDecoder_decodeLength(
        above_limit.data(),
        &length,
        0,
        static_cast<int>(above_limit.size()));
    ok = expect(
        position < 0,
        "BER above the configured nesting limit is rejected") && ok;

    if (ok)
        std::cout << "IEC 61850 BER limit smoke: OK\n";
    return ok ? 0 : 1;
}
