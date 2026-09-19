#!/usr/bin/env python3
"""Verify the running gateway serves Modbus RTU through a configured PTY."""

from http.client import HTTPConnection
import json
import os
from pathlib import Path
import pty
import select
import socket
import struct
import subprocess
import sys
import tempfile
import time
import tty


def fail(message: str) -> None:
    raise SystemExit(f"runtime Modbus RTU smoke failed: {message}")


def free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind(("127.0.0.1", 0))
        return int(sock.getsockname()[1])


def request(
    port: int,
    method: str,
    path: str,
    body: bytes | None = None,
    headers: dict[str, str] | None = None,
) -> tuple[int, bytes, dict[str, str]]:
    connection = HTTPConnection("127.0.0.1", port, timeout=2)
    try:
        connection.request(method, path, body=body, headers=headers or {})
        response = connection.getresponse()
        return response.status, response.read(), dict(response.getheaders())
    finally:
        connection.close()


def crc16(payload: bytes) -> int:
    crc = 0xFFFF
    for byte in payload:
        crc ^= byte
        for _ in range(8):
            crc = (crc >> 1) ^ 0xA001 if crc & 1 else crc >> 1
    return crc


def receive_exact(file_descriptor: int, size: int, timeout: float = 2.0) -> bytes:
    payload = bytearray()
    deadline = time.monotonic() + timeout
    while len(payload) < size:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            fail("timed out waiting for RTU response")
        ready, _, _ = select.select([file_descriptor], [], [], remaining)
        if not ready:
            fail("timed out waiting for RTU response")
        chunk = os.read(file_descriptor, size - len(payload))
        if not chunk:
            fail("RTU PTY closed before response")
        payload.extend(chunk)
    return bytes(payload)


def main() -> int:
    if len(sys.argv) != 3:
        fail("expected server binary and web directory")
    binary = Path(sys.argv[1])
    web_dir = Path(sys.argv[2])
    web_port = free_port()
    master_fd, slave_fd = pty.openpty()
    tty.setraw(master_fd)
    tty.setraw(slave_fd)
    rtu_device = os.ttyname(slave_fd)
    with tempfile.TemporaryDirectory(prefix="uhf-web-rtu-") as state_text:
        state_dir = Path(state_text)
        process = subprocess.Popen(
            [
                str(binary),
                "--web",
                "--http-recovery",
                "--web-root",
                str(web_dir),
                "--state-dir",
                str(state_dir),
                "--simulate",
                "--listen",
                f"127.0.0.1:{web_port}",
                "--modbus-tcp-listen",
                "127.0.0.1:0",
                "--modbus-rtu-device",
                rtu_device,
            ],
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
                    fail(f"server exited early: {stderr.strip()}")
                try:
                    status, _, _ = request(web_port, "GET", "/healthz")
                    if status == 200:
                        break
                except OSError:
                    time.sleep(0.05)
            else:
                fail("server did not start")

            request_body = bytes([1, 4, 0x27, 0x11, 0, 5])
            crc = crc16(request_body)
            os.write(master_fd, request_body + bytes([crc & 0xFF, crc >> 8]))
            response = receive_exact(master_fd, 15)
            if response[:3] != bytes([1, 4, 10]):
                fail(f"unexpected RTU response header: {response[:3]!r}")
            registers = list(struct.unpack("!5H", response[3:13]))
            expected = [0xFFC9, 12, 0xFFCE, 180, 240]
            if registers != expected:
                fail(f"unexpected RTU registers: {registers!r}")
            if crc16(response[:-2]) != response[-2] | response[-1] << 8:
                fail("RTU response CRC mismatch")

            initial_password = (state_dir / "initial-password").read_text(encoding="utf-8").strip()
            origin = f"http://127.0.0.1:{web_port}"
            login_status, login_body, login_headers = request(
                web_port,
                "POST",
                "/api/v1/session",
                json.dumps(
                    {"username": "admin", "password": initial_password},
                    separators=(",", ":"),
                ).encode("utf-8"),
                {"Content-Type": "application/json", "Origin": origin},
            )
            if login_status != 200:
                fail(f"login returned {login_status}: {login_body!r}")
            cookie = login_headers.get("Set-Cookie", "").split(";", 1)[0]
            health_status, health_body, _ = request(
                web_port, "GET", "/api/v1/health", headers={"Cookie": cookie}
            )
            if health_status != 200:
                fail(f"health returned {health_status}")
            health_payload = json.loads(health_body)
            if health_payload.get("modbus_rtu", {}).get("status") != "up":
                fail(f"Modbus RTU health is not up: {health_payload!r}")
            print("runtime Modbus RTU smoke: OK")
            return 0
        finally:
            process.terminate()
            try:
                process.communicate(timeout=2)
            except subprocess.TimeoutExpired:
                process.kill()
                process.communicate()
            os.close(master_fd)


if __name__ == "__main__":
    raise SystemExit(main())
