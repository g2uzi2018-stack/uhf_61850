// SPDX-License-Identifier: GPL-3.0-only
#include "storage/frame_store.hpp"

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <fcntl.h>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <iterator>
#include <utility>
#include <vector>

namespace {

constexpr char kFrameMagic[] = {'U', 'H', 'F', 'F'};
constexpr std::size_t kHeaderBytes = 4U + 2U + 8U + 8U + 1U + 1U + 2U;
constexpr std::size_t kFrameBytes =
    kHeaderBytes + uhf::domain::kPd1000RegisterCount * 2U + 4U;
constexpr mode_t kFrameFileMode = S_IRUSR | S_IWUSR;
constexpr mode_t kFrameDirectoryMode = S_IRWXU;

void append_u16(std::vector<std::uint8_t>& bytes, std::uint16_t value) {
    bytes.push_back(static_cast<std::uint8_t>(value & 0x00FFU));
    bytes.push_back(static_cast<std::uint8_t>(value >> 8U));
}

void append_u64(std::vector<std::uint8_t>& bytes, std::uint64_t value) {
    for (std::size_t index = 0; index < 8U; ++index) {
        bytes.push_back(static_cast<std::uint8_t>(value >> (index * 8U)));
    }
}

std::uint16_t read_u16(const std::uint8_t* bytes) noexcept {
    return static_cast<std::uint16_t>(
        static_cast<std::uint16_t>(bytes[0]) |
        static_cast<std::uint16_t>(bytes[1]) << 8U);
}

std::uint64_t read_u64(const std::uint8_t* bytes) noexcept {
    std::uint64_t value = 0;
    for (std::size_t index = 0; index < 8U; ++index) {
        value |= static_cast<std::uint64_t>(bytes[index]) << (index * 8U);
    }
    return value;
}

std::uint32_t crc32(const std::uint8_t* data, std::size_t size) noexcept {
    std::uint32_t crc = 0xFFFFFFFFU;
    for (std::size_t index = 0; index < size; ++index) {
        crc ^= data[index];
        for (std::size_t bit = 0; bit < 8U; ++bit) {
            crc = (crc & 1U) != 0U ? (crc >> 1U) ^ 0xEDB88320U : crc >> 1U;
        }
    }
    return crc ^ 0xFFFFFFFFU;
}

std::uint8_t status_code(uhf::domain::PayloadStatus status) noexcept {
    switch (status) {
    case uhf::domain::PayloadStatus::good:
        return 0U;
    case uhf::domain::PayloadStatus::degraded:
        return 1U;
    case uhf::domain::PayloadStatus::not_refreshed:
        return 2U;
    }
    return 1U;
}

std::optional<uhf::domain::PayloadStatus> decode_status(std::uint8_t code) noexcept {
    switch (code) {
    case 0U:
        return uhf::domain::PayloadStatus::good;
    case 1U:
        return uhf::domain::PayloadStatus::degraded;
    case 2U:
        return uhf::domain::PayloadStatus::not_refreshed;
    default:
        return std::nullopt;
    }
}

std::optional<std::uint64_t> timestamp_milliseconds(
    std::chrono::system_clock::time_point timestamp) noexcept {
    const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
        timestamp.time_since_epoch());
    if (milliseconds.count() < 0) {
        return std::nullopt;
    }
    return static_cast<std::uint64_t>(milliseconds.count());
}

bool write_atomic(
    const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes) noexcept {
    const std::filesystem::path temporary = path.string() + ".tmp";
    const int file_descriptor = ::open(
        temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, kFrameFileMode);
    if (file_descriptor < 0) {
        return false;
    }
    bool open = true;
    bool renamed = false;
    bool complete = false;
    std::size_t written = 0;
    while (written < bytes.size()) {
        const ssize_t result = ::write(
            file_descriptor, bytes.data() + written, bytes.size() - written);
        if (result <= 0) {
            break;
        }
        written += static_cast<std::size_t>(result);
    }
    if (written == bytes.size() && ::fchmod(file_descriptor, kFrameFileMode) == 0 &&
        ::fsync(file_descriptor) == 0) {
        const int close_result = ::close(file_descriptor);
        open = false;
        if (close_result == 0 && ::rename(temporary.c_str(), path.c_str()) == 0) {
            renamed = true;
            const std::filesystem::path parent = path.parent_path().empty() ? "." : path.parent_path();
            const int directory_descriptor =
                ::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
            if (directory_descriptor >= 0) {
                const int sync_result = ::fsync(directory_descriptor);
                ::close(directory_descriptor);
                complete = sync_result == 0;
            }
        }
    }
    if (open) {
        ::close(file_descriptor);
    }
    if (!renamed) {
        ::unlink(temporary.c_str());
    }
    return complete;
}

std::optional<std::vector<std::uint8_t>> read_bytes(
    const std::filesystem::path& path) {
    std::error_code error;
    if (std::filesystem::file_size(path, error) != kFrameBytes || error) {
        return std::nullopt;
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return std::nullopt;
    }
    std::vector<std::uint8_t> bytes(kFrameBytes);
    input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!input || input.gcount() != static_cast<std::streamsize>(bytes.size())) {
        return std::nullopt;
    }
    return bytes;
}

}  // namespace

