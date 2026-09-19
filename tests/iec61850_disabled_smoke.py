#!/usr/bin/env python3
from pathlib import Path
import socket
import subprocess
import sys
import tempfile


def free_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def assert_closed(port):
    with socket.socket() as sock:
        sock.settimeout(0.2)
        assert sock.connect_ex(("127.0.0.1", port)) != 0


def main():
    binary = Path(sys.argv[1])
    web_root = Path(sys.argv[2])
    web_port = free_port()
    modbus_port = free_port()
    while modbus_port == web_port:
        modbus_port = free_port()
    with tempfile.TemporaryDirectory(prefix="uhf-noiec-") as root_text:
        root = Path(root_text)
        result = subprocess.run(
            [
                str(binary),
                "--web",
                "--http-recovery",
                "--simulate",
                "--web-root", str(web_root),
                "--state-dir", str(root),
                "--data-dir", str(root / "data"),
                "--listen", f"127.0.0.1:{web_port}",
                "--modbus-tcp-listen", f"127.0.0.1:{modbus_port}",
                "--no-modbus-rtu",
            ],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=3,
            check=False,
        )
        assert result.returncode == 1
        assert b"IEC 61850 support is disabled in this build" in result.stderr
        assert_closed(web_port)
        assert_closed(modbus_port)
        assert not (root / "data" / "frames").exists()


if __name__ == "__main__":
    main()
