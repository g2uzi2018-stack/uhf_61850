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


def write_fake_curl(path: Path, body: str) -> None:
    path.write_text(
        "#!/bin/sh\n"
        f"printf '%s\\n' '{body}'\n",
        encoding="utf-8",
    )
    path.chmod(0o700)


def run_guard(
    script: Path, root: Path, fake_systemctl: Path, fake_curl: Path
) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [
            "bash",
            str(script),
            "--root",
            str(root),
            "--systemctl",
            str(fake_systemctl),
            "--curl",
            str(fake_curl),
            "--no-sleep",
        ],
        cwd=root,
        capture_output=True,
        text=True,
        check=False,
    )


def setup(root: Path, health_mode: str = "strict") -> tuple[Path, Path, Path]:
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
        f'{{"version":1,"current":"new","previous":"old","pending":"new","health_mode":"{health_mode}"}}\n',
        encoding="utf-8",
    )
    legacy_dir = state_dir / "legacy"
    legacy_dir.mkdir()
    (legacy_dir / "recovery-enabled").touch()
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
        fake_curl = healthy_root / "curl"
        write_fake_systemctl(fake, True)
        write_fake_curl(
            fake_curl,
            '{"status":"up","web_auth":"ready","components":{"acquisition":"up","storage":"up","modbus_tcp":"up","modbus_rtu":"up","iec61850":"up"}}',
        )
        for _ in range(3):
            result = run_guard(script, healthy_root, fake, fake_curl)
            if result.returncode != 0:
                fail(f"healthy release was rejected: {result.stderr.strip()}")
        state = json.loads(
            (healthy_root / "var/lib/uhf-gateway/release-state.json").read_text(encoding="utf-8")
        )
        if state["pending"] != "" or product.joinpath("current").resolve() != new.resolve():
            fail("healthy release was not confirmed")
        if (healthy_root / "var/lib/uhf-gateway/legacy/recovery-enabled").exists():
            fail("legacy recovery marker was not cleared after confirmation")

        failed_root = Path(temporary) / "failed"
        failed_root.mkdir()
        product, old, new = setup(failed_root)
        fake = failed_root / "systemctl"
        fake_curl = failed_root / "curl"
        write_fake_systemctl(fake, False)
        write_fake_curl(fake_curl, "not-used")
        for _ in range(3):
            result = run_guard(script, failed_root, fake, fake_curl)
            if result.returncode != 0 and _ < 2:
                fail(f"failure counter stopped early: {result.stderr.strip()}")
        if product.joinpath("current").resolve() != old.resolve():
            fail("failed release was not rolled back")
        if product.joinpath("previous").resolve() != new.resolve():
            fail("failed release was not retained as previous")
        state = json.loads(
            (failed_root / "var/lib/uhf-gateway/release-state.json").read_text(encoding="utf-8")
        )
        if state != {
            "version": 1,
            "current": "old",
            "previous": "new",
            "pending": "",
            "healthy": 0,
            "failures": 0,
            "health_mode": "strict",
        }:
            fail(f"unexpected rollback state: {state!r}")

        relaxed_root = Path(temporary) / "relaxed"
        relaxed_root.mkdir()
        product, _, _ = setup(relaxed_root, "relaxed")
        fake = relaxed_root / "systemctl"
        fake_curl = relaxed_root / "curl"
        write_fake_systemctl(fake, True)
        write_fake_curl(
            fake_curl,
            '{"status":"down","web_auth":"ready","components":{"acquisition":"down","storage":"up","modbus_tcp":"up","modbus_rtu":"up","iec61850":"up"}}',
        )
        for _ in range(3):
            result = run_guard(script, relaxed_root, fake, fake_curl)
            if result.returncode != 0:
                fail(f"relaxed health mode rejected missing hardware: {result.stderr.strip()}")
        relaxed_state = json.loads(
            (relaxed_root / "var/lib/uhf-gateway/release-state.json").read_text(encoding="utf-8")
        )
        if relaxed_state["pending"] != "":
            fail("relaxed health mode did not confirm the release")
    print("release guard smoke: OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
