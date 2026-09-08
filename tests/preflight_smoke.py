#!/usr/bin/env python3
"""Exercise release preflight checks using a temporary install tree."""

from pathlib import Path
import os
import shutil
import subprocess
import sys
import tempfile


def fail(message: str) -> None:
    raise SystemExit(f"preflight smoke failed: {message}")


def run(
    script: Path,
    release: Path,
    *extra: str,
    environment: dict[str, str] | None = None,
) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        ["bash", str(script), "--release", str(release), *extra],
        capture_output=True,
        text=True,
        check=False,
        env=environment,
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

        target = Path(temporary) / "target"
        (target / "dev").mkdir(parents=True)
        (target / "dev/ttyS1").touch()
        (target / "dev/ttyS4").touch()
        current_binary = target / "opt/uhf-gateway/current/bin/uhf-gatewayd"
        current_binary.parent.mkdir(parents=True)
        shutil.copy2(release / "bin/uhf-gatewayd", current_binary)
        commands = Path(temporary) / "commands"
        commands.mkdir()
        (commands / "uname").write_text("#!/bin/sh\nprintf 'aarch64\\n'\n", encoding="utf-8")
        (commands / "df").write_text(
            "#!/bin/sh\nprintf 'Filesystem 1024-blocks Used Available Capacity Mounted on\\n'\n"
            "printf '/dev/test 1048576 1 1048575 1%% /\\n'\n",
            encoding="utf-8",
        )
        (commands / "timedatectl").write_text("#!/bin/sh\nprintf 'yes\\n'\n", encoding="utf-8")
        (commands / "systemctl").write_text("#!/bin/sh\nprintf '4242\\n'\n", encoding="utf-8")
        (commands / "ss").write_text(
            "#!/bin/sh\n"
            "pid=${UHF_PREFLIGHT_TEST_PID:-4242}\n"
            "printf 'LISTEN 0 16 0.0.0.0:21 0.0.0.0:* users:((\\\"uhf-gatewayd\\\",pid=%s,fd=9))\\n' \"$pid\"\n",
            encoding="utf-8",
        )
        (commands / "readlink").write_text(
            "#!/bin/sh\n"
            "if [ \"$2\" = /proc/4242/exe ]; then\n"
            "  printf '%s\\n' \"$UHF_PREFLIGHT_TEST_EXECUTABLE\"\n"
            "else\n"
            "  exec /usr/bin/readlink \"$@\"\n"
            "fi\n",
            encoding="utf-8",
        )
        for command in commands.iterdir():
            command.chmod(0o755)
        environment = dict(os.environ)
        environment["PATH"] = f"{commands}:{environment['PATH']}"
        environment["UHF_PREFLIGHT_TEST_EXECUTABLE"] = str(current_binary.resolve())
        result = run(
            script,
            release,
            "--root",
            str(target),
            "--skip-arch",
            "--allow-current-product",
            environment=environment,
        )
        if result.returncode != 0:
            fail(f"current product listeners were rejected: {result.stderr.strip()}")
        result = run(
            script,
            release,
            "--root",
            str(target),
            "--skip-arch",
            environment=environment,
        )
        if result.returncode == 0:
            fail("occupied ports were accepted without current-product authorization")
        foreign_environment = dict(environment)
        foreign_environment["UHF_PREFLIGHT_TEST_PID"] = "9999"
        result = run(
            script,
            release,
            "--root",
            str(target),
            "--skip-arch",
            "--allow-current-product",
            environment=foreign_environment,
        )
        if result.returncode == 0:
            fail("foreign listener was accepted as the current product")

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
