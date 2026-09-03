#!/usr/bin/env python3
"""Exercise authenticated ICD validation, replacement, download and rollback."""

from http.client import HTTPConnection
import hashlib
import json
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import time


def fail(message: str) -> None:
    raise SystemExit(f"IEC ICD web smoke failed: {message}")


def free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind(("127.0.0.1", 0))
        return int(sock.getsockname()[1])


def request(
    port: int, method: str, path: str, body: bytes | None = None,
    headers: dict[str, str] | None = None,
) -> tuple[int, bytes, dict[str, str]]:
    connection = HTTPConnection("127.0.0.1", port, timeout=2)
    try:
        connection.request(method, path, body=body, headers=headers or {})
        response = connection.getresponse()
        return response.status, response.read(), dict(response.getheaders())
    finally:
        connection.close()


def json_bytes(value: object) -> bytes:
    return json.dumps(value, ensure_ascii=False, separators=(",", ":")).encode("utf-8")


def main() -> int:
    if len(sys.argv) != 4:
        fail("expected server binary, web directory and reference ICD")
    binary = Path(sys.argv[1])
    web_dir = Path(sys.argv[2])
    reference = Path(sys.argv[3]).read_bytes()
    port = free_port()
    with tempfile.TemporaryDirectory(prefix="uhf-icd-web-") as state_text:
        state_dir = Path(state_text)
        process = subprocess.Popen(
            [
                str(binary), "--web", "--http-recovery", "--web-root", str(web_dir),
                "--state-dir", str(state_dir), "--no-acquisition", "--listen",
                f"127.0.0.1:{port}", "--privileged-socket", str(state_dir / "privileged.sock"),
            ],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
        )
        try:
            deadline = time.monotonic() + 5
            while time.monotonic() < deadline:
                if process.poll() is not None:
                    fail("server exited before the ICD endpoint was ready")
                try:
                    status, _, _ = request(port, "GET", "/healthz")
                    if status == 200:
                        break
                except OSError:
                    time.sleep(0.05)
            else:
                fail("server did not start")

            password = (state_dir / "initial-password").read_text(encoding="utf-8").strip()
            origin = f"http://127.0.0.1:{port}"
            status, body, headers = request(
                port, "POST", "/api/v1/session",
                json_bytes({"username": "admin", "password": password}),
                {"Content-Type": "application/json", "Origin": origin},
            )
            if status != 200:
                fail(f"login returned {status}: {body!r}")
            session = json.loads(body)
            cookie = headers["Set-Cookie"].split(";", 1)[0]
            csrf = str(session["csrf_token"])
            auth_headers = {"Cookie": cookie, "X-CSRF-Token": csrf, "Content-Type": "application/json"}

            status, _, _ = request(
                port, "PUT", "/api/v1/iec61850/icd", json_bytes({"content": "<bad/>"}),
                {"Cookie": cookie, "Content-Type": "application/json"},
            )
            if status != 403:
                fail(f"missing CSRF token returned {status}")

            invalid = json_bytes({"current_password": password, "content": "<!DOCTYPE SCL><SCL/>"})
            status, _, _ = request(port, "PUT", "/api/v1/iec61850/icd", invalid, auth_headers)
            if status != 400:
                fail(f"invalid ICD returned {status}")

            first = reference.decode("utf-8")
            upload = json_bytes({"current_password": password, "content": first})
            status, _, _ = request(port, "PUT", "/api/v1/iec61850/icd", upload, auth_headers)
            if status != 200:
                fail(f"valid ICD upload returned {status}")
            status, body, _ = request(port, "GET", "/api/v1/iec61850/icd", headers={"Cookie": cookie})
            if status != 200:
                fail(f"ICD status returned {status}")
            current = json.loads(body)
            if not current["available"] or not current["override_active"] or current["previous_available"]:
                fail(f"unexpected first upload status: {current!r}")
            if current["ied_name"] != "UHFPD12PD" or current["sha256"] != hashlib.sha256(reference).hexdigest():
                fail(f"ICD metadata is incorrect: {current!r}")
            status, body, _ = request(port, "GET", "/api/v1/iec61850/icd/download", headers={"Cookie": cookie})
            if status != 200 or body != reference:
                fail("current ICD download does not match uploaded content")

            second = first + "\n"
            upload = json_bytes({"current_password": password, "content": second})
            status, _, _ = request(port, "PUT", "/api/v1/iec61850/icd", upload, auth_headers)
            if status != 200:
                fail(f"second ICD upload returned {status}")
            status, body, _ = request(port, "GET", "/api/v1/iec61850/icd/previous/download", headers={"Cookie": cookie})
            if status != 200 or body != reference:
                fail("previous ICD download does not preserve the prior version")

            status, _, _ = request(
                port, "POST", "/api/v1/iec61850/icd/restore",
                json_bytes({"current_password": password}), auth_headers,
            )
            if status != 200:
                fail(f"ICD restore returned {status}")
            status, body, _ = request(port, "GET", "/api/v1/iec61850/icd", headers={"Cookie": cookie})
            restored = json.loads(body)
            if status != 200 or restored["sha256"] != hashlib.sha256(reference).hexdigest():
                fail(f"restore did not activate the previous ICD: {restored!r}")
            override_path = state_dir / "UHFPD1.icd"
            if not override_path.is_file() or (override_path.stat().st_mode & 0o777) != 0o600:
                fail("ICD override is not a private regular file")
            print("IEC ICD web smoke: OK")
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
