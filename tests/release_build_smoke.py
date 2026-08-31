#!/usr/bin/env python3
"""Build a release directory and archive without writing outside a temp tree."""

from pathlib import Path
import subprocess
import sys
import tarfile
import tempfile


def fail(message: str) -> None:
    raise SystemExit(f"release build smoke failed: {message}")


def main() -> int:
    if len(sys.argv) != 2:
        fail("expected repository root")
    root = Path(sys.argv[1])
    build = root / "build/host"
    script = root / "packaging/build-release.sh"
    if not (build / "cmake_install.cmake").is_file():
        fail("host build is not configured")
    with tempfile.TemporaryDirectory(prefix="uhf-release-build-") as temporary:
        output = Path(temporary)
        result = subprocess.run(
            [
                "bash",
                str(script),
                "--build-dir",
                str(build),
                "--version",
                "smoke-1",
                "--output",
                str(output),
            ],
            cwd=root,
            capture_output=True,
            text=True,
            check=False,
        )
        if result.returncode != 0:
            fail(result.stderr.strip())
        release = output / "uhf-gateway-smoke-1"
        archive = output / "uhf-gateway-smoke-1.tar.gz"
        required = (
            release / "RELEASE",
            release / "bin/uhf-gatewayd",
            release / "bin/uhf-privilegedd",
            release / "bin/uhf-network-recovery",
            release / "web/index.html",
            release / "config/UHFPD1.icd",
            release / "share/uhf-gateway/systemd/uhf-gateway.service",
            release / "share/uhf-gateway/rsyslog/uhf-gateway.conf",
            release / "share/uhf-gateway/logrotate/uhf-gateway",
            release / "install.sh",
            release / "preflight.sh",
        )
        for path in required:
            if not path.is_file():
                fail(f"missing {path.relative_to(release)}")
        release_text = (release / "RELEASE").read_text(encoding="utf-8")
        if "version=smoke-1\n" not in release_text or "password" in release_text.lower():
            fail("release metadata is invalid or contains a password")
        if not archive.is_file():
            fail("release archive is missing")
        with tarfile.open(archive, "r:gz") as package:
            names = set(package.getnames())
        if "uhf-gateway-smoke-1/bin/uhf-gatewayd" not in names:
            fail("archive does not contain the gateway binary")
    print("release build smoke: OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
