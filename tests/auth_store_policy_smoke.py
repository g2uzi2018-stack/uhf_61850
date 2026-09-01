#!/usr/bin/env python3
"""Check that an already-private root-owned bootstrap file is accepted."""

from pathlib import Path
import sys


def fail(message: str) -> None:
    raise SystemExit(f"auth store policy smoke failed: {message}")


def main() -> int:
    if len(sys.argv) != 2:
        fail("expected repository root")
    source = (Path(sys.argv[1]) / "src/web/auth_store.cpp").read_text(encoding="utf-8")
    start = source.find("void ensure_private_file(")
    end = source.find("uhf::web::AuthStore::Record load_record", start)
    if start < 0 or end < 0:
        fail("private-file helper was not found")
    helper = source[start:end]
    if "struct stat status {}" not in helper:
        fail("private-file helper does not inspect existing ownership and mode")
    if "(status.st_mode & 07777) == kPrivateFileMode" not in helper:
        fail("private-file helper does not accept an already-private file")
    print("auth store policy smoke: OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
