// SPDX-License-Identifier: GPL-3.0-only
#include "storage/v3_history_store.hpp"

#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <unistd.h>
#include <vector>

namespace {

std::uint32_t fixture_crc32(const std::vector<std::uint8_t>& bytes) {
    std::uint32_t crc = 0xFFFFFFFFU;
    for (const std::uint8_t value : bytes) {
        crc ^= value;
        for (std::size_t bit = 0U; bit < 8U; ++bit) {
            crc = (crc & 1U) != 0U ? (crc >> 1U) ^ 0xEDB88320U : crc >> 1U;
        }
    }
    return crc ^ 0xFFFFFFFFU;
}

void append_u32(std::vector<std::uint8_t>& bytes, std::uint32_t value) {
    for (std::size_t shift = 0U; shift < 32U; shift += 8U) {
        bytes.push_back(static_cast<std::uint8_t>(value >> shift));
    }
}

}  // namespace

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
    snapshot.current_status.has_sample = true;
    snapshot.current_status.last_attempt_utc = timestamp;
    snapshot.current_status.last_success_utc = timestamp;
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
    assert(record->snapshot.current_status.last_success_utc == timestamp);
    assert(uhf::storage::V3HistoryStore::to_csv(*record).find("Ia") != std::string::npos);

    std::ifstream current_file(*path, std::ios::binary);
    const std::vector<std::uint8_t> current_bytes{
        std::istreambuf_iterator<char>(current_file), std::istreambuf_iterator<char>()};
    assert(current_bytes.size() > 89U);
    std::vector<std::uint8_t> legacy_bytes(current_bytes.begin(), current_bytes.begin() + 22);
    legacy_bytes[4] = 1U;
    legacy_bytes[5] = 0U;
    for (std::size_t status = 0U; status < 3U; ++status) {
        const auto begin = current_bytes.begin() + 22 + static_cast<std::ptrdiff_t>(status * 21U);
        legacy_bytes.insert(legacy_bytes.end(), begin, begin + 5);
    }
    legacy_bytes.insert(
        legacy_bytes.end(), current_bytes.begin() + 85, current_bytes.end() - 4);
    append_u32(legacy_bytes, fixture_crc32(legacy_bytes));
    const auto legacy_path = root / "v3-legacy.bin";
    std::ofstream legacy_file(legacy_path, std::ios::binary);
    legacy_file.write(
        reinterpret_cast<const char*>(legacy_bytes.data()),
        static_cast<std::streamsize>(legacy_bytes.size()));
    legacy_file.close();
    const auto legacy_record = store.read(legacy_path);
    assert(legacy_record && legacy_record->generation == 42U);
    assert(legacy_record->snapshot.current_status.last_success_utc ==
        std::chrono::system_clock::time_point{});

    std::ofstream corrupt(*path, std::ios::binary | std::ios::app);
    corrupt.put('x');
    corrupt.close();
    assert(!store.read(*path));
    assert(!store.read(root.parent_path() / "outside.bin"));

    std::filesystem::remove_all(root, error);
    return 0;
}
