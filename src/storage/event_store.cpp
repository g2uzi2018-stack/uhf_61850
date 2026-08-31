// SPDX-License-Identifier: GPL-3.0-only
#include "storage/event_store.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <fcntl.h>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

constexpr char kEventMagic[] = {'U', 'H', 'F', 'E'};
constexpr std::size_t kHeaderBytes = 54U;
constexpr std::size_t kFrameHeaderBytes = 12U;
constexpr std::size_t kFrameBytes =
    kFrameHeaderBytes + uhf::domain::kPd1000RegisterCount * 2U;
constexpr std::size_t kCrcBytes = 4U;
constexpr std::size_t kMaximumEventBytes =
    kHeaderBytes + uhf::storage::kMaxEventFrames * kFrameBytes + kCrcBytes;
constexpr mode_t kEventFileMode = S_IRUSR | S_IWUSR;
constexpr mode_t kEventDirectoryMode = S_IRWXU;
constexpr std::uint8_t kStrongReason = 1U;
constexpr std::uint8_t kSuddenReason = 2U;
constexpr std::uint8_t kReasonMask = kStrongReason | kSuddenReason;

void append_u16(std::vector<std::uint8_t>& bytes, std::uint16_t value) {
    bytes.push_back(static_cast<std::uint8_t>(value & 0x00FFU));
    bytes.push_back(static_cast<std::uint8_t>(value >> 8U));
}

void append_u32(std::vector<std::uint8_t>& bytes, std::uint32_t value) {
    for (std::size_t index = 0; index < 4U; ++index) {
        bytes.push_back(static_cast<std::uint8_t>(value >> (index * 8U)));
    }
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

std::uint32_t read_u32(const std::uint8_t* bytes) noexcept {
    std::uint32_t value = 0U;
    for (std::size_t index = 0; index < 4U; ++index) {
        value |= static_cast<std::uint32_t>(bytes[index]) << (index * 8U);
    }
    return value;
}

std::uint64_t read_u64(const std::uint8_t* bytes) noexcept {
    std::uint64_t value = 0U;
    for (std::size_t index = 0; index < 8U; ++index) {
        value |= static_cast<std::uint64_t>(bytes[index]) << (index * 8U);
    }
    return value;
}

std::int32_t read_i32(const std::uint8_t* bytes) noexcept {
    return static_cast<std::int32_t>(read_u32(bytes));
}

void append_i32(std::vector<std::uint8_t>& bytes, std::int32_t value) {
    append_u32(bytes, static_cast<std::uint32_t>(value));
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
        temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, kEventFileMode);
    if (file_descriptor < 0) {
        return false;
    }
    bool open = true;
    bool renamed = false;
    bool complete = false;
    std::size_t written = 0U;
    while (written < bytes.size()) {
        const ssize_t result = ::write(
            file_descriptor, bytes.data() + written, bytes.size() - written);
        if (result <= 0) {
            break;
        }
        written += static_cast<std::size_t>(result);
    }
    if (written == bytes.size() && ::fchmod(file_descriptor, kEventFileMode) == 0 &&
        ::fsync(file_descriptor) == 0) {
        const int close_result = ::close(file_descriptor);
        open = false;
        if (close_result == 0 && ::rename(temporary.c_str(), path.c_str()) == 0) {
            renamed = true;
            const std::filesystem::path parent =
                path.parent_path().empty() ? "." : path.parent_path();
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
    const std::uintmax_t size = std::filesystem::file_size(path, error);
    if (error || size < kHeaderBytes + kCrcBytes || size > kMaximumEventBytes) {
        return std::nullopt;
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return std::nullopt;
    }
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!input || input.gcount() != static_cast<std::streamsize>(bytes.size())) {
        return std::nullopt;
    }
    return bytes;
}

bool stats_are_valid(const uhf::storage::EventReasonStats& stats, bool present) noexcept {
    if (!present) {
        return stats.trigger_count == 0U && stats.max_peak_dbm == 0 && stats.max_delta_db == 0;
    }
    return stats.trigger_count > 0U && stats.max_delta_db >= 0;
}

bool contains_generation(
    const std::vector<uhf::acquisition::PublishedSnapshot>& frames,
    std::uint64_t generation) noexcept {
    for (const uhf::acquisition::PublishedSnapshot& frame : frames) {
        if (frame.generation == generation) {
            return true;
        }
    }
    return false;
}

}  // namespace

