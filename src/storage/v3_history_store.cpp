// SPDX-License-Identifier: GPL-3.0-only
#include "storage/v3_history_store.hpp"

#include <algorithm>
#include <array>
#include <bitset>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace {

constexpr std::array<char, 4U> kMagic{'U', 'H', 'F', '3'};
constexpr std::uint16_t kVersion = 1U;
constexpr mode_t kFileMode = S_IRUSR | S_IWUSR;
constexpr mode_t kDirectoryMode = S_IRWXU;
constexpr std::size_t kPackedRegisterValidity = (uhf::v3::kChannelRegisterCount + 7U) / 8U;
constexpr std::size_t kPackedSpectrumValidity = (uhf::v3::kSpectrumPoints + 7U) / 8U;

void append_u8(std::vector<std::uint8_t>& bytes, std::uint8_t value) { bytes.push_back(value); }

void append_u16(std::vector<std::uint8_t>& bytes, std::uint16_t value) {
    bytes.push_back(static_cast<std::uint8_t>(value & 0xFFU));
    bytes.push_back(static_cast<std::uint8_t>(value >> 8U));
}

void append_u32(std::vector<std::uint8_t>& bytes, std::uint32_t value) {
    for (std::size_t shift = 0U; shift < 32U; shift += 8U) {
        bytes.push_back(static_cast<std::uint8_t>(value >> shift));
    }
}

void append_u64(std::vector<std::uint8_t>& bytes, std::uint64_t value) {
    for (std::size_t shift = 0U; shift < 64U; shift += 8U) {
        bytes.push_back(static_cast<std::uint8_t>(value >> shift));
    }
}

void append_float(std::vector<std::uint8_t>& bytes, float value) {
    std::uint32_t bits = 0U;
    static_assert(sizeof(bits) == sizeof(value));
    std::memcpy(&bits, &value, sizeof(bits));
    append_u32(bytes, bits);
}

template <std::size_t N>
void append_bits(std::vector<std::uint8_t>& bytes, const std::bitset<N>& bits) {
    for (std::size_t offset = 0U; offset < N; offset += 8U) {
        std::uint8_t packed = 0U;
        for (std::size_t bit = 0U; bit < 8U && offset + bit < N; ++bit) {
            if (bits[offset + bit]) {
                packed = static_cast<std::uint8_t>(packed | (1U << bit));
            }
        }
        bytes.push_back(packed);
    }
}

void append_value(std::vector<std::uint8_t>& bytes, const uhf::v3::Value& value) {
    append_float(bytes, value.value);
    append_u8(bytes, static_cast<std::uint8_t>(value.quality));
}

std::uint16_t read_u16(const std::uint8_t* bytes) noexcept {
    return static_cast<std::uint16_t>(bytes[0]) |
        static_cast<std::uint16_t>(bytes[1]) << 8U;
}

std::uint32_t read_u32(const std::uint8_t* bytes) noexcept {
    return static_cast<std::uint32_t>(bytes[0]) |
        static_cast<std::uint32_t>(bytes[1]) << 8U |
        static_cast<std::uint32_t>(bytes[2]) << 16U |
        static_cast<std::uint32_t>(bytes[3]) << 24U;
}

std::uint64_t read_u64(const std::uint8_t* bytes) noexcept {
    std::uint64_t value = 0U;
    for (std::size_t shift = 0U; shift < 64U; shift += 8U) {
        value |= static_cast<std::uint64_t>(bytes[shift / 8U]) << shift;
    }
    return value;
}

