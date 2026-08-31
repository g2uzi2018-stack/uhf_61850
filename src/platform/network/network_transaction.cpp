// SPDX-License-Identifier: GPL-3.0-only
#include "platform/network/network_transaction.hpp"

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <fcntl.h>
#include <fstream>
#include <limits>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace {

constexpr std::size_t kMaxTransactionBytes = 64U * 1024U;
constexpr mode_t kDirectoryMode = S_IRWXU;
constexpr mode_t kFileMode = S_IRUSR | S_IWUSR;

std::string json_escape(std::string_view value) {
    std::string result;
    result.reserve(value.size());
    for (const char character : value) {
        if (character == '"' || character == '\\') {
            result.push_back('\\');
        }
        result.push_back(character);
    }
    return result;
}

std::string transaction_json(const uhf::network::Transaction& transaction) {
    return "{\"version\":1,\"id\":" + std::to_string(transaction.id) +
        ",\"boot_id\":\"" + json_escape(transaction.boot_id) +
        "\",\"staged_boottime_ns\":" + std::to_string(transaction.staged_boottime_ns) +
        ",\"deadline_boottime_ns\":" + std::to_string(transaction.deadline_boottime_ns) +
        ",\"state\":\"" + uhf::network::transaction_state_name(transaction.state) +
        "\",\"previous\":" + uhf::network::to_flat_json(transaction.previous) +
        ",\"candidate\":" + uhf::network::to_flat_json(transaction.candidate) + "}\n";
}

bool read_string(std::string_view input, std::size_t& position, std::string& result) {
    if (position >= input.size() || input[position++] != '"') {
        return false;
    }
    result.clear();
    while (position < input.size()) {
        const char character = input[position++];
        if (character == '"') {
            return result.size() <= 256U;
        }
        if (static_cast<unsigned char>(character) < 0x20U) {
            return false;
        }
        if (character != '\\') {
            result.push_back(character);
            continue;
        }
        if (position >= input.size()) {
            return false;
        }
        const char escaped = input[position++];
        if (escaped != '"' && escaped != '\\') {
            return false;
        }
        result.push_back(escaped);
    }
    return false;
}

bool read_unsigned(std::string_view input, std::size_t& position, std::uint64_t& result) {
    const std::size_t begin = position;
    while (position < input.size() && input[position] >= '0' && input[position] <= '9') {
        ++position;
    }
    if (begin == position) {
        return false;
    }
    const auto parsed = std::from_chars(
        input.data() + begin, input.data() + position, result);
    return parsed.ec == std::errc{} && parsed.ptr == input.data() + position;
}

void skip_space(std::string_view input, std::size_t& position) noexcept {
    while (position < input.size()) {
        const unsigned char character = static_cast<unsigned char>(input[position]);
        if (character != ' ' && character != '\t' && character != '\r' && character != '\n') {
            break;
        }
        ++position;
    }
}

bool consume(std::string_view input, std::size_t& position, char expected) noexcept {
    skip_space(input, position);
    if (position >= input.size() || input[position] != expected) {
        return false;
    }
    ++position;
    return true;
}

bool extract_object(
    std::string_view input, std::size_t& position, std::string_view& object) noexcept {
    skip_space(input, position);
    if (position >= input.size() || input[position] != '{') {
        return false;
    }
    const std::size_t begin = position;
    std::size_t depth = 0U;
    bool quoted = false;
    bool escaped = false;
    for (; position < input.size(); ++position) {
        const char character = input[position];
        if (quoted) {
            if (escaped) {
                escaped = false;
            } else if (character == '\\') {
                escaped = true;
            } else if (character == '"') {
                quoted = false;
            }
            continue;
        }
        if (character == '"') {
            quoted = true;
        } else if (character == '{') {
            ++depth;
        } else if (character == '}' && depth-- == 1U) {
            ++position;
            object = input.substr(begin, position - begin);
            return true;
        }
    }
    return false;
}

