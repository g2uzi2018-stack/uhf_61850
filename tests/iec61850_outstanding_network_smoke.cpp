#include "iec61850/model.hpp"

extern "C" {
#include "mms_client_connection.h"
#include "mms_server_libinternal.h"
}

#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <thread>

namespace {

constexpr int kPort = 15106;
constexpr unsigned int kOutstandingLimit = 2U;

struct AccessState {
    std::atomic<unsigned int> calls{0U};
};

struct ResponseState {
    std::atomic<unsigned int> callbacks{0U};
    std::atomic<unsigned int> timeouts{0U};
    std::atomic<unsigned int> rejects{0U};
    std::atomic<unsigned int> unexpectedErrors{0U};
};

bool expect(bool condition, const char* message)
{
    if (!condition)
        std::cerr << "FAIL: " << message << '\n';
    return condition;
}

MmsDataAccessError hold_read_requests(
    void* parameter,
    MmsDomain* domain,
    char* variableId,
    MmsServerConnection connection,
    bool isDirectAccess)
{
    (void)domain;
    (void)variableId;
    (void)connection;
    (void)isDirectAccess;
    auto* state = static_cast<AccessState*>(parameter);
    state->calls.fetch_add(1U);
    return DATA_ACCESS_ERROR_NO_RESPONSE;
}

void read_callback(
    uint32_t invokeId,
    void* parameter,
    MmsError error,
    MmsValue* value)
{
    (void)invokeId;
    auto* state = static_cast<ResponseState*>(parameter);
    if (value != nullptr)
        MmsValue_delete(value);

    if (error == MMS_ERROR_SERVICE_TIMEOUT)
        state->timeouts.fetch_add(1U);
    else if (error == MMS_ERROR_REJECT_MAX_SERV_OUTSTANDING_EXCEEDED)
        state->rejects.fetch_add(1U);
    else
        state->unexpectedErrors.fetch_add(1U);
    state->callbacks.fetch_add(1U);
}

} // namespace

int main()
{
    uhf::iec61850::Model model("LIMITIED");
    IedServerConfig config = IedServerConfig_create();
    if (config == nullptr)
        return 1;

    IedServerConfig_setMaxMmsConnections(config, 1);
    IedServerConfig_enableFileService(config, false);
    IedServerConfig_enableDynamicDataSetService(config, false);
    IedServerConfig_enableLogService(config, false);
    IedServerConfig_setMaxAssociationSpecificDataSets(config, 0);
    IedServerConfig_setMaxDomainSpecificDataSets(config, 0);
    IedServerConfig_setMaxDataSetEntries(config, 0);
    IedServerConfig_enableEditSG(config, false);
    IedServerConfig_enableResvTmsForBRCB(config, false);
    IedServerConfig_enableOwnerForRCB(config, false);
    IedServerConfig_useIntegratedGoosePublisher(config, false);

    IedServer server = IedServer_createWithConfig(model.raw(), nullptr, config);
    IedServerConfig_destroy(config);
    if (server == nullptr)
        return 1;

    IedServer_setLocalIpAddress(server, "127.0.0.1");
    AccessState access_state;
    MmsServer_installReadAccessHandler(
        IedServer_getMmsServer(server),
        hold_read_requests,
        &access_state);
    IedServer_start(server, kPort);

    bool ok = expect(IedServer_isRunning(server), "MMS server starts");
    MmsConnection connection = MmsConnection_create();
    if (connection == nullptr) {
        IedServer_stop(server);
        IedServer_destroy(server);
        return 1;
    }

    MmsError error = MMS_ERROR_NONE;
    MmsConnection_setConnectTimeout(connection, 1000U);
    MmsConnection_setRequestTimeout(connection, 250U);
    if (ok) {
        ok = expect(
            MmsConnection_connect(connection, &error, "127.0.0.1", kPort),
            "MMS client connects") && ok;
    }

    if (ok) {
        /*
         * The negotiated server limit is two. Enlarge only the client-side
         * callback table so the third request reaches the server and can
         * exercise its protocol-level reject.
         */
        MmsConnnection_setMaxOutstandingCalls(
            connection,
            static_cast<int>(kOutstandingLimit + 1U),
            static_cast<int>(kOutstandingLimit + 1U));

        ResponseState response_state;
        constexpr const char* kDomain = "LIMITIEDPDMON";
        constexpr const char* kItem = "GGIO1$MX$AnIn1$mag$f";
        for (unsigned int index = 0U; index < kOutstandingLimit + 1U; ++index) {
            error = MMS_ERROR_NONE;
            MmsConnection_readVariableAsync(
                connection,
                nullptr,
                &error,
                kDomain,
                kItem,
                read_callback,
                &response_state);
            ok = expect(error == MMS_ERROR_NONE, "request is queued by client") && ok;
        }

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (response_state.callbacks.load() < kOutstandingLimit + 1U &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }

        ok = expect(
            response_state.callbacks.load() == kOutstandingLimit + 1U,
            "all outstanding request callbacks complete") && ok;
        ok = expect(
            response_state.timeouts.load() == kOutstandingLimit,
            "first two requests remain outstanding until timeout") && ok;
        ok = expect(
            response_state.rejects.load() == 1U,
            "third request receives max-outstanding reject") && ok;
        ok = expect(
            response_state.unexpectedErrors.load() == 0U,
            "no unexpected request errors") && ok;
        ok = expect(
            access_state.calls.load() == kOutstandingLimit,
            "rejected request does not reach read handler") && ok;
        ok = expect(
            MmsServer_getMaxOutstandingRejectCount(IedServer_getMmsServer(server)) == 1U,
            "max-outstanding rejection is counted") && ok;
    }

    MmsConnection_close(connection);
    MmsConnection_destroy(connection);
    IedServer_stop(server);
    IedServer_destroy(server);

    if (ok)
        std::cout << "IEC 61850 outstanding network smoke: OK\n";
    return ok ? 0 : 1;
}
