#!/usr/bin/env python3
"""Exercise the product's development HTTP server over a real TCP socket."""

from http.client import HTTPConnection
from pathlib import Path
import socket
import subprocess
import sys
import time


def fail(message: str) -> None:
    raise SystemExit(f"web server smoke failed: {message}")


def free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind(("127.0.0.1", 0))
        return int(sock.getsockname()[1])


def request(port: int, method: str, path: str) -> tuple[int, bytes]:
    connection = HTTPConnection("127.0.0.1", port, timeout=1)
    try:
        connection.request(method, path)
        response = connection.getresponse()
        return response.status, response.read()
    finally:
        connection.close()


def main() -> int:
    if len(sys.argv) != 3:
        fail("expected the server binary and web directory")

    binary = Path(sys.argv[1])
    web_dir = Path(sys.argv[2])
    port = free_port()
    process = subprocess.Popen(
        [str(binary), "--web", "--web-root", str(web_dir), "--listen", f"127.0.0.1:{port}"],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )

    try:
        deadline = time.monotonic() + 5
        health_status = None
        health_body = b""
        while time.monotonic() < deadline:
            if process.poll() is not None:
                stderr = process.stderr.read() if process.stderr else ""
                fail(f"server exited early: {stderr.strip()}")
            try:
                health_status, health_body = request(port, "GET", "/healthz")
                break
            except OSError:
                time.sleep(0.05)

        if health_status != 200 or b'"web_auth":"pending"' not in health_body:
            fail(f"unexpected health response: {health_status} {health_body!r}")

        page_status, page_body = request(port, "GET", "/")
        if page_status != 200 or "用户管理".encode() not in page_body:
            fail("root page was not served")

        asset_status, _ = request(port, "GET", "/styles.css")
        if asset_status != 200:
            fail("CSS asset was not served")

        traversal_status, _ = request(port, "GET", "/%2e%2e/CMakeLists.txt")
        if traversal_status == 200:
            fail("path traversal was served")

        method_status, _ = request(port, "POST", "/")
        if method_status != 405:
            fail(f"unexpected method response: {method_status}")

        print("web server smoke: OK")
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