bool parse_transaction(std::string_view input, uhf::network::Transaction& transaction) {
    std::size_t position = 0U;
    if (!consume(input, position, '{')) {
        return false;
    }
    bool version_seen = false;
    bool id_seen = false;
    bool boot_id_seen = false;
    bool staged_seen = false;
    bool deadline_seen = false;
    bool state_seen = false;
    bool previous_seen = false;
    bool candidate_seen = false;
    while (true) {
        std::string key;
        skip_space(input, position);
        if (!read_string(input, position, key) || !consume(input, position, ':')) {
            return false;
        }
        if (key == "version") {
            std::uint64_t value = 0U;
            if (version_seen || !read_unsigned(input, position, value) || value != 1U) {
                return false;
            }
            version_seen = true;
        } else if (key == "id") {
            if (id_seen || !read_unsigned(input, position, transaction.id) || transaction.id == 0U) {
                return false;
            }
            id_seen = true;
        } else if (key == "boot_id") {
            if (boot_id_seen || !read_string(input, position, transaction.boot_id) ||
                transaction.boot_id.empty()) {
                return false;
            }
            boot_id_seen = true;
        } else if (key == "staged_boottime_ns") {
            if (staged_seen || !read_unsigned(input, position, transaction.staged_boottime_ns)) {
                return false;
            }
            staged_seen = true;
        } else if (key == "deadline_boottime_ns") {
            if (deadline_seen || !read_unsigned(input, position, transaction.deadline_boottime_ns) ||
                transaction.deadline_boottime_ns < transaction.staged_boottime_ns) {
                return false;
            }
            deadline_seen = true;
        } else if (key == "state") {
            std::string state;
            if (state_seen || !read_string(input, position, state)) {
                return false;
            }
            if (state == "staged") {
                transaction.state = uhf::network::TransactionState::staged;
            } else if (state == "confirmed") {
                transaction.state = uhf::network::TransactionState::confirmed;
            } else if (state == "rolled_back") {
                transaction.state = uhf::network::TransactionState::rolled_back;
            } else {
                return false;
            }
            state_seen = true;
        } else if (key == "previous" || key == "candidate") {
            std::string_view object;
            if ((key == "previous" && previous_seen) ||
                (key == "candidate" && candidate_seen) || !extract_object(input, position, object)) {
                return false;
            }
            uhf::network::NetworkConfig parsed;
            if (!uhf::network::parse_flat_json(object, parsed)) {
                return false;
            }
            if (key == "previous") {
                transaction.previous = std::move(parsed);
                previous_seen = true;
            } else {
                transaction.candidate = std::move(parsed);
                candidate_seen = true;
            }
        } else {
            return false;
        }
        skip_space(input, position);
        if (position >= input.size()) {
            return false;
        }
        if (input[position] == '}') {
            ++position;
            break;
        }
        if (!consume(input, position, ',')) {
            return false;
        }
    }
    skip_space(input, position);
    return position == input.size() && version_seen && id_seen && boot_id_seen && staged_seen &&
        deadline_seen && state_seen && previous_seen && candidate_seen &&
        uhf::network::validate(transaction.previous).valid &&
        uhf::network::validate(transaction.candidate).valid;
}

