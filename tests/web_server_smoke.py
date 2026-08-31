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
            if len(initial_password) != 32:
                fail("initial password does not have the expected length")

            root_status, _, root_headers = request(port, "GET", "/")
            assert_status(root_status, 302, "unauthenticated root")
            if root_headers.get("Location") != "/login":
                fail("unauthenticated root did not redirect to /login")
            for protected_path in ("/api/v1/health", "/api/v1/snapshot/latest"):
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
            if session.get("must_change") is not True or not isinstance(session.get("csrf_token"), str):
                fail(f"initial login did not require password change: {session!r}")
            csrf_token = str(session["csrf_token"])
            set_cookie = login_headers_response.get("Set-Cookie", "")
            if not set_cookie.startswith("uhf_session=") or "HttpOnly" not in set_cookie or "SameSite=Strict" not in set_cookie:
                fail(f"session cookie is missing required attributes: {set_cookie!r}")
            cookie = set_cookie.split(";", 1)[0]

            session_status, session_body, _ = request(
                port, "GET", "/api/v1/session", headers={"Cookie": cookie}
            )
            assert_status(session_status, 200, "session lookup")
            assert_json(session_body, "must_change", True, "session lookup")
            page_status, page_body, _ = request(port, "GET", "/", headers={"Cookie": cookie})
            assert_status(page_status, 200, "authenticated root")
            if "用户管理".encode("utf-8") not in page_body:
                fail("authenticated root page was not served")
            overview_status, overview_body, _ = request(
                port, "GET", "/overview.html", headers={"Cookie": cookie}
            )
            assert_status(overview_status, 200, "authenticated overview")
            if "实时总览".encode("utf-8") not in overview_body:
                fail("authenticated overview page was not served")

            config_status, config_body, _ = request(
                port, "GET", "/api/v1/config", headers={"Cookie": cookie}
            )
            assert_status(config_status, 200, "config lookup")
            config_payload = json.loads(config_body)
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
            if initial_password in auth_text or password_payload["new_password"] in auth_text:
                fail("plaintext password was written to auth.json")

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


if __name__ == "__main__":
    raise SystemExit(main())
