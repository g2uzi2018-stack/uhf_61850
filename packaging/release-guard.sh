#!/usr/bin/env bash
set -euo pipefail

usage() {
    printf 'usage: %s [--root DIR] [--systemctl PATH] [--no-sleep]\n' "$0" >&2
}

root_dir=/
systemctl_bin=systemctl
no_sleep=false
while [[ $# -gt 0 ]]; do
    case "$1" in
        --root)
            [[ $# -ge 2 ]] || { usage; exit 2; }
            root_dir=$2
            shift 2
            ;;
        --systemctl)
            [[ $# -ge 2 ]] || { usage; exit 2; }
            systemctl_bin=$2
            shift 2
            ;;
        --no-sleep)
            no_sleep=true
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
product_root="${path_prefix}/opt/uhf-gateway"
current_link="${product_root}/current"
previous_link="${product_root}/previous"
release_root="${product_root}/releases"
state_file="${path_prefix}/var/lib/uhf-gateway/release-state.json"
recovery_marker="${path_prefix}/var/lib/uhf-gateway/legacy/recovery-enabled"
if [[ ! -f "$state_file" ]]; then
    exit 0
fi

field() {
    local name=$1
    sed -n 's/.*"'"${name}"'":"\([^" ]*\)".*/\1/p' "$state_file"
}
number_field() {
    local name=$1
    sed -n 's/.*"'"${name}"'":\([0-9][0-9]*\).*/\1/p' "$state_file"
}
write_state() {
    local current=$1
    local previous=$2
    local pending=$3
    local healthy=$4
    local failures=$5
    local temporary="${state_file}.tmp.$$"
    printf '{"version":1,"current":"%s","previous":"%s","pending":"%s","healthy":%s,"failures":%s}\n' \
        "$current" "$previous" "$pending" "$healthy" "$failures" >"$temporary"
    chmod 0600 "$temporary"
    mv -Tf "$temporary" "$state_file"
}

pending=$(field pending)
if [[ -z "$pending" ]]; then
    exit 0
fi
current=$(field current)
previous=$(field previous)
healthy=$(number_field healthy)
healthy=${healthy:-0}
failures=$(number_field failures)
failures=${failures:-0}
if [[ -z "$current" || ! "$pending" =~ ^[A-Za-z0-9][A-Za-z0-9._-]{0,47}$ ||
      ! "$current" =~ ^[A-Za-z0-9][A-Za-z0-9._-]{0,47}$ ]]; then
    printf 'release guard: invalid release state\n' >&2
    exit 1
fi

if "$systemctl_bin" is-active --quiet uhf-gateway.service; then
    healthy=$((healthy + 1))
    failures=0
    if (( healthy >= 3 )); then
        write_state "$current" "$previous" "" 0 0
        rm -f -- "$recovery_marker"
    else
        write_state "$current" "$previous" "$pending" "$healthy" "$failures"
    fi
    exit 0
fi

healthy=0
failures=$((failures + 1))
if (( failures < 3 )); then
    write_state "$current" "$previous" "$pending" "$healthy" "$failures"
    "$systemctl_bin" restart uhf-gateway.service >/dev/null 2>&1 || true
    if [[ "$no_sleep" == false ]]; then
        sleep 2
    fi
    exit 0
fi

if [[ -z "$previous" || ! "$previous" =~ ^[A-Za-z0-9][A-Za-z0-9._-]{0,47}$ ||
      ! -d "${release_root}/${previous}" ]]; then
    printf 'release guard: no valid previous release for rollback\n' >&2
    write_state "$current" "$previous" "$pending" "$healthy" "$failures"
    exit 1
fi
old_current_target=$(readlink -f "$current_link")
old_previous_target=$(readlink -f "$previous_link")
if [[ "$old_current_target" != "${release_root}/"* ||
      "$old_previous_target" != "${release_root}/"* ]]; then
    printf 'release guard: release link is outside managed root\n' >&2
    exit 1
fi
atomic_link() {
    local target=$1
    local link=$2
    local temporary="${link}.tmp.$$"
    if [[ -e "$temporary" || -L "$temporary" ]]; then
        return 1
    fi
    ln -s "$target" "$temporary"
    mv -Tf "$temporary" "$link"
}
atomic_link "$old_previous_target" "$current_link"
atomic_link "$old_current_target" "$previous_link"
write_state "$previous" "$current" "" 0 0
"$systemctl_bin" daemon-reload >/dev/null 2>&1 || true
"$systemctl_bin" restart uhf-gateway.service >/dev/null 2>&1 || true
printf 'release guard: rolled back %s to %s\n' "$current" "$previous"