std::optional<std::string> read_file(
    const std::filesystem::path& path, uhf::network::LoadStatus& status) {
    std::error_code error;
    if (!std::filesystem::exists(path, error)) {
        status = error ? uhf::network::LoadStatus::io_error : uhf::network::LoadStatus::none;
        return std::nullopt;
    }
    if (error || !std::filesystem::is_regular_file(path, error) || error ||
        std::filesystem::file_size(path, error) > kMaxTransactionBytes || error) {
        status = uhf::network::LoadStatus::corrupt;
        return std::nullopt;
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        status = uhf::network::LoadStatus::io_error;
        return std::nullopt;
    }
    std::string contents{
        std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    if (input.bad() || contents.size() > kMaxTransactionBytes) {
        status = uhf::network::LoadStatus::corrupt;
        return std::nullopt;
    }
    status = uhf::network::LoadStatus::valid;
    return contents;
}

bool write_atomic(const std::filesystem::path& path, std::string_view contents) noexcept {
    const std::filesystem::path temporary = path.string() + ".tmp";
    const int file_descriptor = ::open(
        temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, kFileMode);
    if (file_descriptor < 0) {
        return false;
    }
    std::size_t written = 0U;
    while (written < contents.size()) {
        const ssize_t result = ::write(
            file_descriptor, contents.data() + written, contents.size() - written);
        if (result <= 0) {
            ::close(file_descriptor);
            ::unlink(temporary.c_str());
            return false;
        }
        written += static_cast<std::size_t>(result);
    }
    if (::fchmod(file_descriptor, kFileMode) < 0 || ::fsync(file_descriptor) < 0 ||
        ::close(file_descriptor) < 0 || ::rename(temporary.c_str(), path.c_str()) < 0) {
        ::close(file_descriptor);
        ::unlink(temporary.c_str());
        return false;
    }
    const std::filesystem::path parent = path.parent_path().empty() ? "." : path.parent_path();
    const int directory_descriptor = ::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (directory_descriptor < 0) {
        return false;
    }
    const int result = ::fsync(directory_descriptor);
    ::close(directory_descriptor);
    return result == 0;
}

}  // namespace

namespace uhf::network {

std::uint64_t SystemClock::boottime_ns() const noexcept {
    timespec value{};
    if (::clock_gettime(CLOCK_BOOTTIME, &value) != 0 || value.tv_sec < 0 || value.tv_nsec < 0) {
        return 0U;
    }
    return static_cast<std::uint64_t>(value.tv_sec) * 1000000000ULL +
        static_cast<std::uint64_t>(value.tv_nsec);
}

std::string SystemClock::boot_id() const {
    std::ifstream input("/proc/sys/kernel/random/boot_id");
    std::string value;
    std::getline(input, value);
    while (!value.empty() && (value.back() == '\n' || value.back() == '\r' || value.back() == ' ' ||
                              value.back() == '\t')) {
        value.pop_back();
    }
    return value;
}

TransactionStore::TransactionStore(std::filesystem::path file) : file_(std::move(file)) {
    const std::filesystem::path directory = file_.parent_path().empty() ? "." : file_.parent_path();
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error || !std::filesystem::is_directory(directory, error) || error ||
        ::chmod(directory.c_str(), kDirectoryMode) < 0) {
        throw std::runtime_error("unable to prepare network transaction directory");
    }
}

LoadResult TransactionStore::load() const {
    LoadStatus status = LoadStatus::none;
    const std::optional<std::string> contents = read_file(file_, status);
    if (!contents) {
        return LoadResult{status, std::nullopt};
    }
    Transaction transaction;
    if (!parse_transaction(*contents, transaction)) {
        return LoadResult{LoadStatus::corrupt, std::nullopt};
    }
    return LoadResult{LoadStatus::valid, std::move(transaction)};
}

bool TransactionStore::save(const Transaction& transaction) const noexcept {
    return write_atomic(file_, transaction_json(transaction));
}

bool TransactionStore::clear() const noexcept {
    if (::unlink(file_.c_str()) < 0 && errno != ENOENT) {
        return false;
    }
    const std::filesystem::path parent = file_.parent_path().empty() ? "." : file_.parent_path();
    const int directory_descriptor = ::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (directory_descriptor < 0) {
        return false;
    }
    const int result = ::fsync(directory_descriptor);
    ::close(directory_descriptor);
    return result == 0;
}

const std::filesystem::path& TransactionStore::path() const noexcept {
    return file_;
}

TransactionManager::TransactionManager(
    TransactionStore& store,
    Backend& backend,
    const Clock& clock,
    std::uint64_t rollback_window_ns)
    : store_(store), backend_(backend), clock_(clock), rollback_window_ns_(rollback_window_ns) {}

