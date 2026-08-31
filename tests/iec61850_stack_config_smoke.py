#!/usr/bin/env python3
"""Verify that the bundled IEC stack is built with the product service limits."""

from pathlib import Path
import re
import sys


def fail(message: str) -> None:
    raise SystemExit(f"IEC 61850 stack config smoke failed: {message}")


def main() -> int:
    if len(sys.argv) != 2:
        fail("expected generated stack_config.h")
    header = Path(sys.argv[1])
    if not header.is_file():
        fail(f"missing generated header: {header}")
    text = header.read_text(encoding="utf-8")
    expected = {
        "CONFIG_MMS_MAXIMUM_PDU_SIZE": "16384",
        "CONFIG_MAXIMUM_TCP_CLIENT_CONNECTIONS": "4",
        "CONFIG_IEC61850_MEMORY_LIMIT_BYTES": "8388608",
        "CONFIG_MMS_MAX_DATA_STRUCTURE_NESTING_LEVEL": "32",
        "CONFIG_DEFAULT_MAX_SERV_OUTSTANDING_CALLING": "2",
        "CONFIG_DEFAULT_MAX_SERV_OUTSTANDING_CALLED": "2",
        "CONFIG_INCLUDE_GOOSE_SUPPORT": "0",
        "CONFIG_IEC61850_SAMPLED_VALUES_SUPPORT": "0",
        "CONFIG_IEC61850_CONTROL_SERVICE": "0",
        "CONFIG_IEC61850_SETTING_GROUPS": "0",
        "CONFIG_IEC61850_LOG_SERVICE": "0",
        "CONFIG_IEC61850_SERVICE_TRACKING": "1",
    }
    for name, value in expected.items():
        pattern = rf"^#define\s+{re.escape(name)}\s+{re.escape(value)}\s*$"
        if not re.search(pattern, text, re.MULTILINE):
            fail(f"{name} is not {value}")
    print("IEC 61850 stack config smoke: OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
