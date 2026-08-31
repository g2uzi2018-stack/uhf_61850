#!/usr/bin/env python3
"""Verify the product web listener defaults to HTTPS with a generated certificate."""

from http.client import HTTPSConnection, HTTPConnection, RemoteDisconnected
import json
from pathlib import Path
import socket
import ssl
import subprocess
import sys
import tempfile
import time


def fail(message: str) -> None:
    raise SystemExit(f"web TLS smoke failed: {message}")


def free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind(("127.0.0.1", 0))
        return int(sock.getsockname()[1])


def https_request(
    port: int,
    method: str,
    path: str,
    body: bytes | None = None,
    headers: dict[str, str] | None = None,
) -> tuple[int, bytes, dict[str, str]]:
    context = ssl._create_unverified_context()
    connection = HTTPSConnection("127.0.0.1", port, timeout=2, context=context)
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
    port = free_port()
    with tempfile.TemporaryDirectory(prefix="uhf-web-tls-") as state_text:
        state_dir = Path(state_text)
        process = subprocess.Popen(
            [
                str(binary),
                "--web",
                "--web-root",
                str(web_dir),
                "--state-dir",
                str(state_dir),
                "--no-acquisition",
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
                    status, body, _ = https_request(port, "GET", "/healthz")
                    if status != 200:
                        fail(f"HTTPS health returned {status}: {body!r}")
                    break
                except (OSError, ssl.SSLError):
                    time.sleep(0.05)
            else:
                fail("HTTPS server did not start")

            certificate = state_dir / "tls" / "server.crt"
            private_key = state_dir / "tls" / "server.key"
            if not certificate.is_file() or not private_key.is_file():
                fail("generated TLS files are missing")

            plaintext = HTTPConnection("127.0.0.1", port, timeout=2)
            try:
                try:
                    plaintext.request("GET", "/healthz")
                    plaintext.getresponse()
                    fail("plaintext HTTP was accepted on the HTTPS listener")
                except (ConnectionResetError, RemoteDisconnected, OSError):
                    pass
            finally:
                plaintext.close()

            initial_password = (state_dir / "initial-password").read_text(encoding="utf-8").strip()
            origin = f"https://127.0.0.1:{port}"
            status, body, headers = https_request(
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
                fail(f"HTTPS login returned {status}: {body!r}")
            cookie = headers.get("Set-Cookie", "")
            if "Secure" not in cookie or "HttpOnly" not in cookie or "SameSite=Strict" not in cookie:
                fail(f"secure session cookie attributes are missing: {cookie!r}")
            print("web TLS smoke: OK")
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
