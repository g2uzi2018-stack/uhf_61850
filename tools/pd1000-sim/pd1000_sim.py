#!/usr/bin/env python3
"""Small PTY-backed PD1000 Modbus RTU simulator for host development."""

import argparse
import os
from pathlib import Path
import select
import signal
import struct
import sys
import termios
import time
import tty


REQUEST_SIZE = 8
REGISTER_COUNT = 3615
SPECTRUM_OFFSET = 15
SPECTRUM_POINTS = 3600


def crc16(data: bytes) -> int:
    crc = 0xFFFF
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = (crc >> 1) ^ 0xA001 if crc & 1 else crc >> 1
    return crc


def parse_plan(path: Path) -> list[tuple[bytes, int, int]]:
    plan: list[tuple[bytes, int, int]] = []
    for line_number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        fields = line.split()
        if len(fields) != 11:
            raise ValueError(f"fixture line {line_number} has {len(fields)} fields")
        ordinal, start, count = (int(fields[0]), int(fields[1]), int(fields[2]))
        frame = bytes(int(field, 16) for field in fields[3:])
        if ordinal != line_number or len(frame) != REQUEST_SIZE:
            raise ValueError(f"invalid fixture line {line_number}")
        if crc16(frame[:6]) != frame[6] | (frame[7] << 8):
            raise ValueError(f"bad fixture CRC on line {line_number}")
        plan.append((frame, start, count))
    if len(plan) != 31:
        raise ValueError(f"fixture has {len(plan)} requests, expected 31")
    return plan


def int16_register(value: int) -> int:
    return value & 0xFFFF


def build_registers() -> list[int]:
    registers = [0] * REGISTER_COUNT
    registers[0] = int16_register(-55)
    registers[1] = 12
    registers[2] = int16_register(-50)
    registers[3] = 180
    registers[4] = 240
    for index in range(SPECTRUM_POINTS):
        phase_bin = index % 72
        registers[SPECTRUM_OFFSET + index] = int16_register(-60 + phase_bin % 6)
    return registers


def response_for(request: bytes, registers: list[int], start: int, count: int) -> bytes:
    start_index = start - 10001
    values = registers[start_index : start_index + count]
    payload = b"".join(struct.pack(">H", value) for value in values)
    header = bytes((request[0], request[1], len(payload)))
    response = header + payload
    response_crc = crc16(response)
    return response + bytes((response_crc & 0xFF, response_crc >> 8))


def write_all(fd: int, data: bytes) -> None:
    position = 0
    while position < len(data):
        written = os.write(fd, data[position:])
        if written <= 0:
            raise OSError("PTY write failed")
        position += written


def run(
    device: Path | None,
    file_descriptor: int | None,
    fixture: Path,
    response_delay_ms: float,
) -> int:
    plan = parse_plan(fixture)
    registers = build_registers()
    device_fd = os.open(device, os.O_RDWR | os.O_NOCTTY) if device is not None else os.dup(file_descriptor)
    old_attributes = termios.tcgetattr(device_fd) if device is not None else None
    if old_attributes is not None:
        tty.setraw(device_fd)
    stopping = False

    def stop(_signum: int, _frame: object) -> None:
        nonlocal stopping
        stopping = True

    signal.signal(signal.SIGTERM, stop)
    signal.signal(signal.SIGINT, stop)
    buffer = bytearray()
    plan_index = 0
    print("pd1000 simulator ready", flush=True)
    try:
        while not stopping:
            readable, _, _ = select.select([device_fd], [], [], 0.25)
            if not readable:
                continue
            chunk = os.read(device_fd, 256)
            if not chunk:
                continue
            buffer.extend(chunk)
            while len(buffer) >= REQUEST_SIZE:
                expected, start, count = plan[plan_index]
                received = bytes(buffer[:REQUEST_SIZE])
                del buffer[:REQUEST_SIZE]
                if received != expected:
                    raise RuntimeError(
                        f"unexpected request at plan index {plan_index + 1}: {received.hex()}"
                    )
                if response_delay_ms > 0:
                    time.sleep(response_delay_ms / 1000.0)
                write_all(device_fd, response_for(received, registers, start, count))
                plan_index = (plan_index + 1) % len(plan)
    finally:
        if old_attributes is not None:
            termios.tcsetattr(device_fd, termios.TCSANOW, old_attributes)
        os.close(device_fd)
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    device_group = parser.add_mutually_exclusive_group(required=True)
    device_group.add_argument("--device", type=Path, help="PTY slave path")
    device_group.add_argument("--fd", type=int, help="inherited PTY master file descriptor")
    parser.add_argument("--fixture", type=Path, required=True, help="request-plan fixture")
    parser.add_argument("--response-delay-ms", type=float, default=0.0)
    arguments = parser.parse_args()
    if arguments.response_delay_ms < 0:
        parser.error("--response-delay-ms must not be negative")
    try:
        return run(arguments.device, arguments.fd, arguments.fixture, arguments.response_delay_ms)
    except (OSError, RuntimeError, ValueError) as error:
        print(f"pd1000 simulator failed: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
