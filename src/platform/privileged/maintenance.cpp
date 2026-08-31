// SPDX-License-Identifier: GPL-3.0-only
#include "platform/privileged/maintenance.hpp"

#include <cerrno>
#include <sys/wait.h>
#include <unistd.h>

namespace {

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

}  // namespace

namespace uhf::privileged {

bool ExecMaintenanceRunner::restart_service() {
    const char* const arguments[] = {
        "/usr/bin/systemctl", "restart", "uhf-gateway.service", nullptr};
    return run_fixed(arguments[0], arguments);
}

bool ExecMaintenanceRunner::reboot() {
    const char* const arguments[] = {"/sbin/reboot", nullptr};
    return run_fixed(arguments[0], arguments);
}

}  // namespace uhf::privileged
