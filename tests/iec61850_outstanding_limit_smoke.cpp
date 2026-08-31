#include "stack_config.h"

extern "C" {
#include "mms_server_internal.h"
}

#include <iostream>

namespace {

bool expect(bool condition, const char* message)
{
    if (!condition)
        std::cerr << "FAIL: " << message << '\n';
    return condition;
}

} // namespace

int main()
{
    struct sMmsServerConnection connection_storage {};
    MmsServerConnection connection = &connection_storage;
    connection->maxServOutstandingCalled =
        CONFIG_DEFAULT_MAX_SERV_OUTSTANDING_CALLED;

    bool ok = true;
    ok = expect(
        MmsServerConnection_reserveOutstandingCall(connection),
        "first outstanding call is admitted") && ok;
    ok = expect(
        MmsServerConnection_reserveOutstandingCall(connection),
        "second outstanding call is admitted") && ok;
    ok = expect(
        !MmsServerConnection_reserveOutstandingCall(connection),
        "third outstanding call is rejected") && ok;
    ok = expect(
        connection->outstandingCalls == CONFIG_DEFAULT_MAX_SERV_OUTSTANDING_CALLED,
        "outstanding count stops at configured limit") && ok;

    MmsServerConnection_releaseOutstandingCall(connection);
    ok = expect(
        MmsServerConnection_reserveOutstandingCall(connection),
        "released outstanding slot is reusable") && ok;
    MmsServerConnection_releaseOutstandingCall(connection);
    MmsServerConnection_releaseOutstandingCall(connection);
    ok = expect(
        connection->outstandingCalls == 0,
        "all outstanding calls can be released") && ok;

    connection->maxServOutstandingCalled = 0;
    ok = expect(
        !MmsServerConnection_reserveOutstandingCall(connection),
        "zero negotiated limit rejects a request") && ok;

    if (ok)
        std::cout << "IEC 61850 outstanding limit smoke: OK\n";
    return ok ? 0 : 1;
}
