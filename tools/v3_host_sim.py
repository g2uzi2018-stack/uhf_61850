#!/usr/bin/env python3
"""Run the real v3 gateway against three host PTY device simulators."""

import argparse
import base64
import hashlib
import hmac
import os
from pathlib import Path
import pty
import secrets
import select
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
import tty
from typing import List, Optional


def crc16(data: bytes) -> int:
    crc = 0xFFFF
    for value in data:
        crc ^= value
        for _ in range(8):
            crc = ((crc >> 1) ^ 0xA001) if crc & 1 else crc >> 1
    return crc


def frame(data: bytes) -> bytes:
    checksum = crc16(data)
    return data + bytes((checksum & 0xFF, checksum >> 8))


class SimulatedDevice:
    def __init__(self, kind: str) -> None:
        self.kind = kind
        self.master, self.slave = pty.openpty()
        tty.setraw(self.master)
        tty.setraw(self.slave)
        self.path = os.ttyname(self.slave)
        self.stop_event = threading.Event()
        self.thread = threading.Thread(target=self.run, daemon=True)

    def start(self) -> None:
        self.thread.start()

    def close(self) -> None:
        self.stop_event.set()
        self.thread.join(timeout=1)
        for descriptor in (self.master, self.slave):
            try:
                os.close(descriptor)
            except OSError:
                pass

    def registers(self, start: int, count: int) -> List[int]:
        if self.kind == "current":
            return [10 + index for index in range(count)]
        if self.kind == "temperature":
            return [200 + 10 * index for index in range(count)]
        values = [0] * count
        offset = start - 10001
        for index in range(count):
            absolute = offset + index
            if absolute == 2:
                values[index] = 100
            elif absolute < 15:
                values[index] = absolute + 1
            elif absolute < 3615:
                values[index] = absolute & 0xFFFF
        return values

    def run(self) -> None:
        pending = bytearray()
        while not self.stop_event.is_set():
            ready, _, _ = select.select([self.master], [], [], 0.1)
            if not ready:
                continue
            try:
                pending.extend(os.read(self.master, 512))
            except OSError:
                return
            while len(pending) >= 8:
                request = bytes(pending[:8])
                del pending[:8]
                if (request[0] != 1 or request[1] not in (3, 4) or
                        crc16(request[:-2]) != int.from_bytes(request[-2:], "little")):
                    continue
                start, count = struct.unpack(">HH", request[2:6])
                values = self.registers(start, count)
                payload = b"".join(struct.pack(">H", value) for value in values)
                try:
                    os.write(self.master, frame(bytes((1, request[1], len(payload))) + payload))
                except OSError:
                    return


def protected_value(path: Path, value: str) -> None:
    path.write_text(value + "\n", encoding="utf-8")
    path.chmod(0o600)


def wait_for_port(process: subprocess.Popen, port: int) -> bool:
    deadline = time.monotonic() + 8
    while time.monotonic() < deadline:
        if process.poll() is not None:
            return False
        with socket.socket() as connection:
            connection.settimeout(0.2)
            try:
                connection.connect(("127.0.0.1", port))
                return True
            except OSError:
                time.sleep(0.05)
    return False


def wait_for_text_file(process: subprocess.Popen, path: Path) -> Optional[str]:
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        if process.poll() is not None:
            return None
        try:
            value = path.read_text(encoding="utf-8").strip()
            if value:
                return value
        except (FileNotFoundError, OSError):
            pass
        time.sleep(0.05)
    return None


def available_port(excluded: int = 0) -> int:
    while True:
        with socket.socket() as listener:
            listener.bind(("127.0.0.1", 0))
            port = int(listener.getsockname()[1])
        if port != excluded:
            return port


def port_is_available(port: int) -> bool:
    with socket.socket() as listener:
        try:
            listener.bind(("127.0.0.1", port))
            return True
        except OSError:
            return False


def parse_arguments() -> argparse.Namespace:
    repository = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, default=repository / "build/host/uhf-gatewayd")
    parser.add_argument("--web-root", type=Path, default=repository / "web")
    parser.add_argument("--web-port", type=int, default=0,
                        help="localhost HTTP port; default selects a free port")
    parser.add_argument("--modbus-port", type=int, default=0,
                        help="localhost Modbus/TCP port; default selects a free port")
    parser.add_argument("--run-seconds", type=float, default=0.0,
                        help="exit automatically after this duration; default runs until Ctrl-C")
    return parser.parse_args()


