#!/usr/bin/env python3
"""Check that the offline user-management preview has its required assets."""

from pathlib import Path
import sys


def fail(message: str) -> None:
    raise SystemExit(f"web preview smoke failed: {message}")


def main() -> int:
    if len(sys.argv) != 2:
        fail("expected the web asset directory")

    web_dir = Path(sys.argv[1])
    required_files = (
        "index.html", "overview.html", "login.html", "styles.css", "app.js", "overview.js", "login.js"
    )
    for filename in required_files:
        if not (web_dir / filename).is_file():
            fail(f"missing {filename}")

    html = (web_dir / "index.html").read_text(encoding="utf-8")
    login_html = (web_dir / "login.html").read_text(encoding="utf-8")
    javascript = (web_dir / "app.js").read_text(encoding="utf-8")
    overview_html = (web_dir / "overview.html").read_text(encoding="utf-8")
    overview_javascript = (web_dir / "overview.js").read_text(encoding="utf-8")
    login_javascript = (web_dir / "login.js").read_text(encoding="utf-8")
    stylesheet = (web_dir / "styles.css").read_text(encoding="utf-8")

    for marker in ("用户管理", "本地开发模式", "admin", 'id="password-modal"', 'id="password-form"'):
        if marker not in html:
            fail(f"index.html is missing {marker!r}")
    for marker in ("登录控制台", "首次登录", 'id="login-form"'):
        if marker not in login_html:
            fail(f"login.html is missing {marker!r}")
    for marker in ('data-action="change-password"', "showToast", "password-form"):
        if marker not in javascript:
            fail(f"app.js is missing {marker!r}")
    for marker in ("实时总览", "prpd-canvas", "prps-canvas"):
        if marker not in overview_html:
            fail(f"overview.html is missing {marker!r}")
    for marker in ("drawPrpd", "drawPrps", "3600", "api/v1/snapshot/latest", "api/v1/health", "WebSocket"):
        if marker not in overview_javascript:
            fail(f"overview.js is missing {marker!r}")
    for marker in ("api/v1/session", "same-origin", "登录尝试过于频繁"):
        if marker not in login_javascript:
            fail(f"login.js is missing {marker!r}")
    for marker in (".sidebar", ".panel", "@media", ".modal"):
        if marker not in stylesheet:
            fail(f"styles.css is missing {marker!r}")
    if any(host in html for host in ("cdn.", "cdnjs.", "unpkg.com")):
        fail("preview must not load assets from a CDN")
    for page in (html, overview_html):
        if 'href="#"' in page:
            fail("navigation contains a placeholder link")

    print("web preview smoke: OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