float read_float(const std::uint8_t* bytes) noexcept {
    const std::uint32_t bits = read_u32(bytes);
    float value = 0.0F;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

template <std::size_t N>
void read_bits(const std::uint8_t* bytes, std::bitset<N>& bits) noexcept {
    bits.reset();
    for (std::size_t offset = 0U; offset < N; offset += 8U) {
        const std::uint8_t packed = bytes[offset / 8U];
        for (std::size_t bit = 0U; bit < 8U && offset + bit < N; ++bit) {
            bits.set(offset + bit, (packed & (1U << bit)) != 0U);
        }
    }
}

bool write_atomic(const std::filesystem::path& path,
                  const std::vector<std::uint8_t>& bytes) noexcept {
    const std::filesystem::path temporary = path.string() + ".tmp";
    const int fd = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, kFileMode);
    if (fd < 0) {
        return false;
    }
    std::size_t written = 0U;
    while (written < bytes.size()) {
        const ssize_t result = ::write(fd, bytes.data() + written, bytes.size() - written);
        if (result <= 0) {
            ::close(fd);
            ::unlink(temporary.c_str());
            return false;
        }
        written += static_cast<std::size_t>(result);
    }
    const bool synced = ::fchmod(fd, kFileMode) == 0 && ::fsync(fd) == 0 && ::close(fd) == 0;
    if (!synced || ::rename(temporary.c_str(), path.c_str()) != 0) {
        ::unlink(temporary.c_str());
        return false;
    }
    const std::filesystem::path parent = path.parent_path().empty() ? "." : path.parent_path();
    const int parent_fd = ::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (parent_fd < 0) {
        return false;
    }
    const bool parent_synced = ::fsync(parent_fd) == 0;
    ::close(parent_fd);
    return parent_synced;
}

std::optional<std::uint64_t> timestamp_ms(std::chrono::system_clock::time_point timestamp) {
    const auto value = std::chrono::duration_cast<std::chrono::milliseconds>(
        timestamp.time_since_epoch()).count();
    if (value < 0) {
        return std::nullopt;
    }
    return static_cast<std::uint64_t>(value);
}

std::uint32_t crc32(const std::uint8_t* data, std::size_t size) noexcept {
    std::uint32_t crc = 0xFFFFFFFFU;
    for (std::size_t index = 0U; index < size; ++index) {
        crc ^= data[index];
        for (std::size_t bit = 0U; bit < 8U; ++bit) {
            crc = (crc & 1U) != 0U ? (crc >> 1U) ^ 0xEDB88320U : crc >> 1U;
        }
    }
    return crc ^ 0xFFFFFFFFU;
}

std::uint8_t status_flags(const uhf::v3::SourceStatus& status) noexcept {
    return static_cast<std::uint8_t>((status.has_sample ? 1U : 0U) |
        (status.online ? 2U : 0U) | (status.communication_alarm ? 4U : 0U));
}

bool safe_record_path(const std::filesystem::path& directory,
                      const std::filesystem::path& path) noexcept {
    std::error_code error;
    if (path.extension() != ".bin" ||
        !std::filesystem::is_regular_file(path, error) || error ||
        std::filesystem::is_symlink(path, error) || error) {
        return false;
    }
    const auto canonical_directory = std::filesystem::weakly_canonical(directory, error);
    if (error) {
        return false;
    }
    const auto canonical_parent = std::filesystem::weakly_canonical(path.parent_path(), error);
    return !error && canonical_directory == canonical_parent;
}

template <typename T>
void append_values(std::vector<std::uint8_t>& bytes, const T& values) {
    for (const auto& value : values) {
        append_value(bytes, value);
    }
}

bool read_value(const std::uint8_t*& cursor, const std::uint8_t* end, uhf::v3::Value& value) {
    if (static_cast<std::size_t>(end - cursor) < 5U) {
        return false;
    }
    value.value = read_float(cursor);
    cursor += 4U;
    const auto quality = cursor[0];
    cursor += 1U;
    if (quality > static_cast<std::uint8_t>(uhf::v3::Quality::insufficient_samples)) {
        return false;
    }
    value.quality = static_cast<uhf::v3::Quality>(quality);
    return true;
}

}  // namespace

