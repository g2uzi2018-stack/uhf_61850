#!/usr/bin/env python3
"""Exercise the privileged network service without changing host networking."""

import json
import os
from pathlib import Path
import socket
import struct
import subprocess
import tempfile
import time


def fail(message: str) -> None:
    raise SystemExit(f"privileged service smoke failed: {message}")


def exchange(path: Path, message: str) -> tuple[bool, str, str]:
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as sock:
        sock.settimeout(2)
        sock.connect(str(path))
        payload = message.encode("utf-8")
        sock.sendall(struct.pack(">I", len(payload)) + payload)
        header = sock.recv(4)
        if len(header) != 4:
            fail("short response header")
        length = struct.unpack(">I", header)[0]
        if length > 65536:
            fail("oversized response")
        response = bytearray()
        while len(response) < length:
            response.extend(sock.recv(length - len(response)))
        lines = bytes(response).decode("utf-8").split("\n", 2)
        if len(lines) != 3:
            fail(f"malformed response: {response!r}")
        return lines[0] == "OK", lines[1], lines[2]


def main() -> int:
    root = Path(__file__).resolve().parents[1]
    binary = root / "build/host/uhf-privilegedd"
    if not binary.is_file():
        fail(f"missing helper binary: {binary}")
    with tempfile.TemporaryDirectory(prefix="uhf-privileged-smoke-") as text:
        directory = Path(text)
        socket_path = directory / "run" / "privileged.sock"
        network_path = directory / "etc" / "network.json"
        transaction_path = directory / "var" / "transaction.json"
        process = subprocess.Popen(
            [
                str(binary),
                "--socket",
                str(socket_path),
                "--network-config",
                str(network_path),
                "--transaction",
                str(transaction_path),
                "--allowed-uid",
                str(os.getuid()),
                "--skip-network-runtime",
            ],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        try:
            deadline = time.monotonic() + 3
            while time.monotonic() < deadline and not socket_path.exists():
                if process.poll() is not None:
                    fail(f"helper exited: {process.stderr.read() if process.stderr else ''}")
                time.sleep(0.02)
            if not socket_path.exists():
                fail("helper socket was not created")

            ok, code, body = exchange(socket_path, "network.status")
            if not ok or code != "ok":
                fail(f"status failed: {ok}, {code}, {body}")
            status = json.loads(body)
            if status["config"]["eth0"]["address"] != "192.168.3.230":
                fail(f"unexpected default network config: {status!r}")

            candidate = {
                "eth0_mode": "static",
                "eth0_address": "192.168.3.230",
                "eth0_prefix": 24,
                "eth0_gateway": "192.168.3.1",
                "eth0_dns1": "",
                "eth0_dns2": "",
                "eth0_hostname": "",
                "eth0_dhcp_timeout_seconds": 15,
                "eth1_mode": "static",
                "eth1_address": "192.168.0.230",
                "eth1_prefix": 24,
                "eth1_gateway": "",
                "eth1_dns1": "",
                "eth1_dns2": "",
                "eth1_hostname": "",
                "eth1_dhcp_timeout_seconds": 15,
            }
            ok, code, _ = exchange(
                socket_path,
                "network.stage\n" + json.dumps(candidate, separators=(",", ":")),
            )
            if not ok or code != "ok":
                fail(f"no-op stage failed: {ok}, {code}")
            ok, code, _ = exchange(socket_path, "network.confirm")
            if not ok or code != "ok":
                fail(f"no-op confirm failed: {ok}, {code}")
            ok, code, _ = exchange(socket_path, "unknown.operation")
            if ok or code != "unknown_operation":
                fail(f"unknown operation was accepted: {ok}, {code}")
            print("privileged service smoke: OK")
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
