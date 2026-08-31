// SPDX-License-Identifier: GPL-3.0-only
#include "acquisition/acquisition.hpp"

#include <cassert>
#include <fcntl.h>
#include <pty.h>
#include <termios.h>
#include <unistd.h>

int main() {
    int master = -1;
    int slave = -1;
    char slave_name[128]{};
    assert(::openpty(&master, &slave, slave_name, nullptr, nullptr) == 0);
    assert(::close(slave) == 0);

    {
        uhf::acquisition::PosixSerialPort port(slave_name);
        termios attributes{};
        assert(::tcgetattr(master, &attributes) == 0);
        assert(::cfgetispeed(&attributes) == B115200);
        assert(::cfgetospeed(&attributes) == B115200);
        assert((attributes.c_cflag & CSIZE) == CS8);
        assert((attributes.c_cflag & PARENB) == 0);
        assert((attributes.c_cflag & CSTOPB) == 0);
        assert((attributes.c_cflag & CLOCAL) != 0);
        assert((attributes.c_cflag & CREAD) != 0);
#ifdef CRTSCTS
        assert((attributes.c_cflag & CRTSCTS) == 0);
#endif
        assert(port.write_all(nullptr, 0U));
    }

    assert(::close(master) == 0);
    return 0;
}
