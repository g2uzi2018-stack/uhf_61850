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

from websocket_smoke import receive_frame, websocket_handshake


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
        self.request_lock = threading.Lock()
        self.request_log = []
        self.value_lock = threading.Lock()
        self.current_base = 10

    def start(self):
        self.thread.start()

    def close(self):
        if self.stop.is_set():
            return
        self.stop.set()
        self.thread.join(timeout=1)
        os.close(self.master)
        os.close(self.slave)

    def requests(self):
        with self.request_lock:
            return list(self.request_log)

    def set_current_base(self, value):
        with self.value_lock:
            self.current_base = value

    def registers(self, start, count):
        values = [0] * count
        if self.kind == "current":
            with self.value_lock:
                current_base = self.current_base
            values = [(current_base + index) & 0xFFFF for index in range(count)]
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
                with self.request_lock:
                    self.request_log.append((start, count, time.monotonic()))
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


def distinct_free_port(*excluded):
    while True:
        port = free_port()
        if port not in excluded:
            return port


def assert_port_closed(port):
    with socket.socket() as sock:
        sock.settimeout(0.2)
        assert sock.connect_ex(("127.0.0.1", port)) != 0, (
            f"activation-gated service unexpectedly listened on port {port}"
        )


def read_health(port, cookie):
    connection = http.client.HTTPConnection("127.0.0.1", port, timeout=2)
    connection.request("GET", "/api/v1/health", headers={"Cookie": cookie})
    response = connection.getresponse()
    payload = json.loads(response.read())
    connection.close()
    assert response.status == 200
    return payload


def wait_rtu_health(port, cookie, expected, timeout=3):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        payload = read_health(port, cookie)
        if payload["modbus_rtu"]["status"] == expected:
            return payload
        time.sleep(0.01)
    raise AssertionError(f"gateway RTU health did not become {expected}")


def receive_exact(file_descriptor, size, timeout=2):
    payload = bytearray()
    deadline = time.monotonic() + timeout
    while len(payload) < size:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise AssertionError("timed out waiting for v3 RTU response")
        ready, _, _ = select.select([file_descriptor], [], [], remaining)
        if not ready:
            raise AssertionError("timed out waiting for v3 RTU response")
        chunk = os.read(file_descriptor, size - len(payload))
        if not chunk:
            raise AssertionError("v3 RTU PTY closed before response")
        payload.extend(chunk)
    return bytes(payload)


def receive_socket_exact(sock, size):
    payload = bytearray()
    while len(payload) < size:
        chunk = sock.recv(size - len(payload))
        if not chunk:
            raise AssertionError("v3 Modbus TCP connection closed before response")
        payload.extend(chunk)
    return bytes(payload)