namespace uhf::storage {

V3HistoryStore::V3HistoryStore(std::filesystem::path directory)
    : directory_(std::move(directory)) {
    std::error_code error;
    std::filesystem::create_directories(directory_, error);
    if (error || !std::filesystem::is_directory(directory_, error) || error ||
        ::chmod(directory_.c_str(), kDirectoryMode) != 0) {
        throw std::runtime_error("unable to prepare v3 history directory");
    }
}

std::optional<std::filesystem::path> V3HistoryStore::save(
    const v3::UnifiedSnapshot& snapshot, std::chrono::system_clock::time_point timestamp) {
    const auto milliseconds = timestamp_ms(timestamp);
    if (!milliseconds) {
        return std::nullopt;
    }
    std::vector<std::uint8_t> bytes;
    bytes.reserve(100000U);
    bytes.insert(bytes.end(), kMagic.begin(), kMagic.end());
    append_u16(bytes, kVersion);
    append_u64(bytes, snapshot.generation);
    append_u64(bytes, *milliseconds);
    for (const auto* status : {&snapshot.pd_status, &snapshot.current_status,
                               &snapshot.temperature_status}) {
        append_u8(bytes, status_flags(*status));
        append_u32(bytes, status->consecutive_failures);
    }
    for (const bool valid : snapshot.pd_valid) {
        append_u8(bytes, valid ? 1U : 0U);
    }
    for (const v3::PdChannel& channel : snapshot.pd) {
        for (const std::uint16_t value : channel.raw) append_u16(bytes, value);
        append_bits(bytes, channel.received);
        for (const v3::PdFeature& feature : channel.features) {
            append_u16(bytes, feature.raw);
            append_float(bytes, feature.value);
            append_u8(bytes, feature.valid ? 1U : 0U);
        }
        for (const std::int16_t value : channel.spectrum_raw) {
            append_u16(bytes, static_cast<std::uint16_t>(value));
        }
        append_bits(bytes, channel.spectrum_received);
    }
    append_values(bytes, snapshot.current);
    append_values(bytes, snapshot.temperature);
    append_values(bytes, snapshot.measurements);
    std::uint16_t alarms_valid = 0U;
    std::uint16_t alarms_active = 0U;
    for (std::size_t index = 0U; index < v3::kAlarmCount; ++index) {
        if (snapshot.alarm_valid[index]) alarms_valid = static_cast<std::uint16_t>(alarms_valid | (1U << index));
        if (snapshot.alarm_active[index]) alarms_active = static_cast<std::uint16_t>(alarms_active | (1U << index));
    }
    append_u16(bytes, alarms_valid);
    append_u16(bytes, alarms_active);
    append_u32(bytes, crc32(bytes.data(), bytes.size()));
    const auto path = directory_ / ("v3-" + std::to_string(*milliseconds) + "-" +
                                    std::to_string(snapshot.generation) + ".bin");
    return write_atomic(path, bytes) ? std::optional<std::filesystem::path>{path} : std::nullopt;
}

std::optional<V3HistoryRecord> V3HistoryStore::read(const std::filesystem::path& path) const {
    if (!safe_record_path(directory_, path)) {
        return std::nullopt;
    }
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error || size < 32U || size > 200000U) {
        return std::nullopt;
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) return std::nullopt;
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!input || input.gcount() != static_cast<std::streamsize>(bytes.size()) ||
        !std::equal(kMagic.begin(), kMagic.end(), bytes.begin()) ||
        read_u16(bytes.data() + 4U) != kVersion ||
        read_u32(bytes.data() + bytes.size() - 4U) != crc32(bytes.data(), bytes.size() - 4U)) {
        return std::nullopt;
    }
    const std::uint8_t* cursor = bytes.data() + 6U;
    const std::uint8_t* end = bytes.data() + bytes.size() - 4U;
    V3HistoryRecord record;
    record.generation = read_u64(cursor); cursor += 8U;
    record.timestamp = std::chrono::system_clock::time_point(std::chrono::milliseconds(read_u64(cursor))); cursor += 8U;
    std::array<v3::SourceStatus*, 3U> statuses{
        &record.snapshot.pd_status, &record.snapshot.current_status,
        &record.snapshot.temperature_status};
    for (auto* status : statuses) {
        if (end - cursor < 5) return std::nullopt;
        const std::uint8_t flags = *cursor++;
        status->has_sample = (flags & 1U) != 0U;
        status->online = (flags & 2U) != 0U;
        status->communication_alarm = (flags & 4U) != 0U;
        status->consecutive_failures = read_u32(cursor); cursor += 4U;
    }
    for (bool& valid : record.snapshot.pd_valid) {
        if (cursor >= end) return std::nullopt;
        valid = *cursor++ != 0U;
    }
    for (v3::PdChannel& channel : record.snapshot.pd) {
        if (end - cursor < static_cast<std::ptrdiff_t>(v3::kChannelRegisterCount * 2U + kPackedRegisterValidity)) return std::nullopt;
        for (std::uint16_t& value : channel.raw) { value = read_u16(cursor); cursor += 2U; }
        read_bits(cursor, channel.received); cursor += kPackedRegisterValidity;
        for (v3::PdFeature& feature : channel.features) {
            if (end - cursor < 7) return std::nullopt;
            feature.raw = read_u16(cursor); cursor += 2U;
            feature.value = read_float(cursor); cursor += 4U;
            feature.valid = *cursor++ != 0U;
        }
        if (end - cursor < static_cast<std::ptrdiff_t>(v3::kSpectrumPoints * 2U + kPackedSpectrumValidity)) return std::nullopt;
        for (std::int16_t& value : channel.spectrum_raw) { value = static_cast<std::int16_t>(read_u16(cursor)); cursor += 2U; }
        read_bits(cursor, channel.spectrum_received); cursor += kPackedSpectrumValidity;
    }
    for (v3::Value& value : record.snapshot.current) if (!read_value(cursor, end, value)) return std::nullopt;
    for (v3::Value& value : record.snapshot.temperature) if (!read_value(cursor, end, value)) return std::nullopt;
    for (v3::Value& value : record.snapshot.measurements) if (!read_value(cursor, end, value)) return std::nullopt;
    if (end - cursor < 4) return std::nullopt;
    const std::uint16_t valid = read_u16(cursor); cursor += 2U;
    const std::uint16_t active = read_u16(cursor); cursor += 2U;
    record.snapshot.alarm_valid = std::bitset<v3::kAlarmCount>(valid);
    record.snapshot.alarm_active = std::bitset<v3::kAlarmCount>(active);
    record.snapshot.generation = record.generation;
    return cursor == end ? std::optional<V3HistoryRecord>{std::move(record)} : std::nullopt;
}

