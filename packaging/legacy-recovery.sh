#!/usr/bin/env bash
set -euo pipefail

usage() {
    printf 'usage: %s [--root DIR] [--crontab PATH]\n' "$0" >&2
}

root_dir=/
crontab_bin=crontab
while [[ $# -gt 0 ]]; do
    case "$1" in
        --root)
            [[ $# -ge 2 ]] || { usage; exit 2; }
            root_dir=$2
            shift 2
            ;;
        --crontab)
            [[ $# -ge 2 ]] || { usage; exit 2; }
            crontab_bin=$2
            shift 2
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
if [[ "$root_prefix" == "/" && "$(id -u)" -ne 0 ]]; then
    printf 'legacy recovery must run as root\n' >&2
    exit 1
fi

state_root="${root_prefix}/var/lib/uhf-gateway/legacy"
recovery_marker="${state_root}/recovery-enabled"
if [[ ! -e "$recovery_marker" ]]; then
    exit 0
fi

release_state="${root_prefix}/var/lib/uhf-gateway/release-state.json"
pending=
if [[ -f "$release_state" ]]; then
    pending=$(sed -n 's/.*"pending":"\([^"]*\)".*/\1/p' "$release_state")
fi
if [[ -z "$pending" ]]; then
    exit 0
fi

backup_file="${state_root}/root-crontab.before-cutover"
disabled_file="${state_root}/root-crontab.disabled"
legacy_root="${root_prefix%/}/data"
legacy_script="${legacy_root}/run.sh"
if [[ ! -f "$backup_file" || ! -f "$disabled_file" ]]; then
    printf 'legacy recovery: missing cutover backup\n' >&2
    exit 1
fi
if [[ ! -x "$legacy_script" ]]; then
    printf 'legacy recovery: legacy launcher is not executable\n' >&2
    exit 1
fi

temporary_dir=$(mktemp -d "${state_root}/.recovery.XXXXXX")
current_crontab="${temporary_dir}/current"
cleanup() {
    rm -rf -- "$temporary_dir"
}
trap cleanup EXIT
if "$crontab_bin" -l >"$current_crontab" 2>/dev/null; then
    :
else
    status=$?
    if (( status == 1 )); then
        : >"$current_crontab"
    else
        printf 'legacy recovery: cannot read root crontab\n' >&2
        exit "$status"
    fi
fi
if ! cmp -s "$disabled_file" "$current_crontab"; then
    printf 'legacy recovery: root crontab changed after cutover; refusing overwrite\n' >&2
    exit 1
fi
"$crontab_bin" "$backup_file"

recovery_state="${state_root}/recovered"
recovery_state_temporary="${recovery_state}.tmp.$$"
(
    cd "$legacy_root"
    "$legacy_script"
) </dev/null >/dev/null 2>&1 &
legacy_pid=$!
printf 'version=1\npending=%s\nlegacy_pid=%s\n' "$pending" "$legacy_pid" >"$recovery_state_temporary"
chmod 0600 "$recovery_state_temporary"
mv -Tf "$recovery_state_temporary" "$recovery_state"
rm -f -- "$recovery_marker"
printf 'legacy recovery: restored legacy launcher (pid %s)\n' "$legacy_pid"
