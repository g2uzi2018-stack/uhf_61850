#!/usr/bin/env python3
import base64
import hashlib
import hmac
import http.client
import os
import pty
import select
import signal
import socket
import struct
import subprocess
import tempfile
import threading
import time
import tty
import json
import sys


def crc16(data):
    crc = 0xFFFF
    for value in data:
        crc ^= value
        for _ in range(8):
            crc = ((crc >> 1) ^ 0xA001) if crc & 1 else crc >> 1
    return crc


def frame(data):
    crc = crc16(data)
    return data + bytes((crc & 0xFF, crc >> 8))


class Device:
    def __init__(self, kind):
        self.kind = kind
        self.master, self.slave = pty.openpty()
        tty.setraw(self.master)
        tty.setraw(self.slave)
        self.path = os.ttyname(self.slave)
        self.stop = threading.Event()
        self.thread = threading.Thread(target=self.run, daemon=True)

    def start(self):
        self.thread.start()

    def close(self):
        self.stop.set()
        self.thread.join(timeout=1)
        os.close(self.master)
        os.close(self.slave)

    def registers(self, start, count):
        values = [0] * count
        if self.kind == "current":
            values = [(10 + index) & 0xFFFF for index in range(count)]
        elif self.kind == "temperature":
            values = [200 + 10 * index for index in range(count)]
        else:
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

    def run(self):
        pending = bytearray()
        while not self.stop.is_set():
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
                if request[0] != 1 or request[1] not in (3, 4):
                    continue
                start, count = struct.unpack(">HH", request[2:6])
                values = self.registers(start, count)
                payload = b"".join(struct.pack(">H", value) for value in values)
                response = bytes((1, request[1], len(payload))) + payload
                try:
                    os.write(self.master, frame(response))
                except OSError:
                    return


def wait_port(port, timeout=8):
    deadline = time.time() + timeout
    while time.time() < deadline:
        with socket.socket() as sock:
            sock.settimeout(0.2)
            try:
                sock.connect(("127.0.0.1", port))
                return
            except OSError:
                time.sleep(0.05)
    raise AssertionError("gateway port did not open")


