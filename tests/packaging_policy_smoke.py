#!/usr/bin/env python3
"""Check the bounded syslog and logrotate policy shipped with the product."""

from pathlib import Path
import sys


def fail(message: str) -> None:
    raise SystemExit(f"packaging policy smoke failed: {message}")


def require(text: str, fragment: str, context: str) -> None:
    if fragment not in text:
        fail(f"{context}: missing {fragment!r}")


def main() -> int:
    if len(sys.argv) != 2:
        fail("expected repository root")
    root = Path(sys.argv[1])
    rsyslog = (root / "packaging/rsyslog/uhf-gateway.conf").read_text(encoding="utf-8")
    logrotate = (root / "packaging/logrotate/uhf-gateway").read_text(encoding="utf-8")
    installer = (root / "packaging/install.sh").read_text(encoding="utf-8")

    require(rsyslog, "$programname == 'uhf-gatewayd'", "rsyslog filter")
    require(rsyslog, "$syslogfacility-text == 'local0'", "rsyslog facility")
    require(rsyslog, "/var/log/uhf-gateway/gateway.log", "rsyslog destination")
    require(rsyslog, "stop", "rsyslog stop")
    require(logrotate, "daily", "logrotate daily")
    require(logrotate, "size 5M", "logrotate size")
    require(logrotate, "rotate 7", "logrotate retention")
    require(logrotate, "compress", "logrotate compression")
    require(logrotate, "create 0640 syslog syslog", "log ownership")
    require(logrotate, "missingok", "logrotate missingok")
    require(logrotate, "systemctl kill -s HUP rsyslog.service", "log reopen")
    require(installer, "systemctl enable uhf-gateway.service", "installer service enable")
    require(installer, "chown \"root:${syslog_group}\"", "rsyslog directory ownership")
    require(installer, "chmod 0750 \"${path_prefix}/var/log/uhf-gateway\"", "rsyslog directory mode")
    require(installer, "install -o syslog -g \"$syslog_group\" -m 0640 /dev/null", "active log provisioning")
    if "/data" in rsyslog or "/data" in logrotate:
        fail("policy must not target the legacy data service")
    print("packaging policy smoke: OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
