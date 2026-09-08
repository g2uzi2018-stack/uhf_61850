#!/usr/bin/env python3
"""Verify an authenticated ICD upload rebuilds the live MMS model."""

from http.client import HTTPConnection
import json
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import time


def fail(message: str) -> None:
    raise SystemExit(f"IEC runtime reload smoke failed: {message}")


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
    connection = HTTPConnection("127.0.0.1", port, timeout=3)
    try:
        connection.request(method, path, body=body, headers=headers or {})
        response = connection.getresponse()
        return response.status, response.read(), dict(response.getheaders())
    finally:
        connection.close()


def main() -> int:
    if len(sys.argv) != 5:
        fail("expected server binary, web directory, ICD and probe binary")
    binary = Path(sys.argv[1])
    web_dir = Path(sys.argv[2])
    icd = Path(sys.argv[3]).read_bytes()
    probe = Path(sys.argv[4])
    web_port, iec_port, modbus_port = free_port(), free_port(), free_port()
    with tempfile.TemporaryDirectory(prefix="uhf-iec-runtime-") as state_text:
        state_dir = Path(state_text)
        configuration = json.loads(
            (web_dir.parent / "config" / "defaults.json").read_text(encoding="utf-8")
        )
        configuration["modbus_tcp_bind"] = "192.0.2.123"
        configuration["iec_port"] = iec_port
        (state_dir / "config.json").write_text(
            json.dumps(configuration), encoding="utf-8"
        )
        process = subprocess.Popen(
            [
                str(binary),
                "--web",
                "--http-recovery",
                "--simulate",
                "--web-root",
                str(web_dir),
                "--state-dir",
                str(state_dir),
                "--config",
                str(state_dir / "config.json"),
                "--data-dir",
                str(state_dir / "data"),
                "--listen",
                f"127.0.0.1:{web_port}",
                "--modbus-tcp-listen",
                f"127.0.0.1:{modbus_port}",
                "--no-modbus-rtu",
                "--privileged-socket",
                str(state_dir / "privileged.sock"),
            ],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        try:
            deadline = time.monotonic() + 8
            while time.monotonic() < deadline:
                if process.poll() is not None:
                    fail("server exited before the web endpoint was ready")
                try:
                    status, _, _ = request(web_port, "GET", "/healthz")
                    if status == 200:
                        break
                except OSError:
                    time.sleep(0.05)
            else:
                fail("server did not start")

            password = (state_dir / "initial-password").read_text(encoding="utf-8").strip()
            origin = f"http://127.0.0.1:{web_port}"
            status, body, headers = request(
                web_port,
                "POST",
                "/api/v1/session",
                json.dumps({"username": "admin", "password": password}).encode(),
                {"Content-Type": "application/json", "Origin": origin},
            )
            if status != 200:
                fail(f"login returned {status}: {body!r}")
            session = json.loads(body)
            cookie = headers["Set-Cookie"].split(";", 1)[0]
            auth_headers = {
                "Cookie": cookie,
                "X-CSRF-Token": str(session["csrf_token"]),
                "Content-Type": "application/json",
            }
            upload = json.dumps(
                {"current_password": password, "content": icd.decode("utf-8")},
                ensure_ascii=False,
                separators=(",", ":"),
            ).encode()
            status, body, _ = request(
                web_port, "PUT", "/api/v1/iec61850/icd", upload, auth_headers
            )
            if status != 200:
                fail(f"runtime ICD upload returned {status}: {body!r}")
            response = json.loads(body)
            if not response.get("runtime_reloaded"):
                fail(f"upload did not report a runtime reload: {response!r}")

            status, body, _ = request(
                web_port, "GET", "/api/v1/iec61850", headers={"Cookie": cookie}
            )
            if status != 200:
                fail(f"IEC status returned {status}: {body!r}")
            runtime = json.loads(body)
            if runtime.get("runtime_model") != "scl" or runtime.get("ied_name") != "UHFPD12PD":
                fail(f"uploaded model is not live: {runtime!r}")
            if not any(item.get("name") == "DSState" for item in runtime.get("datasets", [])):
                fail("live status does not expose DSState")
            if not any(item.get("name") == "RPState" for item in runtime.get("reports", [])):
                fail("live status does not expose RPState")

            probe_result = subprocess.run(
                [str(probe), "--probe", str(iec_port), "UHFPD12PDMON"],
                check=False,
                capture_output=True,
                text=True,
                timeout=5,
            )
            if probe_result.returncode != 0:
                fail(f"MMS probe failed: {probe_result.stdout}{probe_result.stderr}")

            status, body, _ = request(
                web_port, "GET", "/api/v1/config", headers={"Cookie": cookie}
            )
            if status != 200:
                fail(f"configuration lookup returned {status}: {body!r}")
            configuration = json.loads(body)
            version = int(configuration.pop("version"))
            if configuration.get("iec_ied_name") != "UHFPD12PD":
                fail(f"ICD upload did not synchronize the configured IED name: {configuration!r}")
            configuration["iec_ied_name"] = "RENAMED1"
            update_headers = dict(auth_headers)
            update_headers["If-Match"] = f'"{version}"'
            status, body, _ = request(
                web_port,
                "PUT",
                "/api/v1/config",
                json.dumps(configuration, ensure_ascii=False, separators=(",", ":")).encode(),
                update_headers,
            )
            if status != 200:
                fail(f"IED name configuration update returned {status}: {body!r}")
            if json.loads(body).get("restart_required") is not False:
                fail(f"IED name update unexpectedly requires restart: {body!r}")

            deadline = time.monotonic() + 12
            while time.monotonic() < deadline:
                status, body, _ = request(
                    web_port, "GET", "/api/v1/iec61850", headers={"Cookie": cookie}
                )
                if status == 200 and json.loads(body).get("ied_name") == "RENAMED1":
                    break
                time.sleep(0.1)
            else:
                fail("configured IED name was not applied to the live model")
            renamed_probe = subprocess.run(
                [str(probe), "--probe", str(iec_port), "RENAMED1MON"],
                check=False,
                capture_output=True,
                text=True,
                timeout=5,
            )
            if renamed_probe.returncode != 0:
                fail(f"renamed MMS model is not browsable: {renamed_probe.stdout}{renamed_probe.stderr}")
            print("IEC 61850 runtime ICD reload smoke: OK")
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
