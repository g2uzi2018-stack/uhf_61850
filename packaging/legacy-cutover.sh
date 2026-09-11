#!/usr/bin/env bash
set -euo pipefail

usage() {
    printf 'usage: %s [--root DIR] [--proc-root DIR] [--crontab PATH] [--kill PATH] [--no-wait]\n' "$0" >&2
}

root_dir=/
proc_root=/proc
crontab_bin=crontab
kill_bin=kill
no_wait=false
while [[ $# -gt 0 ]]; do
    case "$1" in
        --root)
            [[ $# -ge 2 ]] || { usage; exit 2; }
            root_dir=$2
            shift 2
            ;;
        --proc-root)
            [[ $# -ge 2 ]] || { usage; exit 2; }
            proc_root=$2
            shift 2
            ;;
        --crontab)
            [[ $# -ge 2 ]] || { usage; exit 2; }
            crontab_bin=$2
            shift 2
            ;;
        --kill)
            [[ $# -ge 2 ]] || { usage; exit 2; }
            kill_bin=$2
            shift 2
            ;;
        --no-wait)
            no_wait=true
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
if [[ "$root_prefix" == "/" && "$(id -u)" -ne 0 ]]; then
    printf 'legacy cutover must run as root\n' >&2
    exit 1
fi

legacy_root="${path_prefix}/data"
state_root="${path_prefix}/var/lib/uhf-gateway/legacy"
backup_file="${state_root}/root-crontab.before-cutover"
disabled_file="${state_root}/root-crontab.disabled"
processes_file="${state_root}/processes.tsv"
state_file="${state_root}/cutover.state"

if [[ -e "$state_file" ]]; then
    printf 'legacy cutover: already prepared\n'
    exit 0
fi
if [[ -e "$backup_file" || -e "$disabled_file" ]]; then
    printf 'legacy cutover: incomplete prior state requires manual review\n' >&2
    exit 1
fi
if [[ ! -d "$legacy_root" ]]; then
    printf 'legacy cutover: missing legacy root %s\n' "$legacy_root" >&2
    exit 1
fi

mkdir -p "$state_root"
chmod 0700 "$state_root"
temporary_dir=$(mktemp -d "${state_root}/.tmp.XXXXXX")
cron_original="${temporary_dir}/original"
cron_expected="${temporary_dir}/expected"
cron_actual="${temporary_dir}/actual"
operation_ok=false
cron_changed=false

restore_crontab_on_error() {
    if [[ "$operation_ok" != true && "$cron_changed" == true ]]; then
        "$crontab_bin" "$cron_original" >/dev/null 2>&1 || true
    fi
    rm -rf -- "$temporary_dir"
}
trap restore_crontab_on_error EXIT

read_crontab() {
    local output=$1
    if "$crontab_bin" -l >"$output" 2>/dev/null; then
        return 0
    fi
    local status=$?
    if (( status == 1 )); then
        : >"$output"
        return 0
    fi
    return "$status"
}

read_crontab "$cron_original"
cp -- "$cron_original" "$backup_file"
chmod 0600 "$backup_file"

legacy_cron_line='@reboot sudo /data/run.sh &'
match_count=$(awk -v target="$legacy_cron_line" '$0 == target { count++ } END { print count + 0 }' "$cron_original")
if (( match_count > 1 )); then
    printf 'legacy cutover: multiple exact legacy cron entries found\n' >&2
    exit 1
fi
if (( match_count == 1 )); then
    sed 's|^@reboot sudo /data/run\.sh &$|# uhf-gateway legacy disabled: @reboot sudo /data/run.sh \&|' \
        "$cron_original" >"$cron_expected"
    "$crontab_bin" "$cron_expected"
    cron_changed=true
else
    cp -- "$cron_original" "$cron_expected"
fi
cp -- "$cron_expected" "$disabled_file"
chmod 0600 "$disabled_file"
read_crontab "$cron_actual"
if ! cmp -s "$cron_expected" "$cron_actual"; then
    printf 'legacy cutover: crontab verification failed\n' >&2
    exit 1
fi

legacy_script_for_dir() {
    local process_dir=$1
    local cwd
    local script=
    cwd=$(readlink -f -- "$process_dir/cwd" 2>/dev/null) || return 1
    [[ "$cwd" == "$legacy_root" ]] || return 1
    [[ -r "$process_dir/cmdline" ]] || return 1
    while IFS= read -r argument; do
        case "$argument" in
            Web.py|*/Web.py)
                script=Web.py
                ;;
            Main.py|*/Main.py)
                script=Main.py
                ;;
        esac
    done < <(tr '\000' '\n' <"$process_dir/cmdline")
    [[ -n "$script" ]] || return 1
    printf '%s\n' "$script"
}

shopt -s nullglob
process_entries=("$proc_root"/[0-9]*)
pids=()
scripts=()
: >"$processes_file"
chmod 0600 "$processes_file"
for process_dir in "${process_entries[@]}"; do
    [[ -d "$process_dir" ]] || continue
    pid=${process_dir##*/}
    [[ "$pid" =~ ^[0-9]+$ ]] || continue
    script_name=$(legacy_script_for_dir "$process_dir" 2>/dev/null) || continue
    command_line=$(tr '\000' ' ' <"$process_dir/cmdline" | tr '\n' ' ')
    printf 'pid=%s\tscript=%s\tcwd=%s\tcmdline=%s\n' \
        "$pid" "$script_name" "$legacy_root" "$command_line" >>"$processes_file"
    pids+=("$pid")
    scripts+=("$script_name")
done

stop_pid() {
    local pid=$1
    local expected_script=$2
    local process_dir="${proc_root}/$pid"
    local current_script
    current_script=$(legacy_script_for_dir "$process_dir" 2>/dev/null) || return 0
    [[ "$current_script" == "$expected_script" ]] || return 0
    if ! "$kill_bin" -TERM "$pid" >/dev/null 2>&1; then
        if "$kill_bin" -0 "$pid" >/dev/null 2>&1; then
            printf 'legacy cutover: failed to terminate PID %s\n' "$pid" >&2
            return 1
        fi
        return 0
    fi
    if [[ "$no_wait" == true ]]; then
        return 0
    fi
    for _ in {1..50}; do
        if ! "$kill_bin" -0 "$pid" >/dev/null 2>&1; then
            return 0
        fi
        sleep 0.1
    done
    if "$kill_bin" -0 "$pid" >/dev/null 2>&1; then
        "$kill_bin" -KILL "$pid" >/dev/null 2>&1 || {
            printf 'legacy cutover: failed to force terminate PID %s\n' "$pid" >&2
            return 1
        }
    fi
}

for index in "${!pids[@]}"; do
    stop_pid "${pids[$index]}" "${scripts[$index]}"
done

state_temporary="${state_file}.tmp.$$"
{
    printf 'version=1\n'
    printf 'cron_backup=%s\n' "$backup_file"
    printf 'disabled_crontab=%s\n' "$disabled_file"
    printf 'processes=%s\n' "$processes_file"
} >"$state_temporary"
chmod 0600 "$state_temporary"
mv -Tf "$state_temporary" "$state_file"
operation_ok=true
printf 'legacy cutover: OK (stopped %s matching processes)\n' "${#pids[@]}"
