#!/usr/bin/env python3
"""Exercise authenticated passive FTP upload/download against the product daemon."""

from ftplib import FTP, error_perm, error_temp
from http.client import HTTPConnection
from io import BytesIO
import json
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import time


def fail(message: str) -> None:
    raise SystemExit(f"FTP server smoke failed: {message}")


def free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind(("127.0.0.1", 0))
        return int(sock.getsockname()[1])


def connect(port: int) -> FTP:
    client = FTP()
    client.connect("127.0.0.1", port, timeout=3)
    client.encoding = "utf-8"
    return client


def request(
    port: int,
    method: str,
    path: str,
    body: bytes | None = None,
    headers: dict[str, str] | None = None,
) -> tuple[int, bytes, dict[str, str]]:
    connection = HTTPConnection("127.0.0.1", port, timeout=3)
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
    web_port, ftp_port = free_port(), free_port()
    while ftp_port == web_port:
        ftp_port = free_port()

    with tempfile.TemporaryDirectory(prefix="uhf-ftp-") as state_text:
        state_dir = Path(state_text)
        configuration = json.loads(
            (web_dir.parent / "config" / "defaults.json").read_text(encoding="utf-8")
        )
        configuration["web_port"] = web_port
        configuration["modbus_tcp_port"] = free_port()
        configuration["iec_enabled"] = False
        configuration["ftp_enabled"] = True
        configuration["ftp_port"] = ftp_port
        config_path = state_dir / "config.json"
        config_path.write_text(json.dumps(configuration), encoding="utf-8")
        ftp_root = state_dir / "ftp"
        ftp_root.mkdir()
        (ftp_root / ".upload-stale").write_bytes(b"partial")

        process = subprocess.Popen(
            [
                str(binary),
                "--web",
                "--http-recovery",
                "--no-acquisition",
                "--web-root",
                str(web_dir),
                "--state-dir",
                str(state_dir),
                "--config",
                str(config_path),
                "--listen",
                f"127.0.0.1:{web_port}",
                "--privileged-socket",
                str(state_dir / "privileged.sock"),
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
                    probe = connect(ftp_port)
                    probe.close()
                    break
                except OSError:
                    time.sleep(0.05)
            else:
                fail("FTP listener did not start")

            password = (state_dir / "initial-password").read_text(encoding="utf-8").strip()
            rejected = connect(ftp_port)
            try:
                rejected.login("admin", "incorrect-password")
                fail("incorrect password was accepted")
            except error_perm:
                pass
            finally:
                rejected.close()

            client = connect(ftp_port)
            client.login("admin", password)
            if (ftp_root / ".upload-stale").exists():
                fail("stale atomic-upload file was not cleaned on startup")

            extra_clients = [connect(ftp_port) for _ in range(3)]
            overflow_client = FTP()
            try:
                overflow_client.connect("127.0.0.1", ftp_port, timeout=3)
                fail("FTP connection limit was not enforced")
            except error_temp:
                pass
            finally:
                overflow_client.close()
                for extra_client in extra_clients:
                    extra_client.close()

            client.set_pasv(False)
            try:
                client.nlst()
                fail("active FTP mode was accepted")
            except error_perm:
                pass
            finally:
                client.set_pasv(True)

            payload = b"UHF FTP round trip\x00\xff\n"
            if not client.storbinary("STOR capture.bin", BytesIO(payload)).startswith("226"):
                fail("upload did not complete")
            if "capture.bin" not in client.nlst():
                fail("uploaded file is missing from directory listing")
            if client.size("capture.bin") != len(payload):
                fail("SIZE does not match uploaded content")
            downloaded = bytearray()
            client.retrbinary("RETR capture.bin", downloaded.extend)
            if bytes(downloaded) != payload:
                fail("downloaded content does not match upload")

            replacement = b"replacement"
            client.storbinary("STOR capture.bin", BytesIO(replacement))
            downloaded.clear()
            client.retrbinary("RETR capture.bin", downloaded.extend)
            if bytes(downloaded) != replacement:
                fail("atomic replacement was not visible")

            for index in range(63):
                client.storbinary(f"STOR quota-{index:02d}.bin", BytesIO(b""))
            try:
                client.storbinary("STOR quota-overflow.bin", BytesIO(b""))
                fail("FTP file-count quota was not enforced")
            except error_perm:
                pass

            try:
                client.storbinary("STOR ../escape.bin", BytesIO(b"escape"))
                fail("path traversal upload was accepted")
            except error_perm:
                pass
            if (state_dir / "escape.bin").exists():
                fail("path traversal created a file outside the FTP root")

            outside = state_dir / "outside.bin"
            outside.write_bytes(b"outside")
            (ftp_root / "linked.bin").symlink_to(outside)
            try:
                client.retrbinary("RETR linked.bin", lambda _: None)
                fail("symbolic-link download was accepted")
            except error_perm:
                pass

            oversized = ftp_root / "oversized.bin"
            with oversized.open("wb") as output:
                output.truncate(64 * 1024 * 1024 + 1)
            try:
                client.retrbinary("RETR oversized.bin", lambda _: None)
                fail("oversized download was accepted")
            except error_perm:
                pass
            client.quit()

            stored = ftp_root / "capture.bin"
            if stored.read_bytes() != replacement or (stored.stat().st_mode & 0o777) != 0o600:
                fail("stored file content or mode is incorrect")
            if list(ftp_root.glob(".upload-*")):
                fail("temporary upload file was not cleaned up")

            origin = f"http://127.0.0.1:{web_port}"
            status, body, headers = request(
                web_port,
                "POST",
                "/api/v1/session",
                json.dumps({"username": "admin", "password": password}).encode(),
                {"Content-Type": "application/json", "Origin": origin},
            )
            if status != 200:
                fail(f"Web login for shared credential test returned {status}: {body!r}")
            session = json.loads(body)
            cookie = headers["Set-Cookie"].split(";", 1)[0]
            status, body, _ = request(
                web_port, "GET", "/api/v1/config", headers={"Cookie": cookie}
            )
            if status != 200:
                fail(f"FTP configuration lookup returned {status}: {body!r}")
            updated_configuration = json.loads(body)
            version = int(updated_configuration.pop("version"))
            updated_configuration["ftp_enabled"] = False
            status, body, _ = request(
                web_port,
                "PUT",
                "/api/v1/config",
                json.dumps(updated_configuration).encode(),
                {
                    "Content-Type": "application/json",
                    "Cookie": cookie,
                    "X-CSRF-Token": str(session["csrf_token"]),
                    "If-Match": f'"{version}"',
                },
            )
            if status != 200 or json.loads(body).get("restart_required") is not True:
                fail(f"FTP setting did not require a safe service restart: {status} {body!r}")
            new_password = "FTP-Smoke-Password-2026!"
            status, body, _ = request(
                web_port,
                "PUT",
                "/api/v1/password",
                json.dumps(
                    {"current_password": password, "new_password": new_password}
                ).encode(),
                {
                    "Content-Type": "application/json",
                    "Cookie": cookie,
                    "X-CSRF-Token": str(session["csrf_token"]),
                },
            )
            if status != 200:
                fail(f"Web password change returned {status}: {body!r}")
            old_credential = connect(ftp_port)
            try:
                old_credential.login("admin", password)
                fail("FTP accepted the previous Web password")
            except error_perm:
                pass
            finally:
                old_credential.close()
            current_credential = connect(ftp_port)
            current_credential.login("admin", new_password)
            current_credential.quit()
            print("FTP server smoke: OK")
            return 0
        finally:
            process.terminate()
            try:
                process.communicate(timeout=3)
            except subprocess.TimeoutExpired:
                process.kill()
                process.communicate()


if __name__ == "__main__":
    raise SystemExit(main())