namespace uhf::storage {

EventBundleStore::EventBundleStore(std::filesystem::path directory)
    : directory_(std::move(directory)) {
    std::error_code error;
    std::filesystem::create_directories(directory_, error);
    if (error || !std::filesystem::is_directory(directory_, error) || error ||
        ::chmod(directory_.c_str(), kEventDirectoryMode) < 0 || !cleanup_incomplete()) {
        throw std::runtime_error("unable to prepare event directory");
    }
}

std::optional<std::filesystem::path> EventBundleStore::save(const EventBundle& bundle) const {
    const std::optional<std::uint64_t> milliseconds =
        timestamp_milliseconds(bundle.first_triggered_at_utc);
    if (!milliseconds || bundle.id == 0U || bundle.reason_mask == 0U ||
        (bundle.reason_mask & static_cast<std::uint8_t>(~kReasonMask)) != 0U ||
        bundle.frames.empty() || bundle.frames.size() > kMaxEventFrames ||
        !stats_are_valid(bundle.strong, (bundle.reason_mask & kStrongReason) != 0U) ||
        !stats_are_valid(bundle.sudden, (bundle.reason_mask & kSuddenReason) != 0U)) {
        return std::nullopt;
    }

    std::vector<std::uint8_t> bytes;
    bytes.reserve(kHeaderBytes + bundle.frames.size() * kFrameBytes + kCrcBytes);
    bytes.insert(bytes.end(), std::begin(kEventMagic), std::end(kEventMagic));
    append_u16(bytes, kEventFormatVersion);
    append_u64(bytes, *milliseconds);
    append_u64(bytes, bundle.id);
    bytes.push_back(bundle.reason_mask);
    bytes.push_back(bundle.partial ? 1U : 0U);
    append_u16(bytes, 0U);
    append_u32(bytes, bundle.strong.trigger_count);
    append_i32(bytes, bundle.strong.max_peak_dbm);
    append_i32(bytes, bundle.strong.max_delta_db);
    append_u32(bytes, bundle.sudden.trigger_count);
    append_i32(bytes, bundle.sudden.max_peak_dbm);
    append_i32(bytes, bundle.sudden.max_delta_db);
    append_u16(bytes, static_cast<std::uint16_t>(bundle.frames.size()));
    append_u16(bytes, 0U);
    for (std::size_t index = 0; index < bundle.frames.size(); ++index) {
        const acquisition::PublishedSnapshot& frame = bundle.frames[index];
        if (frame.generation == 0U) {
            return std::nullopt;
        }
        for (std::size_t previous = 0; previous < index; ++previous) {
            if (bundle.frames[previous].generation == frame.generation) {
                return std::nullopt;
            }
        }
        append_u64(bytes, frame.generation);
        bytes.push_back(status_code(frame.payload.payload_status));
        bytes.push_back(0U);
        append_u16(bytes, static_cast<std::uint16_t>(domain::kPd1000RegisterCount));
        for (const std::uint16_t value : frame.payload.raw_registers) {
            append_u16(bytes, value);
        }
    }
    if (bytes.size() != kHeaderBytes + bundle.frames.size() * kFrameBytes) {
        return std::nullopt;
    }
    const std::uint32_t checksum = crc32(bytes.data(), bytes.size());
    append_u32(bytes, checksum);

    const std::filesystem::path path = directory_ /
        ("event-" + std::to_string(*milliseconds) + "-" + std::to_string(bundle.id) + ".bin");
    if (!write_atomic(path, bytes)) {
        return std::nullopt;
    }
    return path;
}

std::optional<EventBundle> EventBundleStore::read(const std::filesystem::path& path) const {
    const std::optional<std::vector<std::uint8_t>> bytes = read_bytes(path);
    if (!bytes || !std::equal(std::begin(kEventMagic), std::end(kEventMagic), bytes->begin()) ||
        read_u16(bytes->data() + 4U) != kEventFormatVersion) {
        return std::nullopt;
    }
    const std::uint16_t frame_count = read_u16(bytes->data() + 50U);
    if (frame_count == 0U || frame_count > kMaxEventFrames ||
        bytes->size() != kHeaderBytes + static_cast<std::size_t>(frame_count) * kFrameBytes +
                kCrcBytes) {
        return std::nullopt;
    }
    const std::uint32_t expected_crc = crc32(bytes->data(), bytes->size() - kCrcBytes);
    const std::uint32_t stored_crc = read_u32(bytes->data() + bytes->size() - kCrcBytes);
    if (expected_crc != stored_crc) {
        return std::nullopt;
    }

    EventBundle bundle;
    bundle.first_triggered_at_utc = std::chrono::system_clock::time_point(
        std::chrono::milliseconds(read_u64(bytes->data() + 6U)));
    bundle.id = read_u64(bytes->data() + 14U);
    bundle.reason_mask = (*bytes)[22U];
    if (bundle.id == 0U || bundle.reason_mask == 0U ||
        (bundle.reason_mask & static_cast<std::uint8_t>(~kReasonMask)) != 0U ||
        (*bytes)[23U] > 1U || (*bytes)[24U] != 0U || (*bytes)[25U] != 0U ||
        (*bytes)[52U] != 0U || (*bytes)[53U] != 0U) {
        return std::nullopt;
    }
    bundle.partial = (*bytes)[23U] != 0U;
    bundle.strong.trigger_count = read_u32(bytes->data() + 26U);
    bundle.strong.max_peak_dbm = read_i32(bytes->data() + 30U);
    bundle.strong.max_delta_db = read_i32(bytes->data() + 34U);
    bundle.sudden.trigger_count = read_u32(bytes->data() + 38U);
    bundle.sudden.max_peak_dbm = read_i32(bytes->data() + 42U);
    bundle.sudden.max_delta_db = read_i32(bytes->data() + 46U);
    if (!stats_are_valid(bundle.strong, (bundle.reason_mask & kStrongReason) != 0U) ||
        !stats_are_valid(bundle.sudden, (bundle.reason_mask & kSuddenReason) != 0U)) {
        return std::nullopt;
    }

    bundle.frames.reserve(frame_count);
    std::size_t offset = kHeaderBytes;
    for (std::size_t index = 0; index < frame_count; ++index) {
        acquisition::PublishedSnapshot frame;
        frame.generation = read_u64(bytes->data() + offset);
        const std::optional<domain::PayloadStatus> status =
            decode_status((*bytes)[offset + 8U]);
        if (frame.generation == 0U || !status || (*bytes)[offset + 9U] != 0U ||
            read_u16(bytes->data() + offset + 10U) != domain::kPd1000RegisterCount ||
            contains_generation(bundle.frames, frame.generation)) {
            return std::nullopt;
        }
        for (std::size_t register_index = 0;
             register_index < domain::kPd1000RegisterCount; ++register_index) {
            frame.payload.raw_registers[register_index] = read_u16(
                bytes->data() + offset + kFrameHeaderBytes + register_index * 2U);
        }
        frame.payload = domain::parse_pd1000_registers(frame.payload.raw_registers);
        if (frame.payload.payload_status != *status) {
            return std::nullopt;
        }
        bundle.frames.push_back(std::move(frame));
        offset += kFrameBytes;
    }
    return bundle;
}

bool EventBundleStore::cleanup_incomplete() const {
    std::error_code error;
    for (const std::filesystem::directory_entry& entry :
         std::filesystem::directory_iterator(directory_, error)) {
        if (error) {
            return false;
        }
        if (entry.path().extension() != ".tmp") {
            continue;
        }
        std::error_code remove_error;
        const bool removed = std::filesystem::remove(entry.path(), remove_error);
        if (remove_error || !removed) {
            return false;
        }
    }
    return !error;
}

}  // namespace uhf::storage
