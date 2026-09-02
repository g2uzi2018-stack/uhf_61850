// SPDX-License-Identifier: GPL-3.0-only
#include "platform/privileged/maintenance.hpp"

#include <cctype>
#include <cerrno>
#include <cstddef>
#include <ctime>
#include <fcntl.h>
#include <filesystem>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <time.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

constexpr char kTimeSyncDirectory[] = "/etc/systemd/timesyncd.conf.d";
constexpr char kTimeSyncConfig[] =
    "/etc/systemd/timesyncd.conf.d/90-uhf-gateway.conf";
constexpr char kTimeSyncTemporary[] =
    "/etc/systemd/timesyncd.conf.d/90-uhf-gateway.conf.tmp";
constexpr mode_t kTimeSyncFileMode = S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH;

bool run_fixed(const char* command, const char* const* arguments) {
    const pid_t child = ::fork();
    if (child < 0) {
        return false;
    }
    if (child == 0) {
        ::execv(command, const_cast<char* const*>(arguments));
        _exit(127);
    }
    int status = 0;
    while (::waitpid(child, &status, 0) < 0) {
        if (errno != EINTR) {
            return false;
        }
    }
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

bool parse_digits(std::string_view value, std::size_t offset, std::size_t count, int& result) {
    result = 0;
    for (std::size_t index = 0U; index < count; ++index) {
        const unsigned char character = static_cast<unsigned char>(value[offset + index]);
        if (character < static_cast<unsigned char>('0') ||
            character > static_cast<unsigned char>('9')) {
            return false;
        }
        result = result * 10 + static_cast<int>(character - static_cast<unsigned char>('0'));
    }
    return true;
}

bool parse_local_time(std::string_view value, std::tm& local_time, std::time_t& epoch) noexcept {
    if (value.size() != 16U && value.size() != 19U) {
        return false;
    }
    if (value[4] != '-' || value[7] != '-' || value[10] != 'T' || value[13] != ':') {
        return false;
    }
    const bool has_seconds = value.size() == 19U;
    if (has_seconds && value[16] != ':') {
        return false;
    }

    int year = 0;
    int month = 0;
    int day = 0;
    int hour = 0;
    int minute = 0;
    int second = 0;
    if (!parse_digits(value, 0U, 4U, year) || !parse_digits(value, 5U, 2U, month) ||
        !parse_digits(value, 8U, 2U, day) || !parse_digits(value, 11U, 2U, hour) ||
        !parse_digits(value, 14U, 2U, minute) ||
        (has_seconds && !parse_digits(value, 17U, 2U, second))) {
        return false;
    }
    if (year < 1970 || year > 2099 || month < 1 || month > 12 || day < 1 || day > 31 ||
        hour > 23 || minute > 59 || second > 59) {
        return false;
    }

    local_time = {};
    local_time.tm_year = year - 1900;
    local_time.tm_mon = month - 1;
    local_time.tm_mday = day;
    local_time.tm_hour = hour;
    local_time.tm_min = minute;
    local_time.tm_sec = second;
    local_time.tm_isdst = -1;
    errno = 0;
    epoch = std::mktime(&local_time);
    if (epoch == static_cast<std::time_t>(-1) || errno == EOVERFLOW) {
        return false;
    }
    return local_time.tm_year == year - 1900 && local_time.tm_mon == month - 1 &&
        local_time.tm_mday == day && local_time.tm_hour == hour &&
        local_time.tm_min == minute && local_time.tm_sec == second;
}

bool write_all(int file_descriptor, std::string_view contents) noexcept {
    std::size_t written = 0U;
    while (written < contents.size()) {
        const ssize_t result = ::write(
            file_descriptor, contents.data() + written, contents.size() - written);
        if (result > 0) {
            written += static_cast<std::size_t>(result);
        } else if (result < 0 && errno == EINTR) {
            continue;
        } else {
            return false;
        }
    }
    return true;
}

bool write_sntp_config(std::string_view server) noexcept {
    if (::mkdir(kTimeSyncDirectory, S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH) < 0 &&
        errno != EEXIST) {
        return false;
    }
    struct stat directory_status {};
    if (::stat(kTimeSyncDirectory, &directory_status) < 0 ||
        !S_ISDIR(directory_status.st_mode)) {
        return false;
    }

    const int file_descriptor = ::open(
        kTimeSyncTemporary,
        O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW,
        kTimeSyncFileMode);
    if (file_descriptor < 0) {
        return false;
    }
    const std::string contents = "# Managed by uhf-gatewayd.\n[Time]\nNTP=" +
        std::string(server) + "\n";
    bool success = write_all(file_descriptor, contents) &&
        ::fchmod(file_descriptor, kTimeSyncFileMode) == 0 &&
        ::fsync(file_descriptor) == 0;
    if (::close(file_descriptor) < 0) {
        success = false;
    }
    if (!success) {
        (void)::unlink(kTimeSyncTemporary);
        return false;
    }
    if (::rename(kTimeSyncTemporary, kTimeSyncConfig) < 0) {
        (void)::unlink(kTimeSyncTemporary);
        return false;
    }

    const int directory_descriptor = ::open(
        kTimeSyncDirectory, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (directory_descriptor < 0) {
        return false;
    }
    const int sync_result = ::fsync(directory_descriptor);
    (void)::close(directory_descriptor);
    return sync_result == 0;
}

}  // namespace

namespace uhf::privileged {

bool valid_sntp_server(std::string_view server) noexcept {
    if (server.empty() || server.size() > 253U || server.front() == '.' ||
        server.back() == '.') {
        return false;
    }
    char previous = '\0';
    for (const char character : server) {
        const unsigned char unsigned_character = static_cast<unsigned char>(character);
        if (!(std::isalnum(unsigned_character) != 0 || character == '.' || character == '-')) {
            return false;
        }
        if (character == '.' && previous == '.') {
            return false;
        }
        previous = character;
    }
    return true;
}

bool valid_system_time(std::string_view local_time) noexcept {
    std::tm parsed{};
    std::time_t epoch = 0;
    return parse_local_time(local_time, parsed, epoch);
}

bool ExecMaintenanceRunner::restart_service() {
    const char* const arguments[] = {
        "/usr/bin/systemctl", "restart", "uhf-gateway.service", nullptr};
    return run_fixed(arguments[0], arguments);
}

bool ExecMaintenanceRunner::reboot() {
    const char* const arguments[] = {"/usr/bin/systemctl", "--no-block", "reboot", nullptr};
    return run_fixed(arguments[0], arguments);
}

bool ExecMaintenanceRunner::configure_sntp(std::string_view server) {
    if (!valid_sntp_server(server) || !write_sntp_config(server)) {
        return false;
    }
    const char* const restart_arguments[] = {
        "/usr/bin/systemctl", "restart", "systemd-timesyncd.service", nullptr};
    if (!run_fixed(restart_arguments[0], restart_arguments)) {
        return false;
    }
    const char* const enable_arguments[] = {
        "/usr/bin/timedatectl", "set-ntp", "true", nullptr};
    return run_fixed(enable_arguments[0], enable_arguments);
}

bool ExecMaintenanceRunner::disable_sntp() {
    const char* const arguments[] = {
        "/usr/bin/timedatectl", "set-ntp", "false", nullptr};
    return run_fixed(arguments[0], arguments);
}

bool ExecMaintenanceRunner::set_system_time(std::string_view local_time) {
    std::tm parsed{};
    std::time_t epoch = 0;
    if (!parse_local_time(local_time, parsed, epoch)) {
        return false;
    }
    (void)disable_sntp();
    const timespec clock_value{epoch, 0};
    return ::clock_settime(CLOCK_REALTIME, &clock_value) == 0;
}

}  // namespace uhf::privileged
