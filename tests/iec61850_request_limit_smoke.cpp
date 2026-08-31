#include "acquisition/acquisition.hpp"
#include "domain/snapshot.hpp"
#include "iec61850/server.hpp"
#include "linked_list.h"
#include "mms_client_connection.h"
#include "mms_value.h"

#include <cstddef>
#include <iostream>

namespace {

bool expect(bool condition, const char* message)
{
    if (!condition)
        std::cerr << "FAIL: " << message << '\n';
    return condition;
}

LinkedList repeated_items(std::size_t count, char* item)
{
    LinkedList items = LinkedList_create();
    if (items == nullptr)
        return nullptr;
    for (std::size_t index = 0U; index < count; ++index)
        LinkedList_add(items, item);
    return items;
}

LinkedList repeated_values(std::size_t count, MmsValue* value)
{
    LinkedList values = LinkedList_create();
    if (values == nullptr)
        return nullptr;
    for (std::size_t index = 0U; index < count; ++index)
        LinkedList_add(values, value);
    return values;
}

} // namespace

int main()
{
    constexpr std::size_t kLimit = 512U;
    uhf::acquisition::SnapshotStore snapshots;
    uhf::iec61850::ServerOptions options;
    options.bind_address = "127.0.0.1";
    options.port = 15104U;
    options.ied_name = "LIMITIED";
    uhf::iec61850::Server server(snapshots, options);
    server.start();

    bool ok = expect(server.running(), "MMS server starts");
    MmsConnection connection = MmsConnection_create();
    MmsError error = MMS_ERROR_NONE;
    if (connection == nullptr) {
        server.stop();
        return 1;
    }

    if (ok) {
        MmsConnection_setRequestTimeout(connection, 1000U);
        ok = expect(
            MmsConnection_connect(connection, &error, "127.0.0.1", 15104),
            "MMS client connects") && ok;
    }

    char missing_item[] = "missing";
    if (ok) {
        LinkedList accepted_items = repeated_items(kLimit, missing_item);
        ok = expect(accepted_items != nullptr, "create request at limit") && ok;
        if (accepted_items != nullptr) {
            error = MMS_ERROR_NONE;
            MmsValue* values = MmsConnection_readMultipleVariables(
                connection, &error, "missing", accepted_items);
            ok = expect(
                error == MMS_ERROR_NONE && values != nullptr &&
                    MmsValue_getArraySize(values) == static_cast<int>(kLimit),
                "512-element read request is handled") && ok;
            if (values != nullptr)
                MmsValue_delete(values);
            LinkedList_destroyStatic(accepted_items);
        }

        LinkedList rejected_items = repeated_items(kLimit + 1U, missing_item);
        ok = expect(rejected_items != nullptr, "create request over limit") && ok;
        if (rejected_items != nullptr) {
            error = MMS_ERROR_NONE;
            MmsValue* values = MmsConnection_readMultipleVariables(
                connection, &error, "missing", rejected_items);
            ok = expect(
                error != MMS_ERROR_NONE && values == nullptr,
                "513-element read request is rejected") && ok;
            if (values != nullptr)
                MmsValue_delete(values);
            LinkedList_destroyStatic(rejected_items);
        }

        MmsValue* write_value = MmsValue_newIntegerFromInt32(0);
        LinkedList write_items = repeated_items(kLimit + 1U, missing_item);
        LinkedList write_values = repeated_values(kLimit + 1U, write_value);
        LinkedList write_results = nullptr;
        ok = expect(
            write_value != nullptr && write_items != nullptr && write_values != nullptr,
            "create write request over limit") && ok;
        if (write_value != nullptr && write_items != nullptr && write_values != nullptr) {
            error = MMS_ERROR_NONE;
            MmsConnection_writeMultipleVariables(
                connection,
                &error,
                "missing",
                write_items,
                write_values,
                &write_results);
            ok = expect(
                error != MMS_ERROR_NONE && write_results == nullptr,
                "513-element write request is rejected") && ok;
        }
        if (write_results != nullptr)
            LinkedList_destroyDeep(
                write_results,
                reinterpret_cast<LinkedListValueDeleteFunction>(MmsValue_delete));
        if (write_items != nullptr)
            LinkedList_destroyStatic(write_items);
        if (write_values != nullptr)
            LinkedList_destroyStatic(write_values);
        if (write_value != nullptr)
            MmsValue_delete(write_value);
    }

    MmsConnection_close(connection);
    MmsConnection_destroy(connection);
    server.stop();
    ok = expect(!server.running(), "MMS server stops") && ok;
    if (ok)
        std::cout << "IEC 61850 request limit smoke: OK\n";
    return ok ? 0 : 1;
}
