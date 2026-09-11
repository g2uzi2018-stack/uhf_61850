#!/usr/bin/env python3
"""Exercise the development HTTP server and its authentication boundary."""

from http.client import HTTPConnection
import json
from pathlib import Path
import socket
import stat
import subprocess
import sys
import tempfile
import time


def fail(message: str) -> None:
    raise SystemExit(f"web server smoke failed: {message}")


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
    connection = HTTPConnection("127.0.0.1", port, timeout=2)
    try:
        connection.request(method, path, body=body, headers=headers or {})
        response = connection.getresponse()
        return response.status, response.read(), dict(response.getheaders())
    finally:
        connection.close()


def json_body(payload: dict[str, str]) -> bytes:
    return json.dumps(payload, ensure_ascii=False, separators=(",", ":")).encode("utf-8")


def assert_status(actual: int, expected: int, context: str) -> None:
    if actual != expected:
        fail(f"{context}: expected {expected}, got {actual}")


def assert_json(body: bytes, key: str, expected: object, context: str) -> dict[str, object]:
    try:
        payload = json.loads(body)
    except json.JSONDecodeError as error:
        fail(f"{context}: invalid JSON: {error}")
    if payload.get(key) != expected:
        fail(f"{context}: expected {key}={expected!r}, got {payload!r}")
    return payload


