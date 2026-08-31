#!/usr/bin/env python3
"""Verify install-time TLS bootstrap creates a usable certificate and key."""

from pathlib import Path
import socket
import ssl
import subprocess
import sys
import tempfile


def fail(message: str) -> None:
    raise SystemExit(f"TLS init smoke failed: {message}")


def main() -> int:
    if len(sys.argv) != 2:
        fail("expected TLS initializer executable")
    executable = Path(sys.argv[1])
    with tempfile.TemporaryDirectory(prefix="uhf-tls-init-") as temporary:
        state = Path(temporary)
        result = subprocess.run(
            [str(executable), "--state-dir", str(state)],
            capture_output=True,
            text=True,
            check=False,
        )
        if result.returncode != 0:
            fail(result.stderr.strip())
        certificate = state / "tls/server.crt"
        private_key = state / "tls/server.key"
        if not certificate.is_file() or not private_key.is_file():
            fail("initializer did not create both TLS files")
        if certificate.stat().st_mode & 0o777 != 0o600 or private_key.stat().st_mode & 0o777 != 0o600:
            fail("TLS files are not private")
        decoded = ssl._ssl._test_decode_cert(str(certificate))
        sans = set(decoded.get("subjectAltName", ()))
        if ("DNS", "localhost") not in sans or ("IP Address", "127.0.0.1") not in sans:
            fail(f"required SANs are missing: {sans!r}")
        hostname = socket.gethostname()
        if hostname and hostname != "localhost" and ("DNS", hostname) not in sans:
            fail("device hostname is missing from certificate SANs")
        second = subprocess.run(
            [str(executable), "--state-dir", str(state)],
            capture_output=True,
            text=True,
            check=False,
        )
        if second.returncode != 0:
            fail(f"existing TLS state was not accepted: {second.stderr.strip()}")
    print("TLS init smoke: OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
