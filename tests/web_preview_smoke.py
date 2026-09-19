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
    maintenance_javascript = (web_dir / "maintenance.js").read_text(encoding="utf-8")
    login_html = (web_dir / "login.html").read_text(encoding="utf-8")
    javascript = (web_dir / "app.js").read_text(encoding="utf-8")
    overview_html = (web_dir / "overview.html").read_text(encoding="utf-8")
    overview_javascript = (web_dir / "overview.js").read_text(encoding="utf-8")
    network_html = (web_dir / "network.html").read_text(encoding="utf-8")
    network_javascript = (web_dir / "network.js").read_text(encoding="utf-8")
    logs_html = (web_dir / "logs.html").read_text(encoding="utf-8")
    logs_javascript = (web_dir / "logs.js").read_text(encoding="utf-8")
    settings_html = (web_dir / "settings.html").read_text(encoding="utf-8")
    settings_javascript = (web_dir / "settings.js").read_text(encoding="utf-8")
    storage_html = (web_dir / "storage.html").read_text(encoding="utf-8")
    storage_javascript = (web_dir / "storage.js").read_text(encoding="utf-8")
    login_javascript = (web_dir / "login.js").read_text(encoding="utf-8")
    stylesheet = (web_dir / "styles.css").read_text(encoding="utf-8")

    for marker in ("用户管理", "本地开发模式", "HTTPS", "transport-badge", "checklist-progress", "admin", 'id="password-modal"', 'id="password-form"'):
        if marker not in html:
            fail(f"index.html is missing {marker!r}")
    for marker in ("certificate-replacement", "tls-certificate", "tls-private-key", "replace-tls", "certificate_pem", "private_key_pem"):
        if marker not in maintenance_javascript:
            fail(f"maintenance.js is missing {marker!r}")
    for marker in ("登录控制台", "初始密码", 'id="login-form"'):
        if marker not in login_html:
            fail(f"login.html is missing {marker!r}")
    for marker in ('data-action="change-password"', "showToast", "updateChecklistProgress", "password-form"):
        if marker not in javascript:
            fail(f"app.js is missing {marker!r}")
    for marker in (
        "实时总览", "综合监测管理机", "prpd-canvas", "prps-canvas",
        "overview-title-input", "phase-start-degree", "device-status",
        "data-pd-channel", "v3-measurements-body", "v3-alarms", "source-current-state",
    ):
        if marker not in overview_html:
            fail(f"overview.html is missing {marker!r}")
    for marker in (
        "drawPrpd", "drawPrps", "3600", "api/v1/snapshot/latest", "api/v1/health",
        "api/v1/overview", "phaseStartBin", "WebSocket", "schema_version",
        "updateV3Snapshot", "selectedPdChannel", "qualityText", "renderAlarms",
    ):
        if marker not in overview_javascript:
            fail(f"overview.js is missing {marker!r}")
    for obsolete in ("PD1000", "dBm"):
        if obsolete in overview_html or obsolete in overview_javascript:
            fail(f"v3 overview still contains obsolete assumption {obsolete!r}")
    for marker in (
        "IP 设置入口", "IP 修改操作步骤", "① 试应用 IP", "② 确认并永久保存 IP",
        "保存并应用 IED 名称", "SNTP 服务器", "手动设备时间", "api/v1/time",
    ):
        if marker not in network_html and marker not in network_javascript:
            fail(f"network assets are missing {marker!r}")
    if network_html.index('type="submit">① 试应用 IP') > network_html.index('id="confirm-network"'):
        fail("network confirmation action is not positioned after staging")
    for marker in (
        "netmaskForPrefix", "prefixForNetmask", "legacyNetworkFormat",
        "captureConfigurationForm", "restoreConfigurationForm", "configuration version conflict",
        "网络配置已重新读取",
    ):
        if marker not in network_javascript:
            fail(f"network.js is missing compatibility marker {marker!r}")
    for marker in ("api/v1/session", "same-origin", "登录尝试过于频繁", "location.protocol"):
        if marker not in login_javascript:
            fail(f"login.js is missing {marker!r}")
    for marker in (
        "三路采集报文",
        "最多 256 条",
        "不会写入磁盘",
        "packets-body",
        "/api/v1/packets",
        "dropped_entries",
    ):
        if marker not in logs_html and marker not in logs_javascript:
            fail(f"packet trace view is missing {marker!r}")
    for marker in (
        "FTP 文件传输",
        "ftp_enabled",
        "ftp_port",
        "下载 v3 Modbus 点表 CSV",
        "/api/v1/point-table/export.csv",
    ):
        if marker not in (web_dir / "settings.html").read_text(encoding="utf-8") and marker not in (web_dir / "settings.js").read_text(encoding="utf-8"):
            fail(f"FTP settings are missing {marker!r}")
    for marker in (
        'data-runtime="legacy" hidden', 'data-runtime="v3" hidden',
        "未继承旧版 dBm 事件策略", "detectRuntimeMode", "applyRuntimeMode",
        "snapshot/latest", "schema_version",
    ):
        if marker not in settings_html and marker not in settings_javascript:
            fail(f"runtime-aware settings are missing {marker!r}")
    for marker in (
        "v3 历史快照", "35 项计算值", "三源时间与质量", "applyRuntimeMode",
        'fetchJson("/api/v1/frames")', 'mode === "v3"', "snapshot/latest",
    ):
        if marker not in storage_html and marker not in storage_javascript:
            fail(f"runtime-aware storage page is missing {marker!r}")
    for marker in ("MMS 监听全部网口", "客户端连接设备当前 IP"):
        if marker not in (web_dir / "iec61850.html").read_text(encoding="utf-8") and marker not in (web_dir / "iec61850.js").read_text(encoding="utf-8"):
            fail(f"IEC endpoint guidance is missing {marker!r}")
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