def request_stop(_signum: int, _frame: object) -> None:
    raise KeyboardInterrupt


def main() -> int:
    arguments = parse_arguments()
    if not arguments.binary.is_file() or not arguments.web_root.is_dir():
        raise SystemExit("build build/host/uhf-gatewayd first and provide a valid web root")
    if not (0 <= arguments.web_port <= 65535 and 0 <= arguments.modbus_port <= 65535):
        raise SystemExit("ports must be zero (automatic) or between 1 and 65535")
    if arguments.web_port == 0:
        arguments.web_port = available_port()
    if arguments.modbus_port == 0:
        arguments.modbus_port = available_port(arguments.web_port)
    if arguments.web_port == arguments.modbus_port:
        raise SystemExit("web and Modbus/TCP ports must differ")
    if not port_is_available(arguments.web_port):
        raise SystemExit(f"web port {arguments.web_port} is already in use")
    if not port_is_available(arguments.modbus_port):
        raise SystemExit(f"Modbus/TCP port {arguments.modbus_port} is already in use")

    devices: List[SimulatedDevice] = []
    process: Optional[subprocess.Popen] = None
    try:
        devices = [
            SimulatedDevice("pd"),
            SimulatedDevice("current"),
            SimulatedDevice("temperature"),
        ]
        for device in devices:
            device.start()
        with tempfile.TemporaryDirectory(prefix="uhf-v3-host-sim-") as temporary:
            state = Path(temporary)
            print(f"temporary state: {state}", flush=True)
            identity = "host-simulator-" + secrets.token_hex(8)
            key = secrets.token_hex(32)
            code = base64.b32encode(
                hmac.new(key.encode(), identity.encode(), hashlib.sha256).digest()[:12]
            ).decode().rstrip("=")
            identity_file = state / "device-id"
            key_file = state / "manufacturer-key"
            code_file = state / "activation-code"
            protected_value(identity_file, identity)
            protected_value(key_file, key)
            protected_value(code_file, code)
            command = [
                str(arguments.binary), "--web", "--http-recovery", "--v3",
                "--web-root", str(arguments.web_root),
                "--state-dir", str(state),
                "--data-dir", str(state / "data"),
                "--listen", f"127.0.0.1:{arguments.web_port}",
                "--modbus-tcp-listen", f"127.0.0.1:{arguments.modbus_port}",
                "--no-modbus-rtu", "--no-iec61850",
                "--v3-pd-device", devices[0].path,
                "--v3-current-device", devices[1].path,
                "--v3-temperature-device", devices[2].path,
                "--v3-current-serial", "115200/8N1",
                "--v3-temperature-serial", "115200/8N1",
                "--v3-current-multiplier", "1", "--v3-current-offset", "0",
                "--v3-temperature-multiplier", "0.1", "--v3-temperature-offset", "0",
                "--v3-device-id-file", str(identity_file),
                "--v3-activation-key-file", str(key_file),
                "--v3-activation-code-file", str(code_file),
            ]
            process = subprocess.Popen(command)
            if not wait_for_port(process, arguments.web_port):
                return process.poll() if process.poll() is not None else 1
            password = wait_for_text_file(process, state / "initial-password")
            if password is None:
                print("gateway did not create its temporary login in time", file=sys.stderr)
                return process.poll() if process.poll() is not None else 1
            print(f"v3 host simulator: http://127.0.0.1:{arguments.web_port}/", flush=True)
            print(f"temporary login: admin / {password}", flush=True)
            print("Press Ctrl-C to stop; all identity, key, activation, and data files are temporary.",
                  flush=True)
            if arguments.run_seconds > 0:
                deadline = time.monotonic() + arguments.run_seconds
                while time.monotonic() < deadline and process.poll() is None:
                    time.sleep(0.1)
                return 0 if process.poll() is None else int(process.returncode or 0)
            return process.wait()
    except KeyboardInterrupt:
        return 0
    finally:
        if process is not None and process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=2)
        for device in devices:
            device.close()


if __name__ == "__main__":
    signal.signal(signal.SIGTERM, request_stop)
    raise SystemExit(main())