def free_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def main():
    binary, web_root = sys.argv[1:3]
    devices = [Device("pd"), Device("current"), Device("temperature")]
    for device in devices:
        device.start()
    web_port = free_port()
    modbus_port = free_port()
    identity = "host-e2e-board"
    key = "host-e2e-key"
    code = base64.b32encode(hmac.new(key.encode(), identity.encode(), hashlib.sha256).digest()[:12]).decode().rstrip("=")
    with tempfile.TemporaryDirectory(prefix="uhf-v3-e2e-") as root:
        command = [binary, "--web", "--http-recovery", "--v3",
                   "--web-root", web_root, "--state-dir", root,
                   "--data-dir", os.path.join(root, "data"),
                   "--listen", f"127.0.0.1:{web_port}",
                   "--modbus-tcp-listen", f"127.0.0.1:{modbus_port}",
                   "--no-modbus-rtu", "--no-iec61850",
                   "--v3-pd-device", devices[0].path,
                   "--v3-current-device", devices[1].path,
                   "--v3-temperature-device", devices[2].path,
                   "--v3-device-id", identity, "--v3-activation-key", key,
                   "--v3-activation-code", code,
                   "--v3-current-multiplier", "1", "--v3-current-offset", "0",
                   "--v3-temperature-multiplier", "0.1", "--v3-temperature-offset", "0"]
        process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            wait_port(web_port)
            deadline = time.time() + 8
            body = None
            while time.time() < deadline:
                connection = http.client.HTTPConnection("127.0.0.1", web_port, timeout=1)
                connection.request("GET", "/api/v1/snapshot/latest")
                response = connection.getresponse()
                candidate = response.read()
                connection.close()
                if response.status == 200:
                    body = json.loads(candidate)
                    if body["sources"]["current"]["has_sample"] and body["sources"]["temperature"]["has_sample"]:
                        break
                time.sleep(0.1)
            assert body is not None and body["sources"]["current"]["online"]
            assert body["sources"]["temperature"]["online"]
            password = os.path.join(root, "initial-password")
            with open(password, "r", encoding="utf-8") as password_file:
                initial_password = password_file.read().strip()
            connection = http.client.HTTPConnection("127.0.0.1", web_port, timeout=2)
            login_body = json.dumps(
                {"username": "admin", "password": initial_password},
                separators=(",", ":"),
            )
            connection.request(
                "POST",
                "/api/v1/session",
                body=login_body,
                headers={
                    "Content-Type": "application/json",
                    "Origin": f"http://127.0.0.1:{web_port}",
                },
            )
            login_response = connection.getresponse()
            login_payload = json.loads(login_response.read())
            cookie = login_response.getheader("Set-Cookie", "").split(";", 1)[0]
            connection.close()
            assert login_response.status == 200 and cookie.startswith("uhf_session=")
            csrf_token = login_payload["csrf_token"]
            connection = http.client.HTTPConnection("127.0.0.1", web_port, timeout=2)
            connection.request("GET", "/api/v1/config", headers={"Cookie": cookie})
            config_response = connection.getresponse()
            configuration = json.loads(config_response.read())
            connection.close()
            assert config_response.status == 200
            assert configuration["v3_alarm_thresholds"] == [None] * 12
            config_version = configuration.pop("version")
            configuration["v3_alarm_thresholds"] = [
                None, None, None, 9, None, None, None, None, None, 19, None, None
            ]
            connection = http.client.HTTPConnection("127.0.0.1", web_port, timeout=2)
            connection.request(
                "PUT",
                "/api/v1/config",
                body=json.dumps(configuration, separators=(",", ":")),
                headers={
                    "Content-Type": "application/json",
                    "Cookie": cookie,
                    "X-CSRF-Token": csrf_token,
                    "If-Match": f'"{config_version}"',
                },
            )
            update_response = connection.getresponse()
            update_payload = json.loads(update_response.read())
            connection.close()
            assert update_response.status == 200
            assert update_payload["restart_required"] is False
            deadline = time.time() + 3
            while time.time() < deadline:
                connection = http.client.HTTPConnection("127.0.0.1", web_port, timeout=2)
                connection.request("GET", "/api/v1/snapshot/latest")
                alarm_response = connection.getresponse()
                alarm_payload = json.loads(alarm_response.read())
                connection.close()
                alarms = alarm_payload.get("alarms", [])
                if (len(alarms) == 12 and all(
                    alarms[index]["valid"] and alarms[index]["active"]
                    for index in (3, 9)
                )):
                    break
                time.sleep(0.05)
            else:
                raise AssertionError("v3 alarm thresholds were not hot reloaded")
            assert not alarms[1]["valid"] and not alarms[1]["active"]
            connection = http.client.HTTPConnection("127.0.0.1", web_port, timeout=2)
            connection.request("GET", "/api/v1/packets", headers={"Cookie": cookie})
            packet_response = connection.getresponse()
            packet_payload = json.loads(packet_response.read())
            connection.close()
            assert packet_response.status == 200 and packet_payload["memory_only"] is True
            assert len(packet_payload["entries"]) <= packet_payload["max_entries"] == 256
            assert packet_payload["retained_bytes"] <= packet_payload["max_bytes"] == 65536
            assert {entry["source"] for entry in packet_payload["entries"]} >= {
                "pd", "current", "temperature"
            }
            assert {entry["direction"] for entry in packet_payload["entries"]} == {"tx", "rx"}
            assert all(entry["size"] * 2 == len(entry["hex"])
                       for entry in packet_payload["entries"])
            assert not any("packet" in filename.lower()
                           for _, _, filenames in os.walk(os.path.join(root, "data"))
                           for filename in filenames)
            with socket.create_connection(("127.0.0.1", modbus_port), timeout=2) as sock:
                sock.sendall(struct.pack(">HHHBBHH", 1, 0, 6, 1, 3, 1, 2))
                response = bytearray()
                while len(response) < 13:
                    response.extend(sock.recv(64))
            assert response[7] == 3 and response[8] == 4
        finally:
            process.send_signal(signal.SIGTERM)
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=2)
    for device in devices:
        device.close()


if __name__ == "__main__":
    main()
