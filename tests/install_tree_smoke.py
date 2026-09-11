#!/usr/bin/env python3
"""Verify that CMake installs a self-contained, offline product tree."""

from pathlib import Path
import json
import shutil
import subprocess
import sys
import tempfile


def fail(message: str) -> None:
    raise SystemExit(f"install tree smoke failed: {message}")


def main() -> int:
    if len(sys.argv) != 3:
        fail("expected repository and build directories")
    root = Path(sys.argv[1])
    build = Path(sys.argv[2])
    if not (build / "cmake_install.cmake").is_file():
        fail(f"missing configured build: {build}")

    with tempfile.TemporaryDirectory(prefix="uhf-install-tree-") as temporary:
        prefix = Path(temporary) / "release"
        result = subprocess.run(
            ["cmake", "--install", str(build), "--prefix", str(prefix)],
            cwd=root,
            capture_output=True,
            text=True,
            check=False,
        )
        if result.returncode != 0:
            fail(f"cmake install failed: {result.stderr.strip()}")
        required = (
            prefix / "bin/uhf-gatewayd",
            prefix / "bin/uhf-privilegedd",
            prefix / "bin/uhf-network-recovery",
            prefix / "bin/uhf-auth-init",
            prefix / "bin/uhf-tls-init",
            prefix / "web/index.html",
            prefix / "web/login.js",
            prefix / "etc/uhf-gateway/defaults.json",
            prefix / "etc/uhf-gateway/network.json",
            prefix / "config/schema.json",
            prefix / "config/defaults.json",
            prefix / "config/network.json",
            prefix / "config/UHFPD1.icd",
            prefix / "config/manifest.json",
            prefix / "libexec/uhf-gateway/uhf-gateway-hook",
            prefix / "share/uhf-gateway/systemd/uhf-gateway.service",
            prefix / "share/uhf-gateway/systemd/uhf-privileged.service",
            prefix / "share/uhf-gateway/systemd/uhf-network-rollback.service",
            prefix / "share/uhf-gateway/systemd/uhf-network-rollback.timer",
            prefix / "share/uhf-gateway/systemd/uhf-network-recovery.service",
            prefix / "share/uhf-gateway/systemd/uhf-legacy-recovery.service",
            prefix / "libexec/uhf-gateway/legacy-cutover.sh",
            prefix / "libexec/uhf-gateway/legacy-recovery.sh",
            prefix / "share/uhf-gateway/rsyslog/uhf-gateway.conf",
            prefix / "share/uhf-gateway/logrotate/uhf-gateway",
            prefix / "share/doc/uhf_61850/LICENSE",
        )
        for path in required:
            if not path.is_file():
                fail(f"missing installed file: {path.relative_to(prefix)}")
        defaults = json.loads(
            (prefix / "etc/uhf-gateway/defaults.json").read_text(encoding="utf-8")
        )
        if defaults.get("modbus_tcp_bind") != "192.168.3.230":
            fail("product default does not bind Modbus/TCP to eth0")
        if (prefix / "web/index.html").read_text(encoding="utf-8").find("https://") >= 0:
            fail("web tree unexpectedly references a remote asset")
        if (prefix / "libexec/uhf-gateway/uhf-gateway-hook").stat().st_mode & 0o111 == 0:
            fail("DHCP hook is not executable")
        shutil.rmtree(prefix)
    print("install tree smoke: OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
