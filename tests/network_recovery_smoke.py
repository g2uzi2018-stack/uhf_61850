#!/usr/bin/env python3
"""Exercise the independent one-shot network recovery entry point."""

from pathlib import Path
import json
import subprocess
import sys
import tempfile


def fail(message: str) -> None:
    raise SystemExit(f"network recovery smoke failed: {message}")


def run(binary: Path, network: Path, transaction: Path) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [
            str(binary),
            "--network-config",
            str(network),
            "--transaction",
            str(transaction),
        ],
        capture_output=True,
        text=True,
        check=False,
    )


def main() -> int:
    if len(sys.argv) != 2:
        fail("expected recovery binary")
    binary = Path(sys.argv[1])
    if not binary.is_file():
        fail(f"missing binary: {binary}")
    with tempfile.TemporaryDirectory(prefix="uhf-network-recovery-") as temporary:
        root = Path(temporary)
        network = root / "etc" / "network.json"
        transaction = root / "var" / "transaction.json"
        result = run(binary, network, transaction)
        if result.returncode != 0:
            fail(f"empty recovery failed: {result.stderr.strip()}")
        if not network.is_file():
            fail("recovery did not initialize the network config")
        default_config = json.loads(network.read_text(encoding="utf-8"))
        if default_config.get("eth0_address") != "192.168.3.230":
            fail("recovery initialized an unexpected network config")

        network.write_text(
            json.dumps(
                {
                    "eth0_mode": "dhcp",
                    "eth0_address": "",
                    "eth0_prefix": 24,
                    "eth0_gateway": "",
                    "eth0_dns1": "",
                    "eth0_dns2": "",
                    "eth0_hostname": "gateway",
                    "eth0_dhcp_timeout_seconds": 15,
                    "eth1_mode": "static",
                    "eth1_address": "192.168.0.230",
                    "eth1_prefix": 24,
                    "eth1_gateway": "",
                    "eth1_dns1": "",
                    "eth1_dns2": "",
                    "eth1_hostname": "",
                    "eth1_dhcp_timeout_seconds": 15,
                }
            ),
            encoding="utf-8",
        )
        network.with_name(network.name + ".leases").write_text(
            "version=1\n"
            "eth0 192.168.3.240 24 192.168.3.2 192.168.3.1 -\n"
            "eth1 -\n",
            encoding="utf-8",
        )
        result = run(binary, network, transaction)
        if result.returncode != 0:
            fail(f"DHCP no-transaction recovery failed: {result.stderr.strip()}")
        if (root / "etc" / "dhcp").exists():
            fail("early recovery started the DHCP runtime")

        transaction.parent.mkdir(parents=True, exist_ok=True)
        transaction.write_text("{\"version\":1,\"state\":\"staged\"}", encoding="utf-8")
        result = run(binary, network, transaction)
        if result.returncode == 0:
            fail("corrupt transaction was accepted")
        if transaction.read_text(encoding="utf-8") != "{\"version\":1,\"state\":\"staged\"}":
            fail("corrupt transaction was overwritten")
    print("network recovery smoke: OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
