#include "stack_config.h"

extern "C" {
#include "byte_buffer.h"
#include "mms_server_connection.h"
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

std::vector<std::uint8_t> nested_indefinite_pdu(std::size_t depth)
{
    std::vector<std::uint8_t> bytes;
    bytes.reserve(depth * 4U + 4U);
    bytes.push_back(0xa0U);
    bytes.push_back(0x80U);
    for (std::size_t level = 1U; level < depth; ++level) {
        bytes.push_back(0xa0U);
        bytes.push_back(0x80U);
    }
    bytes.push_back(0x04U);
    bytes.push_back(0x00U);
    for (std::size_t level = 0U; level < depth; ++level) {
        bytes.push_back(0x00U);
        bytes.push_back(0x00U);
    }
    return bytes;
}

bool rejects_malformed(
    sMmsServer* server,
    const std::vector<std::uint8_t>& bytes)
{
    struct sMmsServerConnection connection_storage {};
    connection_storage.server = server;

    std::array<std::uint8_t, 32U> response_bytes{};
    ByteBuffer message;
    ByteBuffer response;
    ByteBuffer_wrap(
        &message,
        const_cast<std::uint8_t*>(bytes.data()),
        static_cast<int>(bytes.size()),
        static_cast<int>(bytes.size()));
    ByteBuffer_wrap(
        &response,
        response_bytes.data(),
        0,
        static_cast<int>(response_bytes.size()));

    MmsServerConnection_parseMessage(
        &connection_storage,
        &message,
        &response);

    return response.size == 5 &&
        response.buffer[0] == 0xa4U &&
        response.buffer[1] == 0x03U &&
        response.buffer[2] == 0x85U &&
        response.buffer[3] == 0x01U &&
        response.buffer[4] == 0x01U;
}

} // namespace

int main()
{
    struct sMmsServer server_storage {};

    const bool truncated_length = rejects_malformed(
        &server_storage,
        {0xa0U, 0x82U, 0x00U});
    const bool declared_payload_overrun = rejects_malformed(
        &server_storage,
        {0xa0U, 0x02U, 0x00U});
    const std::vector<std::uint8_t> oversized(
        static_cast<std::size_t>(CONFIG_MMS_MAXIMUM_PDU_SIZE) + 1U,
        0U);
    const bool oversized_pdu = rejects_malformed(&server_storage, oversized);
    const bool deep_pdu = rejects_malformed(
        &server_storage,
        nested_indefinite_pdu(
            static_cast<std::size_t>(
                CONFIG_MMS_MAX_DATA_STRUCTURE_NESTING_LEVEL) +
            1U));
    bool ok = expect(truncated_length, "truncated BER length is rejected");
    ok = expect(declared_payload_overrun, "declared payload overrun is rejected") && ok;
    ok = expect(oversized_pdu, "PDU above the configured size is rejected") && ok;
    ok = expect(deep_pdu, "PDU above the BER depth limit is rejected") && ok;
    ok = expect(
        MmsServer_getMalformedPduRejectCount(&server_storage) == 2U,
        "malformed PDU counter records both malformed rejects") && ok;
    ok = expect(
        MmsServer_getOversizedPduRejectCount(&server_storage) == 1U,
        "oversized PDU counter records the size reject") && ok;
    ok = expect(
        MmsServer_getBerDepthRejectCount(&server_storage) == 1U,
        "BER depth counter records the depth reject") && ok;

    if (ok)
        std::cout << "IEC 61850 malformed PDU smoke: OK\n";
    return ok ? 0 : 1;
}
