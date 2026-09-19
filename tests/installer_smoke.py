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


def install(
    root: Path,
    package: Path,
    target: Path,
    provisioning: tuple[Path, Path, Path, Path] | None = None,
) -> subprocess.CompletedProcess[str]:
    arguments = [
        "bash",
        str(root / "packaging/install.sh"),
        "--package",
        str(package),
        "--root",
        str(target),
        "--no-systemd",
        "--skip-arch",
        "--skip-hardware",
    ]
    if provisioning is not None:
        runtime, identity, key, code = provisioning
        arguments.extend(
            [
                "--v3-runtime-env-file",
                str(runtime),
                "--v3-device-id-file",
                str(identity),
                "--v3-manufacturer-key-file",
                str(key),
                "--v3-activation-code-file",
                str(code),
            ]
        )
    return subprocess.run(
        arguments,
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
    if "systemctl enable --now uhf-privileged.service" in installer:
        fail("installer only enables the privileged service instead of restarting it")
    if "restart_release_service uhf-privileged.service uhf-privilegedd" not in installer:
        fail("installer does not force the privileged service onto the installed release")
    if "restart_release_service uhf-gateway.service uhf-gatewayd" not in installer:
        fail("installer does not verify the gateway service executable after restart")
    if installer.find("restart_release_service uhf-privileged.service uhf-privilegedd") > installer.find(
        "restart_release_service uhf-gateway.service uhf-gatewayd"
    ):
        fail("installer must refresh the privileged helper before restarting the gateway")
    if 'readlink -f "/proc/${main_pid}/exe"' not in installer:
        fail("installer does not verify the running service executable")
    with tempfile.TemporaryDirectory(prefix="uhf-installer-") as temporary:
        temporary_root = Path(temporary)
        packages = temporary_root / "packages"
        packages.mkdir()
        first = build_release(root, packages, "smoke-1")
        provisioning_root = temporary_root / "provisioning"
        provisioning_root.mkdir()
        runtime = provisioning_root / "v3-runtime.env"
        runtime.write_text(
            "UHF_V3_PD_DEVICE=/dev/ttyS1\n"
            "UHF_V3_CURRENT_DEVICE=/dev/ttyS2\n"
            "UHF_V3_TEMPERATURE_DEVICE=/dev/ttyS3\n",
            encoding="utf-8",
        )
        identity = provisioning_root / "device-id"
        key = provisioning_root / "manufacturer-key"
        code = provisioning_root / "activation-code"
        identity.write_text("test-board\n", encoding="utf-8")
        key.write_text("test-key-not-for-production\n", encoding="utf-8")
        code.write_text("AAAAA-AAAAA-AAAAA-AAAAA\n", encoding="utf-8")
        provisioning = (runtime, identity, key, code)
        missing_root = temporary_root / "missing-target"
        missing = install(root, first, missing_root)
        if missing.returncode == 0 or "v3 runtime environment is required" not in missing.stderr:
            fail("installer accepted a first install without v3 manufacturing inputs")
        if missing_root.exists():
            fail("installer mutated the target before rejecting missing v3 inputs")
        rejected_root = temporary_root / "rejected-target"
        rejected = install(
            root,
            first,
            rejected_root,
            (root / "config/v3-runtime.env.example", identity, key, code),
        )
        if rejected.returncode == 0 or "REPLACE_" not in rejected.stderr:
            fail("installer accepted an unconfigured v3 runtime template")
        linked_root = temporary_root / "linked-target"
        linked_config = linked_root / "etc/uhf-gateway"
        linked_config.mkdir(parents=True)
        (linked_config / "v3-manufacturer-key").symlink_to(key)
        linked = install(root, first, linked_root, provisioning)
        if linked.returncode == 0 or "regular non-link" not in linked.stderr:
            fail("installer accepted a provisioning target symlink")
        if (linked_root / "opt/uhf-gateway").exists():
            fail("installer copied a release before rejecting a target symlink")
        target = temporary_root / "target"
        result = install(root, first, target, provisioning)
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
        expected_provisioning = {
            "v3-runtime.env": runtime.read_text(encoding="utf-8"),
            "v3-device-id": identity.read_text(encoding="utf-8"),
            "v3-manufacturer-key": key.read_text(encoding="utf-8"),
            "v3-activation-code": code.read_text(encoding="utf-8"),
        }
        for name, expected_contents in expected_provisioning.items():
            installed = config_directory / name
            if installed.read_text(encoding="utf-8") != expected_contents:
                fail(f"installed v3 provisioning changed {name}")
            if installed.stat().st_mode & 0o777 != 0o600:
                fail(f"non-systemd test install did not protect {name}")
        device_policy = target / "etc/systemd/system/uhf-gateway.service.d/v3-devices.conf"
        expected_policy = (
            "[Service]\n"
            "DeviceAllow=/dev/ttyS1 rw\n"
            "DeviceAllow=/dev/ttyS2 rw\n"
            "DeviceAllow=/dev/ttyS3 rw\n"
        )
        if device_policy.read_text(encoding="utf-8") != expected_policy:
            fail("installer did not generate the exact v3 systemd device allowlist")
        if device_policy.stat().st_mode & 0o777 != 0o644:
            fail("v3 systemd device allowlist has the wrong mode")
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
        }:
            fail(f"unexpected upgrade state: {state!r}")
        if json.loads(network_config.read_text(encoding="utf-8"))["eth0_address"] != "192.168.3.231":
            fail("upgrade overwrote the existing network configuration")
        for name, expected_contents in expected_provisioning.items():
            if (config_directory / name).read_text(encoding="utf-8") != expected_contents:
                fail(f"upgrade overwrote v3 provisioning {name}")
        result = install(root, second, target)
        if result.returncode == 0:
            fail("duplicate release was accepted")
        if current.resolve() != (product / "releases/smoke-2").resolve():
            fail("duplicate install changed current")
    print("installer smoke: OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
