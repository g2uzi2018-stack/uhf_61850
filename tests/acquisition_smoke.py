#!/usr/bin/env python3
"""Run the C++ acquisition engine against the PTY PD1000 simulator."""

import os
from pathlib import Path
import pty
import signal
import subprocess
import sys


def fail(message: str) -> None:
    raise SystemExit(f"acquisition smoke failed: {message}")


def main() -> int:
    if len(sys.argv) != 4:
        fail("expected acquisition binary, simulator and fixture")
    acquisition = Path(sys.argv[1])
    simulator = Path(sys.argv[2])
    fixture = Path(sys.argv[3])
    master_fd, slave_fd = pty.openpty()
    slave_path = os.ttyname(slave_fd)
    simulator_process = subprocess.Popen(
        [sys.executable, str(simulator), "--fd", str(master_fd), "--fixture", str(fixture)],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        pass_fds=(master_fd,),
    )
    acquisition_process = subprocess.Popen(
        [str(acquisition), slave_path],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    os.close(slave_fd)
    os.close(master_fd)
    try:
        try:
            acquisition_stdout, acquisition_stderr = acquisition_process.communicate(timeout=10)
        except subprocess.TimeoutExpired:
            acquisition_process.kill()
            acquisition_stdout, acquisition_stderr = acquisition_process.communicate()
            fail("C++ engine timed out")
        if acquisition_process.returncode != 0:
            fail(f"C++ engine exited {acquisition_process.returncode}: {acquisition_stderr.strip()}")
        if "acquisition smoke: OK" not in acquisition_stdout:
            fail(f"unexpected engine output: {acquisition_stdout!r}")
        print("acquisition smoke: OK")
        return 0
    finally:
        if simulator_process.poll() is None:
            simulator_process.send_signal(signal.SIGTERM)
        try:
            simulator_process.communicate(timeout=2)
        except subprocess.TimeoutExpired:
            simulator_process.kill()
            simulator_process.communicate()


if __name__ == "__main__":
    raise SystemExit(main())
