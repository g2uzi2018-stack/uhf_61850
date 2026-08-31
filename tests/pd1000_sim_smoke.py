#!/usr/bin/env python3
"""Verify a complete 31-block request/response cycle through a Linux PTY."""

import os
from pathlib import Path
import pty
import select
import signal
import subprocess
import sys
import time


REQUEST_SIZE = 8


def fail(message: str) -> None:
    raise SystemExit(f"PD1000 simulator smoke failed: {message}")


def crc16(data: bytes) -> int:
    crc = 0xFFFF
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = (crc >> 1) ^ 0xA001 if crc & 1 else crc >> 1
    return crc


def load_fixture(path: Path) -> list[tuple[bytes, int, int]]:
    result = []
    for line_number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        fields = line.split()
        if len(fields) != 11:
            fail(f"fixture line {line_number} is malformed")
        frame = bytes(int(field, 16) for field in fields[3:])
        count = int(fields[2])
        result.append((frame, int(fields[1]), count))
    return result


def read_exact(fd: int, size: int, timeout: float) -> bytes:
    result = bytearray()
    deadline = time.monotonic() + timeout
    while len(result) < size:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            fail(f"timed out after {len(result)}/{size} response bytes")
        readable, _, _ = select.select([fd], [], [], remaining)
        if not readable:
            continue
        chunk = os.read(fd, size - len(result))
        if not chunk:
            fail("PTY closed while reading response")
        result.extend(chunk)
    return bytes(result)


def main() -> int:
    if len(sys.argv) != 3:
        fail("expected simulator path and request-plan fixture")
    simulator = Path(sys.argv[1])
    fixture = Path(sys.argv[2])
    plan = load_fixture(fixture)
    master_fd, slave_fd = pty.openpty()
    slave_path = os.ttyname(slave_fd)
    process = subprocess.Popen(
        [sys.executable, str(simulator), "--device", slave_path, "--fixture", str(fixture)],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    os.close(slave_fd)
    try:
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            if process.poll() is not None:
                stderr = process.stderr.read() if process.stderr else ""
                fail(f"simulator exited early: {stderr.strip()}")
            if process.stdout is not None:
                line = process.stdout.readline()
                if line == "pd1000 simulator ready\n":
                    break
            time.sleep(0.01)
        else:
            fail("simulator did not become ready")

        for ordinal, (request, start, count) in enumerate(plan, 1):
            if len(request) != REQUEST_SIZE or crc16(request[:6]) != request[6] | request[7] << 8:
                fail(f"fixture request {ordinal} is invalid")
            os.write(master_fd, request)
            response_size = 5 + count * 2
            response = read_exact(master_fd, response_size, 2)
            if response[0:2] != bytes((1, 4)) or response[2] != count * 2:
                fail(f"response header {ordinal} is invalid")
            if crc16(response[:-2]) != response[-2] | response[-1] << 8:
                fail(f"response CRC {ordinal} is invalid")
            expected_first_value = 0xFFC9 if start == 10001 else 0xFFC4
            if int.from_bytes(response[3:5], "big") != expected_first_value:
                fail(f"response {ordinal} does not contain deterministic data")

        print("PD1000 simulator smoke: OK")
        return 0
    finally:
        os.kill(process.pid, signal.SIGTERM)
        try:
            process.communicate(timeout=2)
        except subprocess.TimeoutExpired:
            process.kill()
            process.communicate()
        os.close(master_fd)


if __name__ == "__main__":
    raise SystemExit(main())
