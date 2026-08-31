#!/usr/bin/env bash
set -euo pipefail

usage() {
    printf 'usage: %s [--root DIR] [--release DIR] [--skip-hardware] [--skip-arch] [--allow-legacy]\n' "$0" >&2
}

root_dir=/
release_dir=
skip_hardware=false
skip_arch=false
allow_legacy=false
while [[ $# -gt 0 ]]; do
    case "$1" in
        --root)
            [[ $# -ge 2 ]] || { usage; exit 2; }
            root_dir=$2
            shift 2
            ;;
        --release)
            [[ $# -ge 2 ]] || { usage; exit 2; }
            release_dir=$2
            shift 2
            ;;
        --skip-hardware)
            skip_hardware=true
            shift
            ;;
        --skip-arch)
            skip_arch=true
            shift
            ;;
        --allow-legacy)
            allow_legacy=true
            shift
            ;;
        *)
            usage
            exit 2
            ;;
    esac
done

if [[ -z "$release_dir" ]]; then
    release_dir="${root_dir%/}/opt/uhf-gateway/current"
fi

required_files=(
    "bin/uhf-gatewayd"
    "bin/uhf-privilegedd"
    "bin/uhf-network-recovery"
    "web/index.html"
    "web/login.html"
    "config/defaults.json"
    "config/schema.json"
    "config/UHFPD1.icd"
    "config/manifest.json"
    "libexec/uhf-gateway/uhf-gateway-hook"
    "share/uhf-gateway/systemd/uhf-gateway.service"
    "share/uhf-gateway/systemd/uhf-privileged.service"
    "share/uhf-gateway/systemd/uhf-network-rollback.service"
    "share/uhf-gateway/systemd/uhf-network-rollback.timer"
    "share/uhf-gateway/systemd/uhf-network-recovery.service"
    "share/uhf-gateway/systemd/uhf-release-guard.service"
    "share/uhf-gateway/systemd/uhf-release-guard.timer"
    "libexec/uhf-gateway/release-guard.sh"
    "share/uhf-gateway/systemd/uhf-legacy-recovery.service"
    "libexec/uhf-gateway/legacy-cutover.sh"
    "libexec/uhf-gateway/legacy-recovery.sh"
    "share/uhf-gateway/rsyslog/uhf-gateway.conf"
    "share/uhf-gateway/logrotate/uhf-gateway"
)
for relative in "${required_files[@]}"; do
    if [[ ! -f "${release_dir}/${relative}" ]]; then
        printf 'preflight: missing %s\n' "$relative" >&2
        exit 1
    fi
done
for executable in uhf-gatewayd uhf-privilegedd uhf-network-recovery; do
    if [[ ! -x "${release_dir}/bin/${executable}" ]]; then
        printf 'preflight: not executable: %s\n' "$executable" >&2
        exit 1
    fi
done
if [[ ! -x "${release_dir}/libexec/uhf-gateway/uhf-gateway-hook" ]]; then
    printf 'preflight: DHCP hook is not executable\n' >&2
    exit 1
fi

if ! sh -n "${release_dir}/libexec/uhf-gateway/uhf-gateway-hook"; then
    printf 'preflight: DHCP hook syntax check failed\n' >&2
    exit 1
fi
if ! sh -n "${release_dir}/libexec/uhf-gateway/release-guard.sh"; then
    printf 'preflight: release guard syntax check failed\n' >&2
    exit 1
fi
if ! bash -n "${release_dir}/libexec/uhf-gateway/legacy-cutover.sh"; then
    printf 'preflight: legacy cutover syntax check failed\n' >&2
    exit 1
fi
if ! bash -n "${release_dir}/libexec/uhf-gateway/legacy-recovery.sh"; then
    printf 'preflight: legacy recovery syntax check failed\n' >&2
    exit 1
fi

if [[ "$skip_arch" == false ]]; then
    if ! command -v readelf >/dev/null 2>&1; then
        printf 'preflight: readelf is required for architecture validation\n' >&2
        exit 1
    fi
    machine=$(readelf -h "${release_dir}/bin/uhf-gatewayd" |
        awk '$1 == "Machine:" { print $2; exit }')
    if [[ "$machine" != "AArch64" ]]; then
        printf 'preflight: expected AArch64 gateway, got %s\n' "${machine:-unknown}" >&2
        exit 1
    fi
fi

if [[ "$skip_hardware" == false ]]; then
    if [[ "$(uname -m)" != "aarch64" ]]; then
        printf 'preflight: target architecture is not aarch64\n' >&2
        exit 1
    fi
    for device in /dev/ttyS1 /dev/ttyS4; do
        if [[ ! -e "$device" ]]; then
            printf 'preflight: missing serial device %s\n' "$device" >&2
            exit 1
        fi
    done
    if ! command -v ss >/dev/null 2>&1; then
        printf 'preflight: ss is required for port validation\n' >&2
        exit 1
    fi
    for port in 102 502 8080; do
        if ss -H -ltn "sport = :${port}" | grep -q .; then
            if [[ "$allow_legacy" == true && "$port" == 502 ]]; then
                printf 'preflight: allowing legacy port 502 for first cutover\n'
                continue
            fi
            printf 'preflight: TCP port %s is occupied\n' "$port" >&2
            exit 1
        fi
    done
    available_kib=$(df -Pk "$root_dir" | awk 'NR == 2 { print $4; exit }')
    if [[ ! "$available_kib" =~ ^[0-9]+$ ]] || (( available_kib < 262144 )); then
        printf 'preflight: less than 256 MiB free on %s\n' "$root_dir" >&2
        exit 1
    fi
    if command -v timedatectl >/dev/null 2>&1 &&
       [[ "$(timedatectl show -p NTPSynchronized --value 2>/dev/null || true)" != "yes" ]]; then
        printf 'preflight: NTP is not synchronized\n' >&2
        exit 1
    fi
fi

printf 'preflight: OK (%s)\n' "$release_dir"