TransactionResult TransactionManager::stage(const NetworkConfig& candidate) {
    last_error_.clear();
    if (!validate(candidate).valid) {
        return set_error(TransactionResult::invalid_candidate, "candidate network configuration is invalid");
    }
    const LoadResult existing = store_.load();
    if (existing.status == LoadStatus::corrupt) {
        return set_error(TransactionResult::corrupt, "network transaction is corrupt");
    }
    if (existing.status == LoadStatus::io_error) {
        return set_error(TransactionResult::storage_error, "network transaction cannot be read");
    }
    if (existing.transaction && existing.transaction->state == TransactionState::staged) {
        return set_error(TransactionResult::busy, "another network transaction is pending");
    }
    NetworkConfig previous;
    if (!backend_.read_current(previous) || !validate(previous).valid) {
        return set_error(TransactionResult::backend_error, "current network state is unavailable");
    }
    const std::string boot_id = clock_.boot_id();
    const std::uint64_t staged = clock_.boottime_ns();
    if (boot_id.empty() || staged == 0U ||
        rollback_window_ns_ > std::numeric_limits<std::uint64_t>::max() - staged) {
        return set_error(TransactionResult::backend_error, "monotonic boot clock is unavailable");
    }
    Transaction transaction;
    transaction.id = next_id_++;
    transaction.boot_id = boot_id;
    transaction.staged_boottime_ns = staged;
    transaction.deadline_boottime_ns = staged + rollback_window_ns_;
    transaction.state = TransactionState::staged;
    transaction.previous = previous;
    transaction.candidate = candidate;
    if (!store_.save(transaction)) {
        return set_error(TransactionResult::storage_error, "network transaction cannot be persisted");
    }
    if (!backend_.apply_stage(previous, candidate)) {
        if (!backend_.rollback(previous, candidate)) {
            return set_error(TransactionResult::backend_error, "stage failed and rollback is pending");
        }
        if (!store_.clear()) {
            return set_error(TransactionResult::storage_error, "failed stage transaction cannot be cleared");
        }
        return set_error(TransactionResult::backend_error, "network stage failed");
    }
    return TransactionResult::ok;
}

TransactionResult TransactionManager::confirm() {
    last_error_.clear();
    const LoadResult loaded = store_.load();
    if (loaded.status == LoadStatus::none) {
        return set_error(TransactionResult::no_transaction, "no network transaction is pending");
    }
    if (loaded.status == LoadStatus::corrupt) {
        return set_error(TransactionResult::corrupt, "network transaction is corrupt");
    }
    if (loaded.status == LoadStatus::io_error || !loaded.transaction) {
        return set_error(TransactionResult::storage_error, "network transaction cannot be read");
    }
    const Transaction& transaction = *loaded.transaction;
    if (transaction.state != TransactionState::staged) {
        return set_error(TransactionResult::no_transaction, "network transaction is no longer pending");
    }
    const std::string boot_id = clock_.boot_id();
    const std::uint64_t now = clock_.boottime_ns();
    if (boot_id != transaction.boot_id) {
        return rollback_loaded(transaction, TransactionResult::boot_changed);
    }
    if (now == 0U || now >= transaction.deadline_boottime_ns) {
        return rollback_loaded(transaction, TransactionResult::expired);
    }
    if (!backend_.confirm(transaction.previous, transaction.candidate)) {
        return set_error(TransactionResult::backend_error, "network confirmation failed");
    }
    Transaction confirmed = transaction;
    confirmed.state = TransactionState::confirmed;
    if (!store_.save(confirmed) || !store_.clear()) {
        return set_error(TransactionResult::storage_error, "confirmed network transaction cannot be cleared");
    }
    return TransactionResult::ok;
}

