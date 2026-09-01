#!/usr/bin/env python3
"""Verify release guard stability confirmation and three-failure rollback."""

from pathlib import Path
import json
import os
import subprocess
import sys
import tempfile


def fail(message: str) -> None:
    raise SystemExit(f"release guard smoke failed: {message}")


def write_fake_systemctl(path: Path, active: bool) -> None:
    path.write_text(
        "#!/bin/sh\n"
        "if [ \"$1\" = is-active ]; then\n"
        f"  exit {'0' if active else '1'}\n"
        "fi\n"
        "exit 0\n",
        encoding="utf-8",
    )
    path.chmod(0o700)


def run_guard(script: Path, root: Path, fake_systemctl: Path) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [
            "bash",
            str(script),
            "--root",
            str(root),
            "--systemctl",
            str(fake_systemctl),
            "--no-sleep",
        ],
        cwd=root,
        capture_output=True,
        text=True,
        check=False,
    )


def setup(root: Path) -> tuple[Path, Path, Path]:
    product = root / "opt/uhf-gateway"
    old = product / "releases/old"
    new = product / "releases/new"
    old.mkdir(parents=True)
    new.mkdir(parents=True)
    (product / "current").symlink_to(new)
    (product / "previous").symlink_to(old)
    state_dir = root / "var/lib/uhf-gateway"
    state_dir.mkdir(parents=True)
    (state_dir / "release-state.json").write_text(
        '{"version":1,"current":"new","previous":"old","pending":"new"}\n',
        encoding="utf-8",
    )
    return product, old, new


def main() -> int:
    if len(sys.argv) != 2:
        fail("expected repository root")
    root = Path(sys.argv[1])
    script = root / "packaging/release-guard.sh"
    if "path_prefix=$root_prefix" not in script.read_text(encoding="utf-8"):
        fail("release guard does not normalize the host root before constructing paths")
    with tempfile.TemporaryDirectory(prefix="uhf-release-guard-") as temporary:
        healthy_root = Path(temporary) / "healthy"
        healthy_root.mkdir()
        product, _, new = setup(healthy_root)
        fake = healthy_root / "systemctl"
        write_fake_systemctl(fake, True)
        for _ in range(3):
            result = run_guard(script, healthy_root, fake)
            if result.returncode != 0:
                fail(f"healthy release was rejected: {result.stderr.strip()}")
        state = json.loads(
            (healthy_root / "var/lib/uhf-gateway/release-state.json").read_text(encoding="utf-8")
        )
        if state["pending"] != "" or product.joinpath("current").resolve() != new.resolve():
            fail("healthy release was not confirmed")

        failed_root = Path(temporary) / "failed"
        failed_root.mkdir()
        product, old, new = setup(failed_root)
        fake = failed_root / "systemctl"
        write_fake_systemctl(fake, False)
        for _ in range(3):
            result = run_guard(script, failed_root, fake)
            if result.returncode != 0 and _ < 2:
                fail(f"failure counter stopped early: {result.stderr.strip()}")
        if product.joinpath("current").resolve() != old.resolve():
            fail("failed release was not rolled back")
        if product.joinpath("previous").resolve() != new.resolve():
            fail("failed release was not retained as previous")
        state = json.loads(
            (failed_root / "var/lib/uhf-gateway/release-state.json").read_text(encoding="utf-8")
        )
        if state != {"version": 1, "current": "old", "previous": "new", "pending": "", "healthy": 0, "failures": 0}:
            fail(f"unexpected rollback state: {state!r}")
    print("release guard smoke: OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