std::vector<std::filesystem::path> V3HistoryStore::list(std::size_t limit) const {
    std::vector<std::pair<std::filesystem::path, V3HistoryRecord>> records;
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(directory_, error)) {
        if (error) return {};
        if (entry.path().extension() != ".bin") continue;
        const auto record = read(entry.path());
        if (record) records.emplace_back(entry.path(), *record);
    }
    std::sort(records.begin(), records.end(), [](const auto& first, const auto& second) {
        return first.second.timestamp != second.second.timestamp
            ? first.second.timestamp > second.second.timestamp
            : first.second.generation > second.second.generation;
    });
    std::vector<std::filesystem::path> result;
    for (std::size_t index = 0U; index < std::min(limit, records.size()); ++index) {
        result.push_back(records[index].first);
    }
    return result;
}

std::string V3HistoryStore::to_csv(const V3HistoryRecord& record) {
    const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
        record.timestamp.time_since_epoch()).count();
    std::string output = "generation,timestamp_ms,name,valid,value,quality\n";
    for (std::size_t index = 0U; index < record.snapshot.measurements.size(); ++index) {
        const v3::Value& value = record.snapshot.measurements[index];
        output += std::to_string(record.generation) + "," + std::to_string(milliseconds) + "," +
            std::string(v3::kValueNames[index]) + "," + (value.valid() ? "1" : "0") + "," +
            (value.valid() ? std::to_string(value.value) : "") + "," +
            std::to_string(static_cast<int>(value.quality)) + "\n";
    }
    return output;
}

}  // namespace uhf::storage
