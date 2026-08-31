#!/usr/bin/env python3
"""Verify the authenticated WebSocket telemetry channel and its origin guard."""

from http.client import HTTPConnection
import base64
import hashlib
import json
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import time


def fail(message: str) -> None:
    raise SystemExit(f"websocket smoke failed: {message}")


def free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind(("127.0.0.1", 0))
        return int(sock.getsockname()[1])


def http_request(
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


def receive_exact(sock: socket.socket, size: int) -> bytes:
    payload = bytearray()
    while len(payload) < size:
        chunk = sock.recv(size - len(payload))
        if not chunk:
            fail("connection closed while reading")
        payload.extend(chunk)
    return bytes(payload)


def receive_http_headers(sock: socket.socket) -> bytes:
    payload = bytearray()
    deadline = time.monotonic() + 2
    while b"\r\n\r\n" not in payload:
        if time.monotonic() >= deadline:
            fail("timed out waiting for WebSocket handshake")
        chunk = sock.recv(1024)
        if not chunk:
            fail("connection closed during WebSocket handshake")
        payload.extend(chunk)
    return bytes(payload)


def receive_frame(sock: socket.socket) -> tuple[int, bytes]:
    first, second = receive_exact(sock, 2)
    if second & 0x80:
        fail("server WebSocket frame must not be masked")
    length = second & 0x7F
    if length == 126:
        length = int.from_bytes(receive_exact(sock, 2), "big")
    elif length == 127:
        length = int.from_bytes(receive_exact(sock, 8), "big")
    if length > 128 * 1024:
        fail("WebSocket payload exceeded limit")
    return first & 0x0F, receive_exact(sock, length)


def masked_frame(opcode: int, payload: bytes) -> bytes:
    if len(payload) > 125:
        fail("test control frame is too large")
    mask = b"test"
    masked = bytes(value ^ mask[index % 4] for index, value in enumerate(payload))
    return bytes([0x80 | opcode, 0x80 | len(payload)]) + mask + masked


def websocket_handshake(port: int, cookie: str, origin: str) -> socket.socket:
    sock = socket.create_connection(("127.0.0.1", port), timeout=2)
    key = base64.b64encode(b"0123456789abcdef").decode("ascii")
    request = (
        f"GET /ws/v1/telemetry HTTP/1.1\r\n"
        f"Host: 127.0.0.1:{port}\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        f"Sec-WebSocket-Key: {key}\r\n"
        f"Origin: {origin}\r\n"
        f"Cookie: {cookie}\r\n\r\n"
    ).encode("ascii")
    sock.sendall(request)
    response = receive_http_headers(sock)
    if not response.startswith(b"HTTP/1.1 101 Switching Protocols"):
        sock.close()
        fail(f"unexpected WebSocket handshake: {response!r}")
    accept = None
    for line in response.split(b"\r\n"):
        if line.lower().startswith(b"sec-websocket-accept:"):
            accept = line.split(b":", 1)[1].strip().decode("ascii")
    expected = base64.b64encode(
        hashlib.sha1((key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode("ascii")).digest()
    ).decode("ascii")
    if accept != expected:
        sock.close()
        fail("WebSocket accept key mismatch")
    return sock


def main() -> int:
    if len(sys.argv) != 3:
        fail("expected server binary and web directory")
    binary = Path(sys.argv[1])
    web_dir = Path(sys.argv[2])
    port = free_port()
    with tempfile.TemporaryDirectory(prefix="uhf-websocket-") as state_text:
        state_dir = Path(state_text)
        process = subprocess.Popen(
            [
                str(binary),
                "--web",
                "--web-root",
                str(web_dir),
                "--state-dir",
                str(state_dir),
                "--simulate",
                "--listen",
                f"127.0.0.1:{port}",
                "--modbus-tcp-listen",
                "127.0.0.1:0",
            ],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        try:
            deadline = time.monotonic() + 5
            while time.monotonic() < deadline:
                if process.poll() is not None:
                    stderr = process.stderr.read() if process.stderr else ""
                    fail(f"server exited early: {stderr.strip()}")
                try:
                    status, _, _ = http_request(port, "GET", "/healthz")
                    if status == 200:
                        break
                except OSError:
                    time.sleep(0.05)
            else:
                fail("server did not start")

            initial_password = (state_dir / "initial-password").read_text(encoding="utf-8").strip()
            origin = f"http://127.0.0.1:{port}"
            status, body, headers = http_request(
                port,
                "POST",
                "/api/v1/session",
                json.dumps(
                    {"username": "admin", "password": initial_password},
                    separators=(",", ":"),
                ).encode("utf-8"),
                {"Content-Type": "application/json", "Origin": origin},
            )
            if status != 200:
                fail(f"login returned {status}: {body!r}")
            cookie = headers["Set-Cookie"].split(";", 1)[0]

            invalid_origin_sock = socket.create_connection(("127.0.0.1", port), timeout=2)
            invalid_key = base64.b64encode(b"0123456789abcdef").decode("ascii")
            invalid_origin_sock.sendall(
                (
                    "GET /ws/v1/telemetry HTTP/1.1\r\n"
                    f"Host: 127.0.0.1:{port}\r\n"
                    "Upgrade: websocket\r\nConnection: Upgrade\r\n"
                    "Sec-WebSocket-Version: 13\r\n"
                    f"Sec-WebSocket-Key: {invalid_key}\r\n"
                    "Origin: http://evil.example\r\n"
                    f"Cookie: {cookie}\r\n\r\n"
                ).encode("ascii")
            )
            invalid_response = receive_http_headers(invalid_origin_sock)
            invalid_origin_sock.close()
            if not invalid_response.startswith(b"HTTP/1.1 403"):
                fail(f"cross-origin WebSocket was not rejected: {invalid_response!r}")

            websocket = websocket_handshake(port, cookie, origin)
            try:
                deadline = time.monotonic() + 3
                telemetry = None
                while time.monotonic() < deadline:
                    opcode, payload = receive_frame(websocket)
                    if opcode != 1:
                        continue
                    message = json.loads(payload)
                    if message.get("type") == "telemetry":
                        telemetry = message
                        break
                if telemetry is None:
                    fail("no telemetry message received")
                snapshot = telemetry.get("snapshot", {})
                measurements = {
                    item.get("name"): item.get("value") for item in snapshot.get("measurements", [])
                }
                if measurements != {
                    "average": -55,
                    "frequency": 12,
                    "peak": -50,
                    "phase": 180,
                    "noise": 240,
                }:
                    fail(f"unexpected telemetry measurements: {measurements!r}")
                if len(snapshot.get("spectrum", [])) != 3600:
                    fail("telemetry spectrum length mismatch")

                websocket.sendall(masked_frame(0x9, b"ping"))
                deadline = time.monotonic() + 2
                while time.monotonic() < deadline:
                    opcode, payload = receive_frame(websocket)
                    if opcode == 0xA:
                        if payload != b"ping":
                            fail("WebSocket pong payload mismatch")
                        break
                else:
                    fail("no WebSocket pong received")
                websocket.sendall(masked_frame(0x8, b""))
            finally:
                websocket.close()
            print("websocket smoke: OK")
            return 0
        finally:
            process.terminate()
            try:
                process.communicate(timeout=2)
            except subprocess.TimeoutExpired:
                process.kill()
                process.communicate()


if __name__ == "__main__":
    raise SystemExit(main())
