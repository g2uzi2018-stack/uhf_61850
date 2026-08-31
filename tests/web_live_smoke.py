#!/usr/bin/env python3
"""Verify the real C++ web service exposes a simulator-backed snapshot."""

from http.client import HTTPConnection
import json
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import time


def fail(message: str) -> None:
    raise SystemExit(f"web live smoke failed: {message}")


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


def json_body(payload: dict[str, str]) -> bytes:
    return json.dumps(payload, separators=(",", ":")).encode("utf-8")


def main() -> int:
    if len(sys.argv) != 3:
        fail("expected server binary and web directory")
    binary = Path(sys.argv[1])
    web_dir = Path(sys.argv[2])
    port = free_port()
    with tempfile.TemporaryDirectory(prefix="uhf-web-live-") as state_text:
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
                    status, _, _ = request(port, "GET", "/healthz")
                    if status == 200:
                        break
                except OSError:
                    time.sleep(0.05)
            else:
                fail("server did not start")

            initial_password = (state_dir / "initial-password").read_text(encoding="utf-8").strip()
            origin = f"http://127.0.0.1:{port}"
            login_status, login_body, login_headers = request(
                port,
                "POST",
                "/api/v1/session",
                json_body({"username": "admin", "password": initial_password}),
                {"Content-Type": "application/json", "Origin": origin},
            )
            if login_status != 200:
                fail(f"login returned {login_status}: {login_body!r}")
            cookie = login_headers.get("Set-Cookie", "").split(";", 1)[0]
            if not cookie:
                fail("login did not return a session cookie")

            snapshot_status = 0
            snapshot_payload: dict[str, object] = {}
            deadline = time.monotonic() + 5
            while time.monotonic() < deadline:
                snapshot_status, snapshot_body, _ = request(
                    port, "GET", "/api/v1/snapshot/latest", headers={"Cookie": cookie}
                )
                if snapshot_status == 200:
                    snapshot_payload = json.loads(snapshot_body)
                    break
                time.sleep(0.05)
            if snapshot_status != 200:
                fail(f"simulator snapshot returned {snapshot_status}")
            if snapshot_payload.get("payload_status") != "good":
                fail(f"unexpected payload status: {snapshot_payload!r}")
            measurements = snapshot_payload.get("measurements")
            if not isinstance(measurements, list) or len(measurements) != 5:
                fail("expected five measurements")
            values = {item.get("name"): item.get("value") for item in measurements}
            if values != {
                "average": -55,
                "frequency": 12,
                "peak": -50,
                "phase": 180,
                "noise": 240,
            }:
                fail(f"unexpected measurements: {values!r}")
            spectrum = snapshot_payload.get("spectrum")
            valid = snapshot_payload.get("spectrum_valid")
            if not isinstance(spectrum, list) or len(spectrum) != 3600:
                fail("expected 3600 spectrum points")
            if not isinstance(valid, list) or len(valid) != 3600 or not all(valid):
                fail("spectrum validity mismatch")
            if spectrum[:7] != [-60, -59, -58, -57, -56, -55, -60]:
                fail(f"unexpected spectrum prefix: {spectrum[:7]!r}")

            health_status, health_body, _ = request(
                port, "GET", "/api/v1/health", headers={"Cookie": cookie}
            )
            if health_status != 200:
                fail(f"health returned {health_status}")
            health_payload = json.loads(health_body)
            if health_payload.get("acquisition", {}).get("status") != "up":
                fail(f"acquisition health is not up: {health_payload!r}")
            print("web live smoke: OK")
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
