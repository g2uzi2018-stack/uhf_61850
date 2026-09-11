#!/usr/bin/env python3
"""Verify atomic release installation and current/previous links in a temp root."""

from pathlib import Path
import json
import subprocess
import sys
import tempfile


def fail(message: str) -> None:
    raise SystemExit(f"installer smoke failed: {message}")


def build_release(root: Path, output: Path, version: str) -> Path:
    result = subprocess.run(
        [
            "bash",
            str(root / "packaging/build-release.sh"),
            "--build-dir",
            str(root / "build/host"),
            "--version",
            version,
            "--output",
            str(output),
        ],
        cwd=root,
        capture_output=True,
        text=True,
        check=False,
    )
    if result.returncode != 0:
        fail(f"release assembly failed: {result.stderr.strip()}")
    return output / f"uhf-gateway-{version}"


def install(root: Path, package: Path, target: Path) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [
            "bash",
            str(root / "packaging/install.sh"),
            "--package",
            str(package),
            "--root",
            str(target),
            "--no-systemd",
            "--skip-arch",
            "--skip-hardware",
        ],
        cwd=root,
        capture_output=True,
        text=True,
        check=False,
    )


def main() -> int:
    if len(sys.argv) != 2:
        fail("expected repository root")
    root = Path(sys.argv[1])
    installer = (root / "packaging/install.sh").read_text(encoding="utf-8")
    if "path_prefix=$root_prefix" not in installer:
        fail("installer does not normalize the host root before constructing paths")
    if "preflight_args+=(--allow-current-product)" not in installer:
        fail("installer does not authorize the verified current product during upgrades")
    with tempfile.TemporaryDirectory(prefix="uhf-installer-") as temporary:
        temporary_root = Path(temporary)
        packages = temporary_root / "packages"
        packages.mkdir()
        first = build_release(root, packages, "smoke-1")
        target = temporary_root / "target"
        result = install(root, first, target)
        if result.returncode != 0:
            fail(f"first install failed: {result.stderr.strip()}")
        product = target / "opt/uhf-gateway"
        current = product / "current"
        if current.resolve() != (product / "releases/smoke-1").resolve():
            fail("current does not point to first release")
        if (product / "previous").exists():
            fail("first install unexpectedly created previous")
        state = json.loads(
            (target / "var/lib/uhf-gateway/release-state.json").read_text(encoding="utf-8")
        )
        if state != {
            "version": 1,
            "current": "smoke-1",
            "previous": "",
            "pending": "",
            "health_mode": "relaxed",
        }:
            fail(f"unexpected first release state: {state!r}")
        if not (target / "etc/systemd/system/uhf-gateway.service").is_file():
            fail("systemd unit was not installed")
        if not (target / "usr/lib/uhf-gateway/dhclient-hook").is_file():
            fail("DHCP hook was not installed")
        if not (target / "etc/uhf-gateway/defaults.json").is_file():
            fail("default configuration was not installed")
        config_directory = target / "etc/uhf-gateway"
        if config_directory.stat().st_mode & 0o777 != 0o755:
            fail("configuration directory must be readable by the service user")
        icd_config = target / "etc/uhf-gateway/UHFPD1.icd"
        if not icd_config.is_file():
            fail("ICD configuration was not installed")
        if icd_config.stat().st_mode & 0o777 != 0o644:
            fail("ICD configuration must be service-readable")
        network_config = target / "etc/uhf-gateway/network.json"
        if not network_config.is_file():
            fail("default network configuration was not installed")
        if network_config.stat().st_mode & 0o777 != 0o600:
            fail("network configuration must remain private")
        if json.loads(network_config.read_text(encoding="utf-8"))["eth0_address"] != "192.168.3.230":
            fail("unexpected default network configuration")
        bootstrap = target / "var/lib/uhf-gateway/initial-password"
        auth = target / "var/lib/uhf-gateway/auth.json"
        if not bootstrap.is_file() or not auth.is_file():
            fail("authentication state was not provisioned")
        certificate = target / "var/lib/uhf-gateway/tls/server.crt"
        private_key = target / "var/lib/uhf-gateway/tls/server.key"
        if not certificate.is_file() or not private_key.is_file():
            fail("TLS state was not provisioned")
        if certificate.stat().st_mode & 0o777 != 0o600 or private_key.stat().st_mode & 0o777 != 0o600:
            fail("TLS state is not private")
        if bootstrap.stat().st_mode & 0o777 != 0o600 or auth.stat().st_mode & 0o777 != 0o600:
            fail("authentication state is not private")
        bootstrap_password = bootstrap.read_text(encoding="utf-8").strip()
        auth_record = json.loads(auth.read_text(encoding="utf-8"))
        if (
            "password" in auth_record
            or "current_password" in auth_record
            or "new_password" in auth_record
            or bootstrap_password in str(auth_record.get("hash_hex", ""))
        ):
            fail("plaintext bootstrap password was written to auth.json")
        if (target / "var/lib/uhf-gateway").stat().st_mode & 0o777 != 0o700:
            fail("gateway state directory is not private")
        for path in (
            target / "usr/lib/uhf-gateway/legacy-cutover.sh",
            target / "usr/lib/uhf-gateway/legacy-recovery.sh",
            target / "etc/rsyslog.d/uhf-gateway.conf",
            target / "etc/logrotate.d/uhf-gateway",
        ):
            if not path.is_file():
                fail(f"missing installed support file: {path.relative_to(target)}")

        network_config.write_text(
            network_config.read_text(encoding="utf-8").replace("192.168.3.230", "192.168.3.231"),
            encoding="utf-8",
        )

        second = build_release(root, packages, "smoke-2")
        result = install(root, second, target)
        if result.returncode != 0:
            fail(f"second install failed: {result.stderr.strip()}")
        if current.resolve() != (product / "releases/smoke-2").resolve():
            fail("current does not point to second release")
        if (product / "previous").resolve() != (product / "releases/smoke-1").resolve():
            fail("previous does not point to first release")
        state = json.loads(
            (target / "var/lib/uhf-gateway/release-state.json").read_text(encoding="utf-8")
        )
        if state != {
            "version": 1,
            "current": "smoke-2",
            "previous": "smoke-1",
            "pending": "",
            "health_mode": "relaxed",
        }:
            fail(f"unexpected upgrade state: {state!r}")
        if json.loads(network_config.read_text(encoding="utf-8"))["eth0_address"] != "192.168.3.231":
            fail("upgrade overwrote the existing network configuration")
        result = install(root, second, target)
        if result.returncode == 0:
            fail("duplicate release was accepted")
        if current.resolve() != (product / "releases/smoke-2").resolve():
            fail("duplicate install changed current")
    print("installer smoke: OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