namespace uhf::storage {

FrameStore::FrameStore(std::filesystem::path directory) : directory_(std::move(directory)) {
    std::error_code error;
    std::filesystem::create_directories(directory_, error);
    if (error || !std::filesystem::is_directory(directory_, error) || error ||
        ::chmod(directory_.c_str(), kFrameDirectoryMode) < 0) {
        throw std::runtime_error("unable to prepare frame directory");
    }
}

std::optional<std::filesystem::path> FrameStore::save(
    const acquisition::PublishedSnapshot& snapshot,
    std::chrono::system_clock::time_point timestamp) {
    const std::optional<std::uint64_t> milliseconds = timestamp_milliseconds(timestamp);
    if (!milliseconds) {
        return std::nullopt;
    }

    std::vector<std::uint8_t> bytes;
    bytes.reserve(kFrameBytes);
    bytes.insert(bytes.end(), std::begin(kFrameMagic), std::end(kFrameMagic));
    append_u16(bytes, kFrameFormatVersion);
    append_u64(bytes, snapshot.generation);
    append_u64(bytes, *milliseconds);
    bytes.push_back(status_code(snapshot.payload.payload_status));
    bytes.push_back(0U);
    append_u16(bytes, static_cast<std::uint16_t>(domain::kPd1000RegisterCount));
    for (const std::uint16_t value : snapshot.payload.raw_registers) {
        append_u16(bytes, value);
    }
    const std::uint32_t checksum = crc32(bytes.data(), bytes.size());
    append_u16(bytes, static_cast<std::uint16_t>(checksum & 0xFFFFU));
    append_u16(bytes, static_cast<std::uint16_t>(checksum >> 16U));
    if (bytes.size() != kFrameBytes) {
        return std::nullopt;
    }

    const std::filesystem::path path = directory_ /
        ("frame-" + std::to_string(*milliseconds) + "-" +
         std::to_string(snapshot.generation) + ".bin");
    if (!write_atomic(path, bytes)) {
        return std::nullopt;
    }
    return path;
}

std::optional<FrameRecord> FrameStore::read(const std::filesystem::path& path) const {
    const std::optional<std::vector<std::uint8_t>> bytes = read_bytes(path);
    if (!bytes || !std::equal(std::begin(kFrameMagic), std::end(kFrameMagic), bytes->begin()) ||
        read_u16(bytes->data() + 4U) != kFrameFormatVersion ||
        read_u16(bytes->data() + 24U) != domain::kPd1000RegisterCount) {
        return std::nullopt;
    }
    const std::uint32_t expected_crc = crc32(bytes->data(), bytes->size() - 4U);
    const std::uint32_t stored_crc =
        static_cast<std::uint32_t>(read_u16(bytes->data() + bytes->size() - 4U)) |
        static_cast<std::uint32_t>(read_u16(bytes->data() + bytes->size() - 2U)) << 16U;
    if (expected_crc != stored_crc) {
        return std::nullopt;
    }
    const std::optional<domain::PayloadStatus> status = decode_status((*bytes)[22U]);
    if (!status) {
        return std::nullopt;
    }

    FrameRecord record;
    record.generation = read_u64(bytes->data() + 6U);
    record.timestamp = std::chrono::system_clock::time_point(
        std::chrono::milliseconds(read_u64(bytes->data() + 14U)));
    record.payload_status = *status;
    constexpr std::size_t kRawOffset = kHeaderBytes;
    for (std::size_t index = 0; index < record.raw_registers.size(); ++index) {
        record.raw_registers[index] = read_u16(bytes->data() + kRawOffset + index * 2U);
    }
    return record;
}

bool FrameStore::save_periodic(
    const acquisition::PublishedSnapshot& snapshot,
    std::chrono::system_clock::time_point now,
    std::chrono::seconds period) {
    if (snapshot.generation == 0U || snapshot.generation <= last_periodic_generation_ ||
        (last_periodic_save_ && now < *last_periodic_save_ + period)) {
        return false;
    }
    if (!save(snapshot, now)) {
        return false;
    }
    last_periodic_generation_ = snapshot.generation;
    last_periodic_save_ = now;
    return true;
}

bool FrameStore::cleanup_expired(
    std::chrono::system_clock::time_point now, std::chrono::seconds retention) {
    std::error_code error;
    std::optional<std::filesystem::path> newest_path;
    std::optional<std::chrono::system_clock::time_point> newest_timestamp;
    std::vector<std::pair<std::filesystem::path, FrameRecord>> records;
    for (const std::filesystem::directory_entry& entry :
         std::filesystem::directory_iterator(directory_, error)) {
        if (error) {
            return false;
        }
        if (!entry.is_regular_file(error) || error || entry.path().extension() != ".bin") {
            continue;
        }
        const std::optional<FrameRecord> record = read(entry.path());
        if (!record) {
            continue;
        }
        if (!newest_timestamp || record->timestamp > *newest_timestamp) {
            newest_timestamp = record->timestamp;
            newest_path = entry.path();
        }
        records.emplace_back(entry.path(), *record);
    }

    bool success = true;
    const std::chrono::system_clock::time_point cutoff = now - retention;
    for (const auto& item : records) {
        if (newest_path && item.first == *newest_path) {
            continue;
        }
        if (item.second.timestamp < cutoff && !std::filesystem::remove(item.first, error)) {
            success = false;
        }
        if (error) {
            success = false;
            error.clear();
        }
    }
    return success;
}

}  // namespace uhf::storage
