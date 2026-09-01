#!/usr/bin/env python3
"""Check the systemd hardening and recovery policy shipped with the product."""

from pathlib import Path
import sys


def fail(message: str) -> None:
    raise SystemExit(f"systemd policy smoke failed: {message}")


def require(text: str, fragment: str, unit: str) -> None:
    if fragment not in text:
        fail(f"{unit}: missing {fragment!r}")


def main() -> int:
    if len(sys.argv) != 2:
        fail("expected repository root")
    root = Path(sys.argv[1]) / "packaging/systemd"
    names = (
        "uhf-gateway.service",
        "uhf-privileged.service",
        "uhf-network-rollback.service",
        "uhf-network-rollback.timer",
        "uhf-network-recovery.service",
        "uhf-release-guard.service",
        "uhf-release-guard.timer",
        "uhf-legacy-recovery.service",
    )
    units = {}
    for name in names:
        path = root / name
        if not path.is_file():
            fail(f"missing {name}")
        units[name] = path.read_text(encoding="utf-8")
        if ("/data/" in units[name] or " /data " in units[name] or
                "frpc" in units[name] or "4g_server" in units[name]):
            fail(f"{name}: policy touches protected legacy services")

    gateway = units["uhf-gateway.service"]
    require(gateway, "User=uhfgateway", "uhf-gateway.service")
    require(gateway, "SupplementaryGroups=dialout", "uhf-gateway.service")
    for fragment in (
        "Type=notify",
        "ConditionFileIsExecutable=/opt/uhf-gateway/current/bin/uhf-gatewayd",
        "WatchdogSec=20s",
        "Restart=on-failure",
        "StartLimitBurst=5",
        "MemoryMax=128M",
        "TasksMax=64",
        "NoNewPrivileges=true",
        "ProtectSystem=strict",
        "DeviceAllow=/dev/ttyS1 rw",
        "DeviceAllow=/dev/ttyS4 rw",
        "CapabilityBoundingSet=CAP_NET_BIND_SERVICE",
    ):
        require(gateway, fragment, "uhf-gateway.service")

    privileged = units["uhf-privileged.service"]
    for fragment in (
        "User=root",
        "ConditionFileIsExecutable=/opt/uhf-gateway/current/bin/uhf-privilegedd",
        "ExecStart=/opt/uhf-gateway/current/bin/uhf-privilegedd",
        "--vendor-eth0-config /etc/net.conf --vendor-eth1-config /etc/net2.conf",
        "ReadWritePaths=/etc/uhf-gateway /var/lib/uhf-privileged /run/uhf-gateway",
        "CapabilityBoundingSet=CAP_NET_ADMIN CAP_NET_RAW",
        "RestrictAddressFamilies=AF_UNIX AF_INET AF_INET6 AF_NETLINK",
        "NoNewPrivileges=true",
    ):
        require(privileged, fragment, "uhf-privileged.service")
    for unit in (
        "uhf-privileged.service",
        "uhf-network-rollback.service",
        "uhf-network-recovery.service",
    ):
        for fragment in ("/etc/htnet", "/etc/net.conf", "/etc/net2.conf"):
            require(units[unit], fragment, unit)

    rollback = units["uhf-network-rollback.service"]
    recovery = units["uhf-network-recovery.service"]
    for unit, text in (("uhf-network-rollback.service", rollback), ("uhf-network-recovery.service", recovery)):
        require(text, "DefaultDependencies=no", unit)
        require(text, "uhf-network-recovery", unit)
        require(text, "ConditionFileIsExecutable=", unit)
        require(text, "--vendor-eth0-config /etc/net.conf --vendor-eth1-config /etc/net2.conf", unit)
        require(text, "RestrictAddressFamilies=AF_UNIX AF_INET AF_INET6 AF_NETLINK", unit)
        require(text, "--transaction /var/lib/uhf-privileged/network-transaction.json", unit)
        if "RuntimeDirectory=uhf-gateway" in text or "/run/uhf-gateway" in text:
            fail(f"{unit}: oneshot must not own the shared privileged runtime directory")
    require(units["uhf-network-rollback.timer"], "OnUnitActiveSec=5s", "uhf-network-rollback.timer")
    require(units["uhf-network-rollback.timer"], "Persistent=true", "uhf-network-rollback.timer")
    guard = units["uhf-release-guard.service"]
    require(guard, "ExecStart=/usr/lib/uhf-gateway/release-guard.sh", "uhf-release-guard.service")
    require(guard, "ReadWritePaths=/opt/uhf-gateway /var/lib/uhf-gateway", "uhf-release-guard.service")
    require(guard, "CapabilityBoundingSet=CAP_DAC_READ_SEARCH CAP_DAC_OVERRIDE", "uhf-release-guard.service")
    require(units["uhf-release-guard.timer"], "OnUnitActiveSec=5s", "uhf-release-guard.timer")
    require(units["uhf-gateway.service"], "OnFailure=uhf-legacy-recovery.service", "uhf-gateway.service")
    legacy = units["uhf-legacy-recovery.service"]
    require(legacy, "ExecStart=/usr/lib/uhf-gateway/legacy-recovery.sh", "uhf-legacy-recovery.service")
    require(legacy, "ConditionPathExists=/var/lib/uhf-gateway/legacy/recovery-enabled", "uhf-legacy-recovery.service")
    require(legacy, "ReadWritePaths=/var/lib/uhf-gateway /var/spool/cron", "uhf-legacy-recovery.service")
    print("systemd policy smoke: OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
