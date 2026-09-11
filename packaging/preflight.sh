#!/usr/bin/env bash
set -euo pipefail

usage() {
    printf 'usage: %s [--root DIR] [--release DIR] [--skip-hardware] [--skip-arch] [--allow-legacy] [--allow-current-product]\n' "$0" >&2
}

root_dir=/
release_dir=
skip_hardware=false
skip_arch=false
allow_legacy=false
allow_current_product=false
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
        --allow-current-product)
            allow_current_product=true
            shift
            ;;
        *)
            usage
            exit 2
            ;;
    esac
done

root_prefix=${root_dir%/}
if [[ -z "$root_prefix" ]]; then
    root_prefix=/
fi
path_prefix=$root_prefix
if [[ "$path_prefix" == "/" ]]; then
    path_prefix=
fi

if [[ -z "$release_dir" ]]; then
    release_dir="${path_prefix}/opt/uhf-gateway/current"
fi

required_files=(
    "bin/uhf-gatewayd"
    "bin/uhf-privilegedd"
    "bin/uhf-network-recovery"
    "bin/uhf-auth-init"
    "bin/uhf-tls-init"
    "web/index.html"
    "web/login.html"
    "config/defaults.json"
    "config/schema.json"
    "config/network.json"
    "config/UHFPD1.icd"
    "config/manifest.json"
    "libexec/uhf-gateway/uhf-gateway-hook"
    "share/uhf-gateway/systemd/uhf-gateway.service"
    "share/uhf-gateway/systemd/uhf-privileged.service"
    "share/uhf-gateway/systemd/uhf-network-rollback.service"
    "share/uhf-gateway/systemd/uhf-network-rollback.timer"
    "share/uhf-gateway/systemd/uhf-network-recovery.service"
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
for executable in uhf-gatewayd uhf-privilegedd uhf-network-recovery uhf-auth-init uhf-tls-init; do
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

current_product_owns_listeners() {
    local listeners=$1
    local main_pid=
    local expected_executable=
    local actual_executable=
    local line=
    local without_current_pid=
    local listener_seen=false
    if [[ "$allow_current_product" != true ]] || ! command -v systemctl >/dev/null 2>&1; then
        return 1
    fi
    main_pid=$(systemctl show uhf-gateway.service -p MainPID --value 2>/dev/null || true)
    if [[ ! "$main_pid" =~ ^[1-9][0-9]*$ ]]; then
        return 1
    fi
    expected_executable=$(readlink -f "${path_prefix}/opt/uhf-gateway/current/bin/uhf-gatewayd" 2>/dev/null || true)
    actual_executable=$(readlink -f "/proc/${main_pid}/exe" 2>/dev/null || true)
    if [[ -z "$expected_executable" || "$actual_executable" != "$expected_executable" ]]; then
        return 1
    fi
    while IFS= read -r line; do
        [[ -n "$line" ]] || continue
        listener_seen=true
        if [[ "$line" != *"pid=${main_pid},"* ]]; then
            return 1
        fi
        without_current_pid=${line//pid=${main_pid},/}
        if [[ "$without_current_pid" == *"pid="* ]]; then
            return 1
        fi
    done <<<"$listeners"
    [[ "$listener_seen" == true ]]
}

if [[ "$skip_hardware" == false ]]; then
    if [[ "$(uname -m)" != "aarch64" ]]; then
        printf 'preflight: target architecture is not aarch64\n' >&2
        exit 1
    fi
    for device in "${path_prefix}/dev/ttyS1" "${path_prefix}/dev/ttyS4"; do
        if [[ ! -e "$device" ]]; then
            printf 'preflight: missing serial device %s\n' "$device" >&2
            exit 1
        fi
    done
    if ! command -v ss >/dev/null 2>&1; then
        printf 'preflight: ss is required for port validation\n' >&2
        exit 1
    fi
    for port in 21 102 502 8080; do
        listeners=$(ss -H -ltnp "sport = :${port}")
        if [[ -n "$listeners" ]]; then
            if current_product_owns_listeners "$listeners"; then
                printf 'preflight: allowing current product listener on TCP port %s\n' "$port"
                continue
            fi
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
