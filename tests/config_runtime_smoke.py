#!/usr/bin/env python3
"""Verify the gateway consumes a persisted configuration before binding Web."""

from http.client import HTTPConnection
import json
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import time


def fail(message: str) -> None:
    raise SystemExit(f"config runtime smoke failed: {message}")


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


def main() -> int:
    if len(sys.argv) != 3:
        fail("expected server binary and web directory")
    binary = Path(sys.argv[1])
    web_dir = Path(sys.argv[2])
    configured_web_port = free_port()
    configured_tcp_port = free_port()
    configuration = {
        "version": 7,
        "acquisition_device": "/dev/ttyS1",
        "acquisition_slave_id": 2,
        "acquisition_period_ms": 6000,
        "acquisition_response_timeout_ms": 150,
        "acquisition_max_retries": 2,
        "rtu_device": "/dev/ttyS4",
        "rtu_unit_id": 3,
        "modbus_tcp_bind": "127.0.0.1",
        "modbus_tcp_unit_id": 4,
        "modbus_tcp_port": configured_tcp_port,
        "web_port": configured_web_port,
        "tls_enabled": False,
        "iec_enabled": False,
        "iec_port": free_port(),
        "iec_ied_name": "UHFPD2",
        "storage_period_seconds": 600,
        "storage_retention_days": 2,
        "storage_min_free_bytes": 268435456,
    }
    with tempfile.TemporaryDirectory(prefix="uhf-config-runtime-") as state_text:
        state_dir = Path(state_text)
        config_path = state_dir / "config.json"
        config_path.write_text(json.dumps(configuration), encoding="utf-8")
        process = subprocess.Popen(
            [
                str(binary),
                "--web",
                "--web-root",
                str(web_dir),
                "--state-dir",
                str(state_dir),
                "--config",
                str(config_path),
                "--no-acquisition",
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
                    status, _, _ = request(configured_web_port, "GET", "/healthz")
                    if status == 200:
                        break
                except OSError:
                    time.sleep(0.05)
            else:
                fail("server did not bind configured web port")

            initial_password = (state_dir / "initial-password").read_text(encoding="utf-8").strip()
            origin = f"http://127.0.0.1:{configured_web_port}"
            status, body, headers = request(
                configured_web_port,
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
            config_status, config_body, _ = request(
                configured_web_port, "GET", "/api/v1/config", headers={"Cookie": cookie}
            )
            if config_status != 200:
                fail(f"config lookup returned {config_status}")
            loaded = json.loads(config_body)
            if loaded.get("version") != 7 or loaded.get("rtu_unit_id") != 3 or loaded.get("modbus_tcp_unit_id") != 4 or loaded.get("iec_enabled") is not False:
                fail(f"persisted configuration was not loaded: {loaded!r}")
            print("config runtime smoke: OK")
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