TransactionResult TransactionManager::rollback_now() {
    last_error_.clear();
    const LoadResult loaded = store_.load();
    if (loaded.status == LoadStatus::none) {
        return set_error(TransactionResult::no_transaction, "no network transaction is pending");
    }
    if (loaded.status == LoadStatus::corrupt) {
        return set_error(TransactionResult::corrupt, "network transaction is corrupt");
    }
    if (loaded.status == LoadStatus::io_error || !loaded.transaction) {
        return set_error(TransactionResult::storage_error, "network transaction cannot be read");
    }
    if (loaded.transaction->state != TransactionState::staged) {
        return set_error(TransactionResult::no_transaction, "network transaction is no longer pending");
    }
    return rollback_loaded(*loaded.transaction, TransactionResult::ok);
}

TransactionResult TransactionManager::rollback_if_needed() {
    last_error_.clear();
    const LoadResult loaded = store_.load();
    if (loaded.status == LoadStatus::none) {
        return set_error(TransactionResult::no_transaction, "no network transaction is pending");
    }
    if (loaded.status == LoadStatus::corrupt) {
        return set_error(TransactionResult::corrupt, "network transaction is corrupt");
    }
    if (loaded.status == LoadStatus::io_error || !loaded.transaction) {
        return set_error(TransactionResult::storage_error, "network transaction cannot be read");
    }
    const Transaction& transaction = *loaded.transaction;
    if (transaction.state != TransactionState::staged) {
        if (!store_.clear()) {
            return set_error(TransactionResult::storage_error, "completed network transaction cannot be cleared");
        }
        return TransactionResult::ok;
    }
    const std::string boot_id = clock_.boot_id();
    const std::uint64_t now = clock_.boottime_ns();
    if (boot_id == transaction.boot_id && now != 0U && now < transaction.deadline_boottime_ns) {
        return set_error(TransactionResult::not_due, "network transaction is still within its confirmation window");
    }
    return rollback_loaded(
        transaction,
        boot_id == transaction.boot_id ? TransactionResult::expired : TransactionResult::boot_changed);
}

LoadResult TransactionManager::inspect() const {
    return store_.load();
}

const std::string& TransactionManager::last_error() const noexcept {
    return last_error_;
}

TransactionResult TransactionManager::rollback_loaded(
    const Transaction& transaction, TransactionResult reason) {
    if (!backend_.rollback(transaction.previous, transaction.candidate)) {
        return set_error(TransactionResult::backend_error, "network rollback failed");
    }
    Transaction rolled_back = transaction;
    rolled_back.state = TransactionState::rolled_back;
    if (!store_.save(rolled_back) || !store_.clear()) {
        return set_error(TransactionResult::storage_error, "rolled back network transaction cannot be cleared");
    }
    if (reason == TransactionResult::expired || reason == TransactionResult::boot_changed) {
        return set_error(reason, transaction_result_name(reason));
    }
    return TransactionResult::ok;
}

TransactionResult TransactionManager::set_error(
    TransactionResult result, std::string message) {
    last_error_ = std::move(message);
    return result;
}

std::string transaction_state_name(TransactionState state) noexcept {
    switch (state) {
    case TransactionState::staged:
        return "staged";
    case TransactionState::confirmed:
        return "confirmed";
    case TransactionState::rolled_back:
        return "rolled_back";
    }
    return "staged";
}

std::string transaction_result_name(TransactionResult result) noexcept {
    switch (result) {
    case TransactionResult::ok:
        return "ok";
    case TransactionResult::not_due:
        return "not_due";
    case TransactionResult::no_transaction:
        return "no_transaction";
    case TransactionResult::invalid_candidate:
        return "invalid_candidate";
    case TransactionResult::busy:
        return "busy";
    case TransactionResult::expired:
        return "expired";
    case TransactionResult::boot_changed:
        return "boot_changed";
    case TransactionResult::corrupt:
        return "corrupt";
    case TransactionResult::backend_error:
        return "backend_error";
    case TransactionResult::storage_error:
        return "storage_error";
    }
    return "backend_error";
}

}  // namespace uhf::network
