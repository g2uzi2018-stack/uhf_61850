#!/usr/bin/env python3
"""Exercise release preflight checks using a temporary install tree."""

from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


def fail(message: str) -> None:
    raise SystemExit(f"preflight smoke failed: {message}")


def run(script: Path, release: Path, *extra: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        ["bash", str(script), "--release", str(release), *extra],
        capture_output=True,
        text=True,
        check=False,
    )


def main() -> int:
    if len(sys.argv) != 2:
        fail("expected repository root")
    root = Path(sys.argv[1])
    script = root / "packaging/preflight.sh"
    build = root / "build/host"
    with tempfile.TemporaryDirectory(prefix="uhf-preflight-") as temporary:
        release = Path(temporary) / "release"
        result = subprocess.run(
            ["cmake", "--install", str(build), "--prefix", str(release)],
            cwd=root,
            capture_output=True,
            text=True,
            check=False,
        )
        if result.returncode != 0:
            fail(f"install failed: {result.stderr.strip()}")
        result = run(script, release, "--skip-hardware", "--skip-arch")
        if result.returncode != 0:
            fail(f"valid release rejected: {result.stderr.strip()}")
        result = run(script, release, "--skip-hardware")
        if result.returncode == 0:
            fail("host-architecture release was accepted")

        missing = release / "config/UHFPD1.icd"
        backup = release / "config/UHFPD1.icd.bak"
        shutil.move(missing, backup)
        try:
            result = run(script, release, "--skip-hardware", "--skip-arch")
            if result.returncode == 0:
                fail("release with missing ICD was accepted")
        finally:
            shutil.move(backup, missing)

        missing = release / "config/defaults.json"
        backup = release / "config/defaults.json.bak"
        shutil.move(missing, backup)
        try:
            result = run(script, release, "--skip-hardware", "--skip-arch")
            if result.returncode == 0:
                fail("release with missing defaults was accepted")
        finally:
            shutil.move(backup, missing)
    print("preflight smoke: OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