def main():
    binary, web_root = sys.argv[1:3]
    iec_probe = sys.argv[3] if len(sys.argv) > 3 else None
    partial_scale = subprocess.run(
        [binary, "--web", "--v3-current-multiplier", "1"],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    assert partial_scale.returncode == 2
    assert b"must be provided together" in partial_scale.stderr
    invalid_scale = subprocess.run(
        [
            binary, "--web",
            "--v3-temperature-multiplier", "0",
            "--v3-temperature-offset", "0",
        ],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    assert invalid_scale.returncode == 2
    assert b"multipliers must be positive" in invalid_scale.stderr
    invalid_serial = subprocess.run(
        [binary, "--web", "--v3-current-serial", "12000/8N1"],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    assert invalid_serial.returncode == 2
    assert b"invalid --v3-current-serial" in invalid_serial.stderr
    with tempfile.TemporaryDirectory(prefix="uhf-v3-web-only-gate-") as root:
        web_only_port = free_port()
        web_only_result = subprocess.run(
            [
                binary, "--web", "--http-recovery", "--v3",
                "--no-acquisition", "--web-root", web_root,
                "--state-dir", root,
                "--listen", f"127.0.0.1:{web_only_port}",
                "--no-modbus-rtu", "--no-iec61850",
            ],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=3,
            check=False,
        )
        assert web_only_result.returncode == 1
        assert b"v3 activation is required" in web_only_result.stderr
        assert_port_closed(web_only_port)
        assert not os.path.exists(os.path.join(root, "activation.state"))
    devices = [Device("pd"), Device("current"), Device("temperature")]
    for device in devices:
        device.start()
    rtu_master, rtu_slave = pty.openpty()
    tty.setraw(rtu_master)
    tty.setraw(rtu_slave)
    rtu_device = os.ttyname(rtu_slave)
    web_port = free_port()
    modbus_port = distinct_free_port(web_port)
    iec_port = distinct_free_port(web_port, modbus_port) if iec_probe else None
    identity = "host-e2e-board"
    key = "host-e2e-key"
    code = base64.b32encode(hmac.new(key.encode(), identity.encode(), hashlib.sha256).digest()[:12]).decode().rstrip("=")
    with tempfile.TemporaryDirectory(prefix="uhf-v3-e2e-") as root:
        identity_file = os.path.join(root, "device-id")
        key_file = os.path.join(root, "manufacturer-key")
        code_file = os.path.join(root, "activation-code")
        for path, value in (
            (identity_file, identity), (key_file, key), (code_file, code)
        ):
            with open(path, "w", encoding="utf-8") as output:
                output.write(value + "\n")
            os.chmod(path, 0o600)
        invalid_code_file = os.path.join(root, "invalid-activation-code")
        invalid_code = ("A" if code[0] != "A" else "B") + code[1:]
        with open(invalid_code_file, "w", encoding="utf-8") as output:
            output.write(invalid_code + "\n")
        os.chmod(invalid_code_file, 0o600)
        gated_state = os.path.join(root, "activation-gated")
        gated_web_port = distinct_free_port(web_port, modbus_port)
        gated_modbus_port = distinct_free_port(
            web_port, modbus_port, gated_web_port
        )
        gated_iec_port = distinct_free_port(
            web_port, modbus_port, gated_web_port, gated_modbus_port
        )
        gated_rtu_master, gated_rtu_slave = pty.openpty()
        tty.setraw(gated_rtu_master)
        tty.setraw(gated_rtu_slave)
        gated_rtu_device = os.ttyname(gated_rtu_slave)
        gated_request_counts = [len(device.requests()) for device in devices]
        gated_command = [
            binary, "--web", "--http-recovery", "--v3",
            "--web-root", web_root, "--state-dir", gated_state,
            "--data-dir", os.path.join(gated_state, "data"),
            "--listen", f"127.0.0.1:{gated_web_port}",
            "--modbus-tcp-listen", f"127.0.0.1:{gated_modbus_port}",
            "--modbus-rtu-device", gated_rtu_device,
            "--v3-pd-device", devices[0].path,
            "--v3-current-device", devices[1].path,
            "--v3-temperature-device", devices[2].path,
            "--v3-current-serial", "115200/8N1",
            "--v3-temperature-serial", "115200/8N1",
            "--v3-device-id-file", identity_file,
            "--v3-activation-key-file", key_file,
            "--v3-activation-code-file", invalid_code_file,
        ]
        if iec_probe:
            gated_command.extend(
                ["--iec61850-listen", f"127.0.0.1:{gated_iec_port}"]
            )
        else:
            gated_command.append("--no-iec61850")
        gated_result = subprocess.run(
            gated_command,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=3,
            check=False,
        )
        assert gated_result.returncode == 1
        assert b"v3 activation is required" in gated_result.stderr
        assert [len(device.requests()) for device in devices] == gated_request_counts
        assert not os.path.exists(os.path.join(gated_state, "activation.state"))
        assert_port_closed(gated_web_port)
        assert_port_closed(gated_modbus_port)
        if iec_probe:
            assert_port_closed(gated_iec_port)
        os.write(gated_rtu_master, frame(bytes((1, 3, 0, 1, 0, 2))))
        assert not select.select([gated_rtu_master], [], [], 0.1)[0]

        unconfigured_state = os.path.join(root, "unconfigured")
        unconfigured_web_port = distinct_free_port(web_port, modbus_port)
        unconfigured_modbus_port = distinct_free_port(
            web_port, modbus_port, unconfigured_web_port
        )
        late_current_device = os.path.join(root, "late-current-device")
        unconfigured_command = [
            binary, "--web", "--http-recovery", "--v3",
            "--web-root", web_root, "--state-dir", unconfigured_state,
            "--data-dir", os.path.join(unconfigured_state, "data"),
            "--listen", f"127.0.0.1:{unconfigured_web_port}",
            "--modbus-tcp-listen", f"127.0.0.1:{unconfigured_modbus_port}",
            "--no-modbus-rtu", "--no-iec61850",
            "--v3-pd-device", devices[0].path,
            "--v3-current-device", late_current_device,
            "--v3-temperature-device", "/definitely/missing-temperature-device",
            "--v3-current-serial", "115200/8N1",
            "--v3-device-id-file", identity_file,
            "--v3-activation-key-file", key_file,
            "--v3-activation-code-file", code_file,
        ]
        unconfigured_process = subprocess.Popen(
            unconfigured_command, stdout=subprocess.PIPE, stderr=subprocess.PIPE
        )
        try:
            wait_port(unconfigured_web_port)
            deadline = time.time() + 5
            unconfigured_snapshot = None
            while time.time() < deadline:
                connection = http.client.HTTPConnection(
                    "127.0.0.1", unconfigured_web_port, timeout=2
                )
                connection.request("GET", "/api/v1/snapshot/latest")
                unconfigured_response = connection.getresponse()
                candidate = json.loads(unconfigured_response.read())
                connection.close()
                assert unconfigured_response.status == 200
                unconfigured_snapshot = candidate
                if candidate["sources"]["pd"]["has_sample"]:
                    break
                time.sleep(0.05)
            assert unconfigured_snapshot is not None
            assert unconfigured_snapshot["sources"]["pd"]["has_sample"] is True
            assert unconfigured_snapshot["sources"]["pd"]["online"] is True
            assert unconfigured_response.status == 200
            assert unconfigured_snapshot["sources"]["current"]["has_sample"] is False
            assert unconfigured_snapshot["sources"]["current"]["last_attempt_ms"] is not None
            assert unconfigured_snapshot["sources"]["current"]["consecutive_failures"] >= 1
            assert unconfigured_snapshot["sources"]["temperature"]["has_sample"] is False

            assert not os.path.lexists(late_current_device)
            os.symlink(devices[1].path, late_current_device)
            recovery_deadline = time.monotonic() + 5
            while time.monotonic() < recovery_deadline:
                connection = http.client.HTTPConnection(
                    "127.0.0.1", unconfigured_web_port, timeout=2
                )
                connection.request("GET", "/api/v1/snapshot/latest")
                recovery_response = connection.getresponse()
                recovered_snapshot = json.loads(recovery_response.read())
                connection.close()
                assert recovery_response.status == 200
                current_source = recovered_snapshot["sources"]["current"]
                if current_source["has_sample"] and current_source["online"]:
                    break
                time.sleep(0.05)
            else:
                raise AssertionError("configured current source did not recover")
            assert current_source["consecutive_failures"] == 0
            assert current_source["last_success_ms"] is not None
            assert recovered_snapshot["sources"]["pd"]["online"] is True
            assert recovered_snapshot["sources"]["temperature"]["has_sample"] is False
            recovered_measurements = {
                entry["name"]: entry["value"]
                for entry in recovered_snapshot["measurements"]
            }
            assert recovered_measurements["Ia"]["valid"] is False
            assert recovered_measurements["Ia"]["value"] is None
        finally:
            unconfigured_process.send_signal(signal.SIGTERM)
            try:
                unconfigured_process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                unconfigured_process.kill()
                unconfigured_process.wait(timeout=2)

        mismatch_identity_file = os.path.join(root, "mismatched-device-id")
        with open(mismatch_identity_file, "w", encoding="utf-8") as output:
            output.write("different-host-e2e-board\n")
        os.chmod(mismatch_identity_file, 0o600)
        mismatch_request_counts = [len(device.requests()) for device in devices]
        mismatch_command = [
            binary, "--web", "--http-recovery", "--v3",
            "--web-root", web_root, "--state-dir", unconfigured_state,
            "--data-dir", os.path.join(unconfigured_state, "data"),
            "--listen", f"127.0.0.1:{gated_web_port}",
            "--modbus-tcp-listen", f"127.0.0.1:{gated_modbus_port}",
            "--modbus-rtu-device", gated_rtu_device,
            "--v3-pd-device", devices[0].path,
            "--v3-current-device", devices[1].path,
            "--v3-temperature-device", devices[2].path,
            "--v3-current-serial", "115200/8N1",
            "--v3-temperature-serial", "115200/8N1",
            "--v3-device-id-file", mismatch_identity_file,
            "--v3-activation-key-file", key_file,
        ]
        if iec_probe:
            mismatch_command.extend(
                ["--iec61850-listen", f"127.0.0.1:{gated_iec_port}"]
            )
        else:
            mismatch_command.append("--no-iec61850")
        mismatch_result = subprocess.run(
            mismatch_command,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=3,
            check=False,
        )
        assert mismatch_result.returncode == 1
        assert b"v3 activation is required" in mismatch_result.stderr
        assert [len(device.requests()) for device in devices] == mismatch_request_counts
        assert_port_closed(gated_web_port)
        assert_port_closed(gated_modbus_port)
        if iec_probe:
            assert_port_closed(gated_iec_port)
        assert not select.select([gated_rtu_master], [], [], 0.1)[0]
        os.close(gated_rtu_master)
        os.close(gated_rtu_slave)

        time.sleep(0.1)
        pd_request_offset = len(devices[0].requests())
        runtime_rtu_device = os.path.join(root, "modbus-rtu-device")
        runtime_current_device = os.path.join(root, "current-device")
        os.symlink(devices[1].path, runtime_current_device)
        command = [binary, "--web", "--http-recovery", "--v3",
                   "--web-root", web_root, "--state-dir", root,
                   "--data-dir", os.path.join(root, "data"),
                   "--listen", f"127.0.0.1:{web_port}",
                   "--modbus-tcp-listen", f"127.0.0.1:{modbus_port}",
                   "--modbus-rtu-device", runtime_rtu_device,
                   "--v3-pd-device", devices[0].path,
                   "--v3-current-device", runtime_current_device,
                   "--v3-temperature-device", devices[2].path,
                   "--v3-current-serial", "115200/8N1",
                   "--v3-temperature-serial", "115200/8N1",
                   "--v3-device-id-file", identity_file,
                   "--v3-activation-key-file", key_file,
                   "--v3-activation-code-file", code_file]
        if iec_probe:
            command.extend(["--iec61850-listen", f"127.0.0.1:{iec_port}"])
        else:
            command.append("--no-iec61850")
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
            measurements = {entry["name"]: entry["value"] for entry in body["measurements"]}
            assert measurements["Ia"]["valid"] is False
            assert measurements["TA"]["valid"] is False
            websocket = websocket_handshake(
                web_port, "", f"http://127.0.0.1:{web_port}"
            )
            try:
                websocket_payload = None
                for _ in range(4):
                    opcode, payload = receive_frame(websocket)
                    if opcode != 1:
                        continue
                    candidate = json.loads(payload)
                    if candidate.get("type") == "telemetry":
                        websocket_payload = candidate
                        break
                assert websocket_payload is not None
                websocket_snapshot = websocket_payload["snapshot"]
                assert websocket_snapshot["schema_version"] == 3
                assert len(websocket_snapshot["pd"]) == 3
                assert len(websocket_snapshot["measurements"]) == 35
                assert websocket_snapshot["sources"]["current"]["has_sample"] is True
                assert websocket_snapshot["sources"]["temperature"]["has_sample"] is True
            finally:
                websocket.close()
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
            health_before_rtu = read_health(web_port, cookie)
            assert health_before_rtu["modbus_rtu"]["status"] == "down"
            assert configuration["v3_alarm_thresholds"] == [None] * 12
            assert configuration["v3_current_encoding"] == "unconfigured"
            assert configuration["v3_current_multiplier"] is None
            assert configuration["v3_temperature_multiplier"] is None
            assert configuration["v3_pd_slave_id"] == 1
            assert configuration["v3_pd_interval_ms"] == 3000
            assert configuration["v3_current_interval_ms"] == 1000
            assert configuration["v3_temperature_interval_ms"] == 1000
            assert configuration["v3_response_timeout_ms"] == 150
            assert configuration["v3_retry_delay_ms"] == 20
            assert configuration["v3_late_frame_quarantine_ms"] == 20
            assert configuration["v3_max_retries"] == 3
            assert configuration["v3_pd_freshness_ms"] == 600000
            assert configuration["v3_current_freshness_ms"] == 5000
            assert configuration["v3_temperature_freshness_ms"] == 5000
            rtu_unit_id = configuration["rtu_unit_id"]
            history_entries = []
            history_deadline = time.monotonic() + 3
            while time.monotonic() < history_deadline:
                connection = http.client.HTTPConnection(
                    "127.0.0.1", web_port, timeout=2
                )
                connection.request(
                    "GET", "/api/v1/frames", headers={"Cookie": cookie}
                )
                frames_response = connection.getresponse()
                frames_payload = json.loads(frames_response.read())
                connection.close()
                assert frames_response.status == 200
                assert frames_payload["schema_version"] == 3
                history_entries = frames_payload["entries"]
                if history_entries:
                    break
                time.sleep(0.05)
            assert history_entries, "formal v3 history index remained empty"
            with os.scandir(os.path.join(root, "data", "v3")) as persisted_entries:
                assert any(
                    entry.is_file() and entry.name.endswith(".bin")
                    for entry in persisted_entries
                ), "formal v3 history directory has no binary record"
            connection = http.client.HTTPConnection(
                "127.0.0.1", web_port, timeout=2
            )
            connection.request(
                "GET", "/api/v1/frames/export.csv", headers={"Cookie": cookie}
            )
            frame_export_response = connection.getresponse()
            frame_export = frame_export_response.read().decode("utf-8")
            frame_export_type = frame_export_response.getheader("Content-Type", "")
            frame_export_disposition = frame_export_response.getheader(
                "Content-Disposition", ""
            )
            connection.close()
            assert frame_export_response.status == 200
            assert frame_export_type.startswith("text/csv")
            assert 'filename="latest-frame.csv"' in frame_export_disposition, repr(
                frame_export_disposition
            )
            history_rows = [row.split(",") for row in frame_export.strip().splitlines()]
            assert history_rows[0] == [
                "generation", "timestamp_ms", "name", "valid", "value", "quality"
            ]
            assert len(history_rows) == 36
            initial_ia = next(row for row in history_rows[1:] if row[2] == "Ia")
            assert initial_ia[3] == "0" and initial_ia[4] == ""
            connection = http.client.HTTPConnection("127.0.0.1", web_port, timeout=2)
            connection.request("GET", "/api/v1/events", headers={"Cookie": cookie})
            events_response = connection.getresponse()
            events_payload = json.loads(events_response.read())
            connection.close()
            assert events_response.status == 200
            assert events_payload == {
                "schema_version": 3,
                "supported": False,
                "reason": "v3 event policy is not configured",
                "entries": [],
            }
            connection = http.client.HTTPConnection("127.0.0.1", web_port, timeout=2)
            connection.request("GET", "/api/v1/events/export.csv", headers={"Cookie": cookie})
            event_export_response = connection.getresponse()
            event_export_response.read()
            connection.close()
            assert event_export_response.status == 404
            config_version = configuration.pop("version")
            configuration["v3_alarm_thresholds"] = [
                None, None, None, 9, None, None, None, None, None, 19, None, None
            ]
            configuration["v3_current_encoding"] = "unsigned16"
            configuration["v3_current_multiplier"] = 1
            configuration["v3_current_offset"] = 0
            configuration["v3_temperature_multiplier"] = 0.1
            configuration["v3_temperature_offset"] = 0
            configuration["v3_pd_interval_ms"] = 3000
            configuration["v3_current_interval_ms"] = 1200
            configuration["v3_temperature_interval_ms"] = 1300
            configuration["v3_response_timeout_ms"] = 170
            configuration["v3_retry_delay_ms"] = 25
            configuration["v3_late_frame_quarantine_ms"] = 30
            configuration["v3_max_retries"] = 2
            configuration["v3_pd_freshness_ms"] = 610000
            configuration["v3_current_freshness_ms"] = 6000
            configuration["v3_temperature_freshness_ms"] = 7000
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
                live_measurements = {
                    entry["name"]: entry["value"]
                    for entry in alarm_payload.get("measurements", [])
                }
                if (len(alarms) == 12 and all(
                    alarms[index]["valid"] and alarms[index]["active"]
                    for index in (3, 9)
                ) and live_measurements.get("Ia", {}).get("value") == 10.0
                    and live_measurements.get("TA", {}).get("value") == 20.0
                    and alarm_payload["sources"]["pd"]["freshness_limit_ms"] == 610000
                    and alarm_payload["sources"]["current"]["freshness_limit_ms"] == 6000
                    and alarm_payload["sources"]["temperature"]["freshness_limit_ms"] == 7000
                    and alarm_payload["sources"]["pd"]["acquisition"]["unit_id"] == 1
                    and alarm_payload["sources"]["pd"]["acquisition"]["poll_interval_ms"] == 3000
                    and alarm_payload["sources"]["current"]["acquisition"]["poll_interval_ms"] == 1200
                    and alarm_payload["sources"]["temperature"]["acquisition"]["poll_interval_ms"] == 1300
                    and alarm_payload["sources"]["current"]["acquisition"]["response_timeout_ms"] == 170
                    and alarm_payload["sources"]["current"]["acquisition"]["retry_delay_ms"] == 25
                    and alarm_payload["sources"]["current"]["acquisition"]["late_frame_quarantine_ms"] == 30
                    and alarm_payload["sources"]["current"]["acquisition"]["max_retries"] == 2):
                    break
                time.sleep(0.05)
            else:
                raise AssertionError("v3 alarm thresholds were not hot reloaded")
            assert not alarms[1]["valid"] and not alarms[1]["active"]
            if iec_probe:
                probe = subprocess.Popen(
                    [
                        iec_probe,
                        "--v3-probe",
                        str(iec_port),
                        "UHFPD1PDMON",
                        "30",
                    ],
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                    text=True,
                    bufsize=1,
                )
                try:
                    ready, _, _ = select.select([probe.stdout], [], [], 5)
                    assert ready, "IEC v3 probe did not become ready"
                    assert probe.stdout.readline().strip() == "IEC_V3_PROBE_READY"
                    devices[1].set_current_base(30)
                    probe_stdout, probe_stderr = probe.communicate(timeout=10)
                    assert probe.returncode == 0, probe_stderr
                    assert "IEC 61850 running v3 gateway probe: OK" in probe_stdout
                finally:
                    if probe.poll() is None:
                        probe.kill()
                        probe.wait(timeout=2)
            expected_current = 30.0 if iec_probe else 10.0
            assert not os.path.lexists(runtime_rtu_device)
            os.symlink(rtu_device, runtime_rtu_device)
            wait_rtu_health(web_port, cookie, "up")
            rtu_request_body = bytes((rtu_unit_id, 3, 0, 1, 0, 2))
            rtu_crc = crc16(rtu_request_body)
            os.write(
                rtu_master,
                rtu_request_body + bytes((rtu_crc & 0xFF, rtu_crc >> 8)),
            )
            rtu_response = receive_exact(rtu_master, 9)
            assert rtu_response[:3] == bytes((rtu_unit_id, 3, 4))
            assert abs(struct.unpack(">f", rtu_response[3:7])[0] - expected_current) < 0.01
            assert crc16(rtu_response[:-2]) == rtu_response[-2] | rtu_response[-1] << 8
            health_after_rtu = read_health(web_port, cookie)
            assert health_after_rtu["modbus_rtu"]["status"] == "up"

            alarm_request_body = bytes((rtu_unit_id, 2, 0, 6, 0, 1))
            alarm_crc = crc16(alarm_request_body)
            os.write(
                rtu_master,
                alarm_request_body + bytes((alarm_crc & 0xFF, alarm_crc >> 8)),
            )
            alarm_response = receive_exact(rtu_master, 6)
            assert alarm_response[:4] == bytes((rtu_unit_id, 2, 1, 1))
            assert crc16(alarm_response[:-2]) == alarm_response[-2] | alarm_response[-1] << 8

            # Leave half a request in the old stream, then replace the tty.
            # The server must report the loss and discard those bytes before
            # accepting a complete request from the replacement device.
            os.write(rtu_master, rtu_request_body[:4])
            time.sleep(0.1)
            os.close(rtu_master)
            os.close(rtu_slave)
            rtu_master = -1
            rtu_slave = -1
            os.unlink(runtime_rtu_device)
            wait_rtu_health(web_port, cookie, "down")

            rtu_master, rtu_slave = pty.openpty()
            tty.setraw(rtu_master)
            tty.setraw(rtu_slave)
            rtu_device = os.ttyname(rtu_slave)
            os.symlink(rtu_device, runtime_rtu_device)
            wait_rtu_health(web_port, cookie, "up")
            os.write(
                rtu_master,
                rtu_request_body + bytes((rtu_crc & 0xFF, rtu_crc >> 8)),
            )
            replacement_response = receive_exact(rtu_master, 9)
            assert replacement_response[:3] == bytes((rtu_unit_id, 3, 4))
            assert abs(
                struct.unpack(">f", replacement_response[3:7])[0] - expected_current
            ) < 0.01
            assert crc16(replacement_response[:-2]) == (
                replacement_response[-2] | replacement_response[-1] << 8
            )
            assert read_health(web_port, cookie)["modbus_rtu"]["status"] == "up"
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
            deadline = time.monotonic() + 5
            main_pd_requests = []
            while time.monotonic() < deadline:
                main_pd_requests = devices[0].requests()[pd_request_offset:]
                channel_one_starts = [
                    timestamp for start, _, timestamp in main_pd_requests
                    if start == 10001
                ]
                if len(channel_one_starts) >= 2:
                    break
                time.sleep(0.05)
            else:
                raise AssertionError("second PD channel round did not start")
            first_segment = next(
                timestamp for start, _, timestamp in main_pd_requests
                if start == 10016
            )
            assert first_segment - channel_one_starts[0] < 1.0
            assert channel_one_starts[1] - channel_one_starts[0] >= 2.99
            with socket.create_connection(("127.0.0.1", modbus_port), timeout=2) as sock:
                sock.sendall(
                    struct.pack(">HHHBBHH", 0x5630, 0, 6, rtu_unit_id, 3, 1, 2)
                )
                response = receive_socket_exact(sock, 13)
            assert struct.unpack(">HHH", response[:6]) == (0x5630, 0, 7)
            assert response[6:9] == bytes((rtu_unit_id, 3, 4))
            assert abs(struct.unpack(">f", response[9:13])[0] - expected_current) < 0.01

            devices[1].close()
            replacement_current = Device("current")
            replacement_current.set_current_base(42)
            replacement_current.start()
            devices[1] = replacement_current
            os.unlink(runtime_current_device)

            failure_deadline = time.monotonic() + 8
            while time.monotonic() < failure_deadline:
                connection = http.client.HTTPConnection(
                    "127.0.0.1", web_port, timeout=2
                )
                connection.request("GET", "/api/v1/snapshot/latest")
                failure_response = connection.getresponse()
                failed_snapshot = json.loads(failure_response.read())
                connection.close()
                assert failure_response.status == 200
                current_source = failed_snapshot["sources"]["current"]
                if current_source["communication_alarm"]:
                    break
                time.sleep(0.05)
            else:
                raise AssertionError("current disconnect did not raise communication alarm")
            assert current_source["online"] is False
            assert current_source["consecutive_failures"] >= 3
            assert failed_snapshot["sources"]["pd"]["online"] is True
            assert failed_snapshot["sources"]["temperature"]["online"] is True
            failed_measurements = {
                entry["name"]: entry["value"]
                for entry in failed_snapshot["measurements"]
            }
            assert failed_measurements["Ia"]["valid"] is False
            assert failed_measurements["Ia"]["value"] is None
            failed_health = read_health(web_port, cookie)
            assert failed_health["acquisition"]["status"] == "down"

            with socket.create_connection(("127.0.0.1", modbus_port), timeout=2) as sock:
                sock.sendall(
                    struct.pack(">HHHBBHH", 0x5631, 0, 6, rtu_unit_id, 2, 1, 1)
                )
                fault_response = receive_socket_exact(sock, 10)
            assert struct.unpack(">HHH", fault_response[:6]) == (0x5631, 0, 4)
            assert fault_response[6:10] == bytes((rtu_unit_id, 2, 1, 1))

            os.symlink(replacement_current.path, runtime_current_device)
            recovery_deadline = time.monotonic() + 5
            while time.monotonic() < recovery_deadline:
                connection = http.client.HTTPConnection(
                    "127.0.0.1", web_port, timeout=2
                )
                connection.request("GET", "/api/v1/snapshot/latest")
                recovery_response = connection.getresponse()
                recovered_snapshot = json.loads(recovery_response.read())
                connection.close()
                assert recovery_response.status == 200
                current_source = recovered_snapshot["sources"]["current"]
                recovered_measurements = {
                    entry["name"]: entry["value"]
                    for entry in recovered_snapshot["measurements"]
                }
                if (current_source["online"] and
                        recovered_measurements["Ia"]["value"] == 42.0):
                    break
                time.sleep(0.05)
            else:
                raise AssertionError("replacement current device did not recover")
            assert current_source["communication_alarm"] is False
            assert current_source["consecutive_failures"] == 0
            assert recovered_measurements["Ia"]["valid"] is True
            recovered_health = read_health(web_port, cookie)
            assert recovered_health["acquisition"]["status"] == "up"

            with socket.create_connection(("127.0.0.1", modbus_port), timeout=2) as sock:
                sock.sendall(
                    struct.pack(">HHHBBHH", 0x5632, 0, 6, rtu_unit_id, 2, 1, 1)
                )
                recovered_fault_response = receive_socket_exact(sock, 10)
                sock.sendall(
                    struct.pack(">HHHBBHH", 0x5633, 0, 6, rtu_unit_id, 3, 1, 2)
                )
                recovered_value_response = receive_socket_exact(sock, 13)
            assert struct.unpack(">HHH", recovered_fault_response[:6]) == (
                0x5632, 0, 4
            )
            assert recovered_fault_response[6:10] == bytes((rtu_unit_id, 2, 1, 0))
            assert struct.unpack(">HHH", recovered_value_response[:6]) == (0x5633, 0, 7)
            assert recovered_value_response[6:9] == bytes((rtu_unit_id, 3, 4))
            assert abs(
                struct.unpack(">f", recovered_value_response[9:13])[0] - 42.0
            ) < 0.01
        finally:
            process.send_signal(signal.SIGTERM)
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=2)
    for device in devices:
        device.close()
    if rtu_master >= 0:
        os.close(rtu_master)
    if rtu_slave >= 0:
        os.close(rtu_slave)


if __name__ == "__main__":
    main()
