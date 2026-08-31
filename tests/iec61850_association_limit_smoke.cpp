#include "stack_config.h"

extern "C" {
#include "ber_decode.h"
#include "byte_buffer.h"
#include "mms_server_internal.h"
}

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <vector>

namespace {

bool expect(bool condition, const char* message)
{
    if (!condition)
        std::cerr << "FAIL: " << message << '\n';
    return condition;
}

bool decode_nesting_level(const ByteBuffer& response, std::uint32_t* nesting_level)
{
    int outer_length = 0;
    int position = BerDecoder_decodeLength(
        response.buffer,
        &outer_length,
        1,
        response.size);
    if (position < 0 || response.buffer[0] != 0xa9U ||
        position + outer_length > response.size) {
        return false;
    }

    const int end_position = position + outer_length;
    while (position < end_position) {
        const std::uint8_t tag = response.buffer[position++];
        int length = 0;
        position = BerDecoder_decodeLength(
            response.buffer,
            &length,
            position,
            end_position);
        if (position < 0 || position + length > end_position)
            return false;
        if (tag == 0x83U) {
            if (length <= 0)
                return false;
            *nesting_level = BerDecoder_decodeUint32(
                response.buffer,
                length,
                position);
            return true;
        }
        position += length;
    }
    return false;
}

bool check_proposed_level(
    const std::vector<std::uint8_t>& encoded_level,
    std::uint32_t expected_level)
{
    if (encoded_level.empty() || encoded_level.size() > 255U)
        return false;

    std::vector<std::uint8_t> request = {
        0x80U, 0x02U, 0x01U, 0x00U,
        0x81U, 0x01U, 0x01U,
        0x82U, 0x01U, 0x01U,
    };
    request.reserve(request.size() + 2U + encoded_level.size());
    request.push_back(0x83U);
    request.push_back(static_cast<std::uint8_t>(encoded_level.size()));
    request.insert(request.end(), encoded_level.begin(), encoded_level.end());

    std::array<std::uint8_t, 512> response_bytes{};
    ByteBuffer response;
    ByteBuffer_wrap(
        &response,
        response_bytes.data(),
        0,
        static_cast<int>(response_bytes.size()));

    struct sMmsServer server_storage {};
    server_storage.fileServiceEnabled = false;
    server_storage.dynamicVariableListServiceEnabled = false;
    server_storage.journalServiceEnabled = false;

    struct sMmsServerConnection connection_storage {};
    connection_storage.server = &server_storage;
    MmsServerConnection connection = &connection_storage;
    mmsServer_handleInitiateRequest(
        connection,
        request.data(),
        0,
        static_cast<int>(request.size()),
        &response);

    std::uint32_t actual_level = 0U;
    return response.size > 0 &&
        decode_nesting_level(response, &actual_level) &&
        actual_level == expected_level;
}

} // namespace

int main()
{
    bool ok = true;
    ok = expect(
        check_proposed_level(
            std::vector<std::uint8_t>{99U},
            static_cast<std::uint32_t>(
                CONFIG_MMS_MAX_DATA_STRUCTURE_NESTING_LEVEL)),
        "large proposed nesting level is capped") && ok;
    ok = expect(
        check_proposed_level(std::vector<std::uint8_t>{16U}, 16U),
        "smaller proposed nesting level is preserved") && ok;
    ok = expect(
        check_proposed_level(
            std::vector<std::uint8_t>{0xffU, 0xffU, 0xffU, 0xffU},
            static_cast<std::uint32_t>(
                CONFIG_MMS_MAX_DATA_STRUCTURE_NESTING_LEVEL)),
        "unsigned overflow proposal is capped") && ok;

    if (ok)
        std::cout << "IEC 61850 association limit smoke: OK\n";
    return ok ? 0 : 1;
}