def main() -> int:
    if len(sys.argv) != 3:
        fail("expected the server binary and web directory")

    binary = Path(sys.argv[1])
    web_dir = Path(sys.argv[2])
    port = free_port()
    with tempfile.TemporaryDirectory(prefix="uhf-web-smoke-") as state_directory_text:
        state_directory = Path(state_directory_text)
        privileged_process = None
        process = subprocess.Popen(
            [
                str(binary),
                "--web",
                "--http-recovery",
                "--web-root",
                str(web_dir),
                "--state-dir",
                str(state_directory),
                "--no-acquisition",
                "--listen",
                f"127.0.0.1:{port}",
                "--privileged-socket",
                str(state_directory / "privileged.sock"),
            ],
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
                    health_status, health_body, _ = request(port, "GET", "/healthz")
                    break
                except OSError:
                    time.sleep(0.05)

            assert_status(health_status or 0, 200, "health")
            if b'"web_auth":"ready"' not in health_body:
                fail(f"authentication is not ready: {health_body!r}")
            for component in (b'"storage":"', b'"modbus_tcp":"', b'"modbus_rtu":"'):
                if component not in health_body:
                    fail(f"health response is missing component status: {health_body!r}")

            slow = socket.create_connection(("127.0.0.1", port), timeout=2)
            slow.sendall(f"GET /healthz HTTP/1.1\r\nHost: 127.0.0.1:{port}\r\n".encode())
            try:
                concurrent_status, _, _ = request(port, "GET", "/healthz")
                assert_status(concurrent_status, 200, "health while another client is slow")
            finally:
                slow.close()

            slow_drip = socket.create_connection(("127.0.0.1", port), timeout=2)
            try:
                slow_drip.sendall(b"G")
                time.sleep(4.0)
                slow_drip.sendall(b"E")
                time.sleep(1.5)
                slow_drip.settimeout(1)
                try:
                    slow_drip.recv(128)
                except socket.timeout:
                    fail("slow drip request exceeded the absolute client deadline")
            finally:
                slow_drip.close()

            if stat.S_IMODE(state_directory.stat().st_mode) != 0o700:
                fail("authentication state directory is not mode 0700")
            bootstrap_path = state_directory / "initial-password"
            auth_path = state_directory / "auth.json"
            if not bootstrap_path.is_file() or not auth_path.is_file():
                fail("first-run authentication files were not created")
            if stat.S_IMODE(bootstrap_path.stat().st_mode) != 0o600:
                fail("initial password file is not mode 0600")
            if stat.S_IMODE(auth_path.stat().st_mode) != 0o600:
                fail("auth.json is not mode 0600")
            initial_password = bootstrap_path.read_text(encoding="utf-8").strip()
            if initial_password != "admin":
                fail("initial password does not use the configured default")

            root_status, root_body, root_headers = request(port, "GET", "/")
            assert_status(root_status, 200, "unauthenticated root")
            if root_headers.get("Location") is not None or "实时总览".encode("utf-8") not in root_body:
                fail("unauthenticated root did not serve the real-time overview")
            public_health_status, _, _ = request(port, "GET", "/api/v1/health")
            assert_status(public_health_status, 200, "public health")
            public_snapshot_status, _, _ = request(port, "GET", "/api/v1/snapshot/latest")
            assert_status(public_snapshot_status, 503, "public snapshot without acquisition")
            for protected_path in ("/api/v1/config", "/api/v1/network", "/api/v1/time", "/api/v1/iec61850"):
                protected_status, _, _ = request(port, "GET", protected_path)
                assert_status(protected_status, 401, f"unauthenticated {protected_path}")

            login_status, login_body, _ = request(port, "GET", "/login")
            assert_status(login_status, 200, "login page")
            if "登录控制台".encode("utf-8") not in login_body:
                fail("login page was not served")

            asset_status, _, _ = request(port, "GET", "/styles.css")
            assert_status(asset_status, 200, "CSS asset")
            traversal_status, _, _ = request(port, "GET", "/%2e%2e/CMakeLists.txt")
            if traversal_status == 200:
                fail("path traversal was served")
            method_status, _, _ = request(port, "POST", "/")
            assert_status(method_status, 405, "static method guard")

            origin = f"http://127.0.0.1:{port}"
            login_headers = {
                "Content-Type": "application/json",
                "Origin": origin,
            }
            missing_origin_status, _, _ = request(
                port,
                "POST",
                "/api/v1/session",
                json_body({"username": "admin", "password": initial_password}),
                {"Content-Type": "application/json"},
            )
            assert_status(missing_origin_status, 403, "missing login origin")

            login_status, login_body, login_headers_response = request(
                port,
                "POST",
                "/api/v1/session",
                json_body({"username": "admin", "password": initial_password}),
                login_headers,
            )
            assert_status(login_status, 200, "initial login")
            session = assert_json(login_body, "authenticated", True, "initial login")
            if session.get("must_change") is not False or not isinstance(session.get("csrf_token"), str):
                fail(f"initial login did not expose optional password change state: {session!r}")
            csrf_token = str(session["csrf_token"])
            set_cookie = login_headers_response.get("Set-Cookie", "")
            if not set_cookie.startswith("uhf_session=") or "HttpOnly" not in set_cookie or "SameSite=Strict" not in set_cookie:
                fail(f"session cookie is missing required attributes: {set_cookie!r}")
            cookie = set_cookie.split(";", 1)[0]

            session_status, session_body, _ = request(
                port, "GET", "/api/v1/session", headers={"Cookie": cookie}
            )
            assert_status(session_status, 200, "session lookup")
            assert_json(session_body, "must_change", False, "session lookup")
            page_status, page_body, _ = request(port, "GET", "/", headers={"Cookie": cookie})
            assert_status(page_status, 200, "authenticated root")
            if "实时总览".encode("utf-8") not in page_body:
                fail("authenticated root page was not served")
            user_page_status, user_page_body, _ = request(
                port, "GET", "/index.html", headers={"Cookie": cookie}
            )
            assert_status(user_page_status, 200, "authenticated user management")
            if "用户管理".encode("utf-8") not in user_page_body:
                fail("authenticated user-management page was not served")
            overview_status, overview_body, _ = request(
                port, "GET", "/overview.html", headers={"Cookie": cookie}
            )
            assert_status(overview_status, 200, "authenticated overview")
            if "实时总览".encode("utf-8") not in overview_body:
                fail("authenticated overview page was not served")
            overview_api_status, overview_api_body, _ = request(
                port, "GET", "/api/v1/overview"
            )
            assert_status(overview_api_status, 200, "public overview settings")
            overview_payload = json.loads(overview_api_body)
            if overview_payload.get("overview_title") != "局部放电在线监测系统" or overview_payload.get("phase_start_degree") != 0:
                fail(f"unexpected public overview settings: {overview_payload!r}")
            settings_status, settings_body, _ = request(
                port, "GET", "/settings.html", headers={"Cookie": cookie}
            )
            assert_status(settings_status, 200, "authenticated settings")
            if "采集与转发".encode("utf-8") not in settings_body:
                fail("authenticated settings page was not served")
            if b'name="iec_ied_name"' in settings_body:
                fail("IED name was not moved out of acquisition settings")
            for page, marker in (('/logs.html', '日志与健康'), ('/storage.html', '存储与日志'), ('/network.html', '网络设置'), ('/maintenance.html', '系统维护')):
                page_status, page_body, _ = request(port, "GET", page, headers={"Cookie": cookie})
                assert_status(page_status, 200, f"authenticated {page}")
                if marker.encode("utf-8") not in page_body:
                    fail(f"authenticated page was not served: {page}")
            network_page_status, network_page_body, _ = request(
                port, "GET", "/network.html", headers={"Cookie": cookie}
            )
            assert_status(network_page_status, 200, "network time settings page")
            for marker in ("SNTP 服务器", "手动设备时间", "IED 名称"):
                if marker.encode("utf-8") not in network_page_body:
                    fail(f"network page is missing {marker}")
            iec_page_status, iec_page_body, _ = request(port, "GET", "/iec61850.html", headers={"Cookie": cookie})
            assert_status(iec_page_status, 200, "authenticated IEC page")
            if "IEC 61850".encode("utf-8") not in iec_page_body:
                fail("authenticated IEC page was not served")
            privileged_process = subprocess.Popen(
                [
                    str(binary.with_name("uhf-privilegedd")),
                    "--socket",
                    str(state_directory / "privileged.sock"),
                    "--network-config",
                    str(state_directory / "network.json"),
                    "--transaction",
                    str(state_directory / "transaction.json"),
                    "--skip-network-runtime",
                ],
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
            )
            socket_deadline = time.monotonic() + 2
            while time.monotonic() < socket_deadline and not (state_directory / "privileged.sock").exists():
                if privileged_process.poll() is not None:
                    stderr = privileged_process.stderr.read() if privileged_process.stderr else ""
                    fail(f"privileged service exited early: {stderr.strip()}")
                time.sleep(0.02)
            if not (state_directory / "privileged.sock").exists():
                fail("privileged service did not create its socket")
            network_status, _, _ = request(port, "GET", "/api/v1/network", headers={"Cookie": cookie})
            assert_status(network_status, 200, "network helper status")
            time_status, time_body, _ = request(port, "GET", "/api/v1/time", headers={"Cookie": cookie})
            assert_status(time_status, 200, "time settings lookup")
            time_payload = json.loads(time_body)
            if time_payload.get("sntp_server") != "pool.ntp.org":
                fail(f"unexpected time settings: {time_payload!r}")
            invalid_time_status, _, _ = request(
                port,
                "POST",
                "/api/v1/time",
                json_body({"action": "set", "current_password": initial_password, "local_time": "2026-02-30T12:34"}),
                {"Cookie": cookie, "X-CSRF-Token": csrf_token},
            )
            assert_status(invalid_time_status, 400, "invalid manual time")
            confirm_without_password_status, _, _ = request(
                port,
                "POST",
                "/api/v1/network/confirm",
                json_body({}),
                {"Cookie": cookie, "X-CSRF-Token": csrf_token},
            )
            assert_status(confirm_without_password_status, 401, "network confirm reauthentication")
            confirm_wrong_password_status, _, _ = request(
                port,
                "POST",
                "/api/v1/network/confirm",
                json_body({"current_password": "wrong-password"}),
                {"Cookie": cookie, "X-CSRF-Token": csrf_token},
            )
            assert_status(confirm_wrong_password_status, 401, "network confirm password")
            confirm_status, _, _ = request(
                port,
                "POST",
                "/api/v1/network/confirm",
                json_body({"current_password": initial_password}),
                {"Cookie": cookie, "X-CSRF-Token": csrf_token},
            )
            assert_status(confirm_status, 409, "network confirm without transaction")
            network_candidate = {
                "eth0_mode": "static",
                "eth0_address": "192.168.3.230",
                "eth0_netmask": "255.255.255.0",
                "eth0_gateway": "192.168.3.1",
                "eth0_dns1": "",
                "eth0_dns2": "",
                "eth0_hostname": "",
                "eth0_dhcp_timeout_seconds": 15,
                "eth1_mode": "static",
                "eth1_address": "192.168.0.230",
                "eth1_netmask": "255.255.255.0",
                "eth1_gateway": "",
                "eth1_dns1": "",
                "eth1_dns2": "",
                "eth1_hostname": "",
                "eth1_dhcp_timeout_seconds": 15,
            }
            stage_status, _, _ = request(
                port,
                "POST",
                "/api/v1/network/stage",
                json_body({"current_password": initial_password, "candidate": network_candidate}),
                {"Cookie": cookie, "X-CSRF-Token": csrf_token},
            )
            assert_status(stage_status, 200, "network stage before IED rename")
            confirmed_status, _, _ = request(
                port,
                "POST",
                "/api/v1/network/confirm",
                json_body({"current_password": initial_password}),
                {"Cookie": cookie, "X-CSRF-Token": csrf_token},
            )
            assert_status(confirmed_status, 200, "network confirm before IED rename")
            post_network_config_status, post_network_config_body, _ = request(
                port, "GET", "/api/v1/config", headers={"Cookie": cookie}
            )
            assert_status(post_network_config_status, 200, "config after network confirm")
            if json.loads(post_network_config_body).get("iec_ied_name") != "UHFPD1":
                fail("network confirmation changed the configured IED name")
            iec_status, iec_body, _ = request(port, "GET", "/api/v1/iec61850", headers={"Cookie": cookie})
            assert_status(iec_status, 200, "IEC status lookup")
            iec_payload = json.loads(iec_body)
            model_references = {
                item.get("reference") for item in iec_payload.get("model", [])
            }
            if (
                iec_payload.get("ied_name") != "UHFPD1"
                or len(model_references) != 8
                or "PDMON/GGIO1.Ind1.stVal" not in model_references
            ):
                fail(f"IEC status model is incomplete: {iec_payload!r}")
            counters = iec_payload.get("counters", {})
            if iec_payload.get("active_connections") != 0:
                fail(f"IEC active connections are not initialized: {iec_payload!r}")
            if (
                counters.get("connection_rejections") != 0
                or counters.get("malformed_pdu_rejections") != 0
                or counters.get("oversized_pdu_rejections") != 0
                or counters.get("request_element_rejections") != 0
                or counters.get("ber_depth_rejections") != 0
                or counters.get("max_outstanding_rejections") != 0
                or counters.get("report_buffer_overflows") != 0
            ):
                fail(f"IEC resource counters are not initialized: {iec_payload!r}")
            ied_config = json.loads(post_network_config_body)
            ied_config_version = int(ied_config.pop("version"))
            ied_config["iec_ied_name"] = "RENAMED1"
            ied_update_status, ied_update_body, _ = request(
                port,
                "PUT",
                "/api/v1/config",
                json_body(ied_config),
                {
                    "Content-Type": "application/json",
                    "Cookie": cookie,
                    "X-CSRF-Token": csrf_token,
                    "If-Match": f'"{ied_config_version}"',
                },
            )
            assert_status(ied_update_status, 200, "IED rename after network confirm")
            assert_json(ied_update_body, "version", ied_config_version + 1, "IED rename after network confirm")
            renamed_iec_status, renamed_iec_body, _ = request(
                port, "GET", "/api/v1/iec61850", headers={"Cookie": cookie}
            )
            assert_status(renamed_iec_status, 200, "renamed IEC status lookup")
            if json.loads(renamed_iec_body).get("ied_name") != "RENAMED1":
                fail("IED rename was not visible after network confirmation")

            config_status, config_body, _ = request(
                port, "GET", "/api/v1/config", headers={"Cookie": cookie}
            )
            assert_status(config_status, 200, "config lookup")
            config_payload = json.loads(config_body)
            overview_version = int(config_payload["version"])
            overview_update = dict(config_payload)
            overview_update.pop("version", None)
            overview_update["overview_title"] = "现场局放监测"
            overview_update["overview_device"] = "2号主变"
            overview_update["phase_start_degree"] = 45
            overview_update_status, overview_update_body, _ = request(
                port,
                "PUT",
                "/api/v1/config",
                json_body(overview_update),
                {
                    "Content-Type": "application/json",
                    "Cookie": cookie,
                    "X-CSRF-Token": csrf_token,
                    "If-Match": f'"{overview_version}"',
                },
            )
            assert_status(overview_update_status, 200, "overview settings update")
            assert_json(overview_update_body, "version", overview_version + 1, "overview settings update")
            config_payload = overview_update
            config_payload["version"] = overview_version + 1
            public_overview_status, public_overview_body, _ = request(
                port, "GET", "/api/v1/overview"
            )
            assert_status(public_overview_status, 200, "updated public overview settings")
            public_overview = json.loads(public_overview_body)
            if public_overview.get("overview_title") != "现场局放监测" or public_overview.get("phase_start_degree") != 45:
                fail(f"overview settings update was not public: {public_overview!r}")
            schema_status, schema_body, _ = request(
                port, "GET", "/api/v1/config/schema", headers={"Cookie": cookie}
            )
            assert_status(schema_status, 200, "config schema lookup")
            if json.loads(schema_body).get("properties", {}).get("iec_port", {}).get("maximum") != 65535:
                fail("configuration schema is incomplete")
            logs_status, logs_body, _ = request(
                port, "GET", "/api/v1/logs", headers={"Cookie": cookie}
            )
            assert_status(logs_status, 200, "logs lookup")
            logs_payload = json.loads(logs_body)
            if not isinstance(logs_payload.get("entries"), list):
                fail("logs endpoint did not return an entry list")
            config_version = int(config_payload["version"])
            config_without_csrf_status, _, _ = request(
                port,
                "PUT",
                "/api/v1/config",
                json_body(config_payload),
                {"Content-Type": "application/json", "Cookie": cookie, "If-Match": f'"{config_version}"'},
            )
            assert_status(config_without_csrf_status, 403, "config CSRF guard")
            stale_config_status, _, _ = request(
                port,
                "PUT",
                "/api/v1/config",
                json_body(config_payload),
                {
                    "Content-Type": "application/json",
                    "Cookie": cookie,
                    "X-CSRF-Token": csrf_token,
                    "If-Match": f'"{config_version + 1}"',
                },
            )
            assert_status(stale_config_status, 409, "stale config version")
            changed_config_status, changed_config_body, _ = request(
                port,
                "PUT",
                "/api/v1/config",
                json_body(config_payload),
                {
                    "Content-Type": "application/json",
                    "Cookie": cookie,
                    "X-CSRF-Token": csrf_token,
                    "If-Match": f'"{config_version}"',
                },
            )
            assert_status(changed_config_status, 200, "config update")
            assert_json(changed_config_body, "version", config_version + 1, "config update")
            assert_json(changed_config_body, "restart_required", False, "config update")

            password_payload = {
                "current_password": initial_password,
                "new_password": "Smoke-Password-2026!",
            }
            no_csrf_status, _, _ = request(
                port,
                "PUT",
                "/api/v1/password",
                json_body(password_payload),
                {"Content-Type": "application/json", "Cookie": cookie},
            )
            assert_status(no_csrf_status, 403, "password CSRF guard")

            bad_current_status, _, _ = request(
                port,
                "PUT",
                "/api/v1/password",
                json_body({"current_password": "wrong-password", "new_password": "Smoke-Password-2026!"}),
                {"Content-Type": "application/json", "Cookie": cookie, "X-CSRF-Token": csrf_token},
            )
            assert_status(bad_current_status, 401, "wrong current password")

            short_password_status, _, _ = request(
                port,
                "PUT",
                "/api/v1/password",
                json_body({"current_password": initial_password, "new_password": "short"}),
                {"Content-Type": "application/json", "Cookie": cookie, "X-CSRF-Token": csrf_token},
            )
            assert_status(short_password_status, 400, "short new password")

            changed_status, changed_body, changed_headers = request(
                port,
                "PUT",
                "/api/v1/password",
                json_body(password_payload),
                {"Content-Type": "application/json", "Cookie": cookie, "X-CSRF-Token": csrf_token},
            )
            assert_status(changed_status, 200, "password change")
            assert_json(changed_body, "changed", True, "password change")
            if "Max-Age=0" not in changed_headers.get("Set-Cookie", ""):
                fail("password change did not expire the old session cookie")
            if bootstrap_path.exists():
                fail("one-time initial password file was not removed")
            auth_text = auth_path.read_text(encoding="utf-8")
            auth_record = json.loads(auth_text)
            if "password" in auth_record or "current_password" in auth_record or "new_password" in auth_record:
                fail("plaintext password was written to auth.json")
            if password_payload["new_password"] in auth_text:
                fail("new plaintext password was written to auth.json")

            old_session_status, _, _ = request(
                port, "GET", "/api/v1/session", headers={"Cookie": cookie}
            )
            assert_status(old_session_status, 401, "invalidated session")
            old_password_status, _, _ = request(
                port,
                "POST",
                "/api/v1/session",
                json_body({"username": "admin", "password": initial_password}),
                login_headers,
            )
            assert_status(old_password_status, 401, "old password")

            new_login_status, new_login_body, new_login_headers = request(
                port,
                "POST",
                "/api/v1/session",
                json_body({"username": "admin", "password": password_payload["new_password"]}),
                login_headers,
            )
            assert_status(new_login_status, 200, "new password login")
            new_session = assert_json(new_login_body, "must_change", False, "new password login")
            new_cookie = new_login_headers["Set-Cookie"].split(";", 1)[0]
            new_csrf = str(new_session["csrf_token"])

            health_status, health_body, _ = request(
                port, "GET", "/api/v1/health", headers={"Cookie": new_cookie}
            )
            assert_status(health_status, 200, "authenticated health")
            assert_json(health_body, "status", "down", "authenticated health")
            snapshot_status, _, _ = request(
                port, "GET", "/api/v1/snapshot/latest", headers={"Cookie": new_cookie}
            )
            assert_status(snapshot_status, 503, "snapshot without acquisition")

            logout_without_csrf_status, _, _ = request(
                port, "DELETE", "/api/v1/session", headers={"Cookie": new_cookie}
            )
            assert_status(logout_without_csrf_status, 403, "logout CSRF guard")
            logout_status, _, logout_headers = request(
                port,
                "DELETE",
                "/api/v1/session",
                headers={"Cookie": new_cookie, "X-CSRF-Token": new_csrf},
            )
            assert_status(logout_status, 204, "logout")
            if "Max-Age=0" not in logout_headers.get("Set-Cookie", ""):
                fail("logout did not expire the session cookie")

            for _ in range(5):
                rate_status, _, _ = request(
                    port,
                    "POST",
                    "/api/v1/session",
                    json_body({"username": "admin", "password": "wrong-password"}),
                    login_headers,
                )
                assert_status(rate_status, 401, "failed login")
            rate_status, _, rate_headers = request(
                port,
                "POST",
                "/api/v1/session",
                json_body({"username": "admin", "password": "wrong-password"}),
                login_headers,
            )
            assert_status(rate_status, 429, "login rate limit")
            if "Retry-After" not in rate_headers:
                fail("login rate limit did not include Retry-After")

            print("web server smoke: OK")
            return 0
        finally:
            process.terminate()
            try:
                process.communicate(timeout=2)
            except subprocess.TimeoutExpired:
                process.kill()
                process.communicate()
            if privileged_process is not None:
                privileged_process.terminate()
                try:
                    privileged_process.communicate(timeout=2)
                except subprocess.TimeoutExpired:
                    privileged_process.kill()
                    privileged_process.communicate()


if __name__ == "__main__":
    raise SystemExit(main())
