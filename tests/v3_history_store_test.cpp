// SPDX-License-Identifier: GPL-3.0-only
#include "storage/v3_history_store.hpp"

#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>

int main() {
    const auto root = std::filesystem::temp_directory_path() /
        ("uhf-v3-history-" + std::to_string(static_cast<long long>(::getpid())));
    std::error_code error;
    std::filesystem::remove_all(root, error);
    uhf::storage::V3HistoryStore store(root);

    uhf::v3::UnifiedSnapshot snapshot;
    snapshot.generation = 42U;
    snapshot.pd_valid[0] = true;
    snapshot.pd[0].features[2] = uhf::v3::PdFeature{77U, 77.0F, true};
    snapshot.measurements[0] = uhf::v3::valid_value(12.5F);
    snapshot.alarm_valid.set(0U);
    snapshot.alarm_active.set(0U);
    const auto timestamp = std::chrono::system_clock::time_point(std::chrono::milliseconds(123456));
    const auto path = store.save(snapshot, timestamp);
    assert(path);
    const auto paths = store.list(10U);
    assert(paths.size() == 1U);
    const auto record = store.read(paths.front());
    assert(record);
    assert(record->generation == 42U);
    assert(record->timestamp == timestamp);
    assert(record->snapshot.measurements[0].valid());
    assert(record->snapshot.measurements[0].value == 12.5F);
    assert(record->snapshot.pd[0].features[2].raw == 77U);
    assert(record->snapshot.alarm_active[0]);
    assert(uhf::storage::V3HistoryStore::to_csv(*record).find("Ia") != std::string::npos);

    std::ofstream corrupt(*path, std::ios::binary | std::ios::app);
    corrupt.put('x');
    corrupt.close();
    assert(!store.read(*path));
    assert(!store.read(root.parent_path() / "outside.bin"));

    std::filesystem::remove_all(root, error);
    return 0;
}
