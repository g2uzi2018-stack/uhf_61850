#!/usr/bin/env python3
"""Verify the interactive v3 host simulator starts the formal gateway path."""

import http.client
import json
import os
from pathlib import Path
import select
import socket
import subprocess
import sys
import time


def free_port() -> int:
    with socket.socket() as listener:
        listener.bind(("127.0.0.1", 0))
        return int(listener.getsockname()[1])


def distinct_free_port(*excluded: int) -> int:
    while True:
        port = free_port()
        if port not in excluded:
            return port


def fail(message: str, process: subprocess.Popen) -> None:
    process.terminate()
    try:
        stdout, stderr = process.communicate(timeout=5)
    except subprocess.TimeoutExpired:
        process.kill()
        stdout, stderr = process.communicate(timeout=2)
    raise SystemExit(
        f"v3 host simulator smoke failed: {message}\n"
        f"stdout={stdout.decode(errors='replace')}\n"
        f"stderr={stderr.decode(errors='replace')}"
    )


def main() -> int:
    if len(sys.argv) != 4:
        raise SystemExit("expected simulator, gateway binary, and web root")
    simulator, binary, web_root = map(Path, sys.argv[1:])
    web_port = free_port()
    modbus_port = distinct_free_port(web_port)
    process = subprocess.Popen(
        [
            sys.executable, str(simulator), "--binary", str(binary),
            "--web-root", str(web_root), "--web-port", str(web_port),
            "--modbus-port", str(modbus_port), "--run-seconds", "8",
        ],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    deadline = time.monotonic() + 7
    snapshot = None
    while time.monotonic() < deadline and process.poll() is None:
        try:
            connection = http.client.HTTPConnection("127.0.0.1", web_port, timeout=1)
            connection.request("GET", "/api/v1/snapshot/latest")
            response = connection.getresponse()
            body = response.read()
            connection.close()
            if response.status == 200:
                candidate = json.loads(body)
                sources = candidate.get("sources", {})
                if (sources.get("current", {}).get("has_sample") and
                        sources.get("temperature", {}).get("has_sample")):
                    snapshot = candidate
                    break
        except (OSError, json.JSONDecodeError):
            pass
        time.sleep(0.1)
    if snapshot is None:
        fail("formal v3 snapshot did not become available", process)
    if snapshot.get("schema_version") != 3 or len(snapshot.get("pd", [])) != 3:
        fail("simulator did not expose the schema 3 three-channel snapshot", process)
    measurements = {entry["name"]: entry["value"] for entry in snapshot["measurements"]}
    if measurements["Ia"].get("value") != 10.0 or measurements["TA"].get("value") != 20.0:
        fail("simulator engineering conversions did not reach the formal snapshot", process)
    captured_stdout = bytearray()
    ready_deadline = time.monotonic() + 5
    while b"v3 host simulator:" not in captured_stdout and time.monotonic() < ready_deadline:
        if process.poll() is not None:
            break
        ready, _, _ = select.select([process.stdout], [], [], 0.1)
        if ready:
            captured_stdout.extend(os.read(process.stdout.fileno(), 4096))
    if b"v3 host simulator:" not in captured_stdout:
        fail("simulator did not report readiness", process)
    process.terminate()
    stdout, stderr = process.communicate(timeout=5)
    stdout = bytes(captured_stdout) + stdout
    if process.returncode != 0 or b"v3 host simulator:" not in stdout:
        raise SystemExit(
            "v3 host simulator smoke failed during clean shutdown\n"
            f"stdout={stdout.decode(errors='replace')}\n"
            f"stderr={stderr.decode(errors='replace')}"
        )
    if b"temporary login: admin / " not in stdout:
        raise SystemExit("v3 host simulator did not expose its temporary login")
    state_prefix = "temporary state: "
    state_lines = [
        line[len(state_prefix):]
        for line in stdout.decode(errors="replace").splitlines()
        if line.startswith(state_prefix)
    ]
    if len(state_lines) != 1 or Path(state_lines[0]).exists():
        raise SystemExit("v3 host simulator did not remove its temporary state")
    print("v3 host simulator smoke: OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
