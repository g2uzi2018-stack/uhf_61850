#include "lib_memory.h"

#include <cstddef>
#include <cstdint>
#include <iostream>

namespace {

int fail(const char* message)
{
    std::cerr << "IEC 61850 memory smoke failed: " << message << '\n';
    return 1;
}

} // namespace

int main()
{
    constexpr std::size_t kMemoryLimit = 8U * 1024U * 1024U;
    constexpr std::size_t kInitialSize = 1024U;
    constexpr std::size_t kGrownSize = 2048U;
    constexpr std::size_t kShrunkSize = 512U;
    constexpr std::size_t kZeroCount = 64U;

    void* oversized = Memory_malloc(kMemoryLimit + 1U);
    if (oversized != nullptr) {
        Memory_free(oversized);
        return fail("allocation above the configured budget succeeded");
    }

    auto* bytes = static_cast<unsigned char*>(Memory_malloc(kInitialSize));
    if (bytes == nullptr)
        return fail("small allocation failed");
    if (reinterpret_cast<std::uintptr_t>(bytes) % alignof(std::max_align_t) != 0U) {
        Memory_free(bytes);
        return fail("allocation is not maximally aligned");
    }

    bytes[0] = 0x5AU;
    bytes[kInitialSize - 1U] = 0xA5U;

    auto* grown = static_cast<unsigned char*>(Memory_realloc(bytes, kGrownSize));
    if (grown == nullptr)
        return fail("growing an allocation failed");
    if (grown[0] != 0x5AU || grown[kInitialSize - 1U] != 0xA5U) {
        Memory_free(grown);
        return fail("realloc did not preserve existing bytes");
    }

    auto* shrunk = static_cast<unsigned char*>(Memory_realloc(grown, kShrunkSize));
    if (shrunk == nullptr)
        return fail("shrinking an allocation failed");
    if (shrunk[0] != 0x5AU) {
        Memory_free(shrunk);
        return fail("shrinking an allocation did not preserve bytes");
    }
    Memory_free(shrunk);

    auto* zeroed = static_cast<unsigned char*>(
        Memory_calloc(kZeroCount, sizeof(unsigned char)));
    if (zeroed == nullptr)
        return fail("calloc failed");
    for (std::size_t index = 0U; index < kZeroCount; ++index) {
        if (zeroed[index] != 0U) {
            Memory_free(zeroed);
            return fail("calloc did not clear memory");
        }
    }
    Memory_free(zeroed);

    std::cout << "IEC 61850 memory smoke: OK\n";
    return 0;
}
