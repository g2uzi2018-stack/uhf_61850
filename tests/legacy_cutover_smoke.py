#!/usr/bin/env python3
"""Verify legacy cutover only changes the exact cron line and matching processes."""

from pathlib import Path
import subprocess
import sys
import tempfile


def fail(message: str) -> None:
    raise SystemExit(f"legacy cutover smoke failed: {message}")


def write_executable(path: Path, contents: str) -> None:
    path.write_text(contents, encoding="utf-8")
    path.chmod(0o700)


def make_crontab(path: Path, initial: str) -> Path:
    state = path / "crontab"
    state.write_text(initial, encoding="utf-8")
    script = path / "crontab-tool"
    write_executable(
        script,
        "#!/bin/sh\n"
        f"state={state!s}\n"
        'if [ "$1" = "-l" ]; then\n'
        '  [ -f "$state" ] || exit 1\n'
        '  cat "$state"\n'
        "  exit 0\n"
        "fi\n"
        'cp "$1" "$state"\n',
    )
    return script


def make_kill(path: Path) -> tuple[Path, Path]:
    log = path / "kill.log"
    script = path / "kill-tool"
    write_executable(
        script,
        "#!/bin/sh\n"
        f'log={log!s}\n'
        'if [ "$1" = "-0" ]; then exit 1; fi\n'
        'printf "%s %s\\n" "$1" "$2" >> "$log"\n',
    )
    return script, log


def make_process(proc_root: Path, pid: str, cwd: Path, arguments: list[str]) -> None:
    process = proc_root / pid
    process.mkdir()
    (process / "cwd").symlink_to(cwd)
    (process / "cmdline").write_bytes(b"\0".join(argument.encode() for argument in arguments) + b"\0")


def run(script: Path, *args: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        ["bash", str(script), *args],
        capture_output=True,
        text=True,
        check=False,
    )


def setup_root(root: Path, cron_text: str) -> tuple[Path, Path, Path, Path]:
    data = root / "data"
    data.mkdir(parents=True)
    proc = root / "proc"
    proc.mkdir()
    tools = root / "tools"
    tools.mkdir()
    crontab = make_crontab(tools, cron_text)
    kill, kill_log = make_kill(tools)
    return data, proc, crontab, kill_log


def main() -> int:
    if len(sys.argv) != 2:
        fail("expected repository root")
    repo = Path(sys.argv[1])
    cutover = repo / "packaging/legacy-cutover.sh"
    normalized_legacy_root = 'legacy_root="${path_prefix}/data"'
    if normalized_legacy_root not in cutover.read_text(encoding="utf-8"):
        fail("cutover does not normalize the root path before matching /data")
    with tempfile.TemporaryDirectory(prefix="uhf-legacy-cutover-") as temporary:
        root = Path(temporary)
        cron_text = (
            "# keep this entry\n"
            "@reboot sudo /data/run.sh &\n"
            "0 * * * * /data/health.sh\n"
            "@reboot /usr/bin/frpc -c /etc/frpc.ini\n"
        )
        data, proc, crontab, kill_log = setup_root(root, cron_text)
        (data / "run.sh").write_text(
            "#!/bin/sh\n"
            f"printf recovered > {data / 'recovered'}\n",
            encoding="utf-8",
        )
        (data / "run.sh").chmod(0o700)
        make_process(proc, "101", data, ["/usr/bin/python", "Web.py"])
        make_process(proc, "102", data, ["/usr/bin/python", "/data/Main.py"])
        make_process(proc, "103", data, ["/usr/bin/frpc", "-c", "frpc.ini"])
        other = root / "other"
        other.mkdir()
        make_process(proc, "104", other, ["/usr/bin/python", "Web.py"])
        state_dir = root / "var/lib/uhf-gateway"
        state_dir.mkdir(parents=True)
        (state_dir / "release-state.json").write_text(
            '{"version":1,"current":"first","previous":"","pending":"first"}\n',
            encoding="utf-8",
        )
        result = run(
            cutover,
            "--root",
            str(root),
            "--proc-root",
            str(proc),
            "--crontab",
            str(crontab),
            "--kill",
            str(root / "tools/kill-tool"),
            "--no-wait",
        )
        if result.returncode != 0:
            fail(f"cutover failed: {result.stderr.strip()}")
        expected = cron_text.replace(
            "@reboot sudo /data/run.sh &",
            "# uhf-gateway legacy disabled: @reboot sudo /data/run.sh &",
        )
        actual = (root / "tools/crontab").read_text(encoding="utf-8")
        if actual != expected:
            fail("cutover changed more than the exact legacy cron line")
        backup = root / "var/lib/uhf-gateway/legacy/root-crontab.before-cutover"
        if backup.read_text(encoding="utf-8") != cron_text:
            fail("root crontab backup is not byte-for-byte complete")
        process_log = (root / "var/lib/uhf-gateway/legacy/processes.tsv").read_text(encoding="utf-8")
        if "pid=101" not in process_log or "pid=102" not in process_log:
            fail("matching legacy processes were not recorded")
        if "pid=103" in process_log or "pid=104" in process_log:
            fail("unrelated process was selected")
        if kill_log.read_text(encoding="utf-8").splitlines() != ["-TERM 101", "-TERM 102"]:
            fail("unexpected process termination set")
        marker = root / "var/lib/uhf-gateway/legacy/recovery-enabled"
        if marker.exists():
            fail("legacy recovery marker was created")

        result = run(
            cutover,
            "--root",
            str(root),
            "--proc-root",
            str(proc),
            "--crontab",
            str(crontab),
            "--kill",
            str(root / "tools/kill-tool"),
            "--no-wait",
        )
        if result.returncode != 0 or kill_log.read_text(encoding="utf-8").splitlines() != [
            "-TERM 101",
            "-TERM 102",
        ]:
            fail("idempotent cutover was not preserved")

        duplicate_root = root / "duplicate"
        duplicate_root.mkdir()
        duplicate_data, duplicate_proc, duplicate_crontab, duplicate_kill_log = setup_root(
            duplicate_root,
            "@reboot sudo /data/run.sh &\n@reboot sudo /data/run.sh &\n",
        )
        make_process(duplicate_proc, "201", duplicate_data, ["/usr/bin/python", "Web.py"])
        result = run(
            cutover,
            "--root",
            str(duplicate_root),
            "--proc-root",
            str(duplicate_proc),
            "--crontab",
            str(duplicate_crontab),
            "--kill",
            str(duplicate_root / "tools/kill-tool"),
            "--no-wait",
        )
        if result.returncode == 0:
            fail("duplicate legacy cron entries were accepted")
        if (duplicate_root / "tools/crontab").read_text(encoding="utf-8") != (
            "@reboot sudo /data/run.sh &\n@reboot sudo /data/run.sh &\n"
        ):
            fail("failed cutover modified the crontab")
        if duplicate_kill_log.exists() and duplicate_kill_log.read_text(encoding="utf-8"):
            fail("failed cutover stopped a process")
    print("legacy cutover smoke: OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
