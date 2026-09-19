#!/usr/bin/env bash
set -euo pipefail

usage() {
    printf 'usage: %s --package DIR [--root DIR] [--version VERSION] [--no-systemd] [--skip-arch] [--skip-hardware] [--v3-runtime-env-file FILE --v3-device-id-file FILE --v3-manufacturer-key-file FILE --v3-activation-code-file FILE]\n' "$0" >&2
}

package_dir=
root_dir=/
version=
no_systemd=false
skip_arch=false
skip_hardware=false
v3_runtime_env_file=
v3_device_id_file=
v3_manufacturer_key_file=
v3_activation_code_file=
while [[ $# -gt 0 ]]; do
    case "$1" in
        --package)
            [[ $# -ge 2 ]] || { usage; exit 2; }
            package_dir=$2
            shift 2
            ;;
        --root)
            [[ $# -ge 2 ]] || { usage; exit 2; }
            root_dir=$2
            shift 2
            ;;
        --version)
            [[ $# -ge 2 ]] || { usage; exit 2; }
            version=$2
            shift 2
            ;;
        --no-systemd)
            no_systemd=true
            shift
            ;;
        --skip-arch)
            skip_arch=true
            shift
            ;;
        --skip-hardware)
            skip_hardware=true
            shift
            ;;
        --v3-runtime-env-file)
            [[ $# -ge 2 ]] || { usage; exit 2; }
            v3_runtime_env_file=$2
            shift 2
            ;;
        --v3-device-id-file)
            [[ $# -ge 2 ]] || { usage; exit 2; }
            v3_device_id_file=$2
            shift 2
            ;;
        --v3-manufacturer-key-file)
            [[ $# -ge 2 ]] || { usage; exit 2; }
            v3_manufacturer_key_file=$2
            shift 2
            ;;
        --v3-activation-code-file)
            [[ $# -ge 2 ]] || { usage; exit 2; }
            v3_activation_code_file=$2
            shift 2
            ;;
        *)
            usage
            exit 2
            ;;
    esac
done

if [[ -z "$package_dir" || ! -d "$package_dir" ]]; then
    usage
    exit 2
fi

release_metadata="${package_dir}/RELEASE"
if [[ -z "$version" && -f "$release_metadata" ]]; then
    while IFS= read -r line; do
        if [[ "$line" == version=* ]]; then
            version=${line#version=}
            break
        fi
    done <"$release_metadata"
fi
if [[ -z "$version" || ! "$version" =~ ^[A-Za-z0-9][A-Za-z0-9._-]{0,47}$ ]]; then
    printf 'invalid or missing release version\n' >&2
    exit 2
fi

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
root_prefix=${root_dir%/}
if [[ -z "$root_prefix" ]]; then
    root_prefix=/
fi
path_prefix=$root_prefix
if [[ "$path_prefix" == "/" ]]; then
    path_prefix=
fi
if [[ "$no_systemd" == false && "$root_prefix" != "/" ]]; then
    printf '%s\n' '--no-systemd is required when --root is not /' >&2
    exit 2
fi
install_root="${path_prefix}/opt/uhf-gateway"
releases_root="${install_root}/releases"
release_dir="${releases_root}/${version}"
temporary_release="${releases_root}/.${version}.tmp.$$"
current_link="${install_root}/current"
previous_link="${install_root}/previous"
state_dir="${path_prefix}/var/lib/uhf-gateway"
state_file="${state_dir}/release-state.json"
v3_runtime_env_target="${path_prefix}/etc/uhf-gateway/v3-runtime.env"
v3_device_id_target="${path_prefix}/etc/uhf-gateway/v3-device-id"
v3_manufacturer_key_target="${path_prefix}/etc/uhf-gateway/v3-manufacturer-key"
v3_activation_code_target="${path_prefix}/etc/uhf-gateway/v3-activation-code"

validate_provisioning_file() {
    local label=$1
    local path=$2
    local kind=$3
    local bytes=
    if [[ ! -f "$path" || -L "$path" ]]; then
        printf '%s must be a regular non-link file: %s\n' "$label" "$path" >&2
        exit 2
    fi
    bytes=$(wc -c <"$path")
    if [[ ! "$bytes" =~ ^[0-9]+$ || "$bytes" -lt 1 || "$bytes" -gt 4096 ]]; then
        printf '%s must contain 1..4096 bytes: %s\n' "$label" "$path" >&2
        exit 2
    fi
    if [[ "$kind" == runtime ]]; then
        local variable=
        local matches=
        for variable in UHF_V3_PD_DEVICE UHF_V3_CURRENT_DEVICE UHF_V3_TEMPERATURE_DEVICE; do
            matches=$(grep -Ec "^${variable}=/[^[:space:]]+$" "$path" || true)
            if [[ "$matches" != 1 ]]; then
                printf '%s must define %s exactly once as an absolute path\n' "$label" "$variable" >&2
                exit 2
            fi
        done
        if grep -Evq '^(#.*|[[:space:]]*|UHF_V3_(PD|CURRENT|TEMPERATURE)_DEVICE=/[A-Za-z0-9._/:@+-]+)$' "$path"; then
            printf '%s contains a placeholder, unsupported variable, or unsafe path character\n' "$label" >&2
            exit 2
        fi
        if grep -q 'REPLACE_' "$path"; then
            printf '%s still contains an unconfigured REPLACE_ placeholder\n' "$label" >&2
            exit 2
        fi
    else
        local lines=
        lines=$(awk 'END { print NR }' "$path")
        if [[ "$bytes" -gt 258 || "$lines" != 1 ]] ||
           ! grep -Eq '[^[:space:]]' "$path"; then
            printf '%s must contain one value of at most 256 bytes plus a line ending\n' "$label" >&2
            exit 2
        fi
    fi
}

require_or_validate_provisioning() {
    local label=$1
    local source=$2
    local target=$3
    local kind=$4
    if [[ (-e "$target" || -L "$target") && (! -f "$target" || -L "$target") ]]; then
        printf '%s target must be a regular non-link file: %s\n' "$label" "$target" >&2
        exit 2
    fi
    if [[ -n "$source" ]]; then
        validate_provisioning_file "$label" "$source" "$kind"
    else
        if [[ ! -e "$target" ]]; then
            printf '%s is required before enabling the v3 service; use the matching installer option\n' "$label" >&2
            exit 2
        fi
        validate_provisioning_file "$label" "$target" "$kind"
    fi
}

require_or_validate_provisioning "v3 runtime environment" "$v3_runtime_env_file" "$v3_runtime_env_target" runtime
require_or_validate_provisioning "v3 device identity" "$v3_device_id_file" "$v3_device_id_target" credential
require_or_validate_provisioning "v3 manufacturer key" "$v3_manufacturer_key_file" "$v3_manufacturer_key_target" credential
require_or_validate_provisioning "v3 activation code" "$v3_activation_code_file" "$v3_activation_code_target" credential

old_current_target=
if [[ -L "$current_link" ]]; then
    old_current_target=$(readlink -f "$current_link")
    if [[ ! "$old_current_target" == "${releases_root}/"* || ! -d "$old_current_target" ]]; then
        printf 'current link is outside the managed release root\n' >&2
        exit 1
    fi
elif [[ -e "$current_link" ]]; then
    printf 'current path is not a symbolic link\n' >&2
    exit 1
fi
if [[ -e "$previous_link" && ! -L "$previous_link" ]]; then
    printf 'previous path is not a symbolic link\n' >&2
    exit 1
fi

ensure_runtime_account() {
    if ! getent group uhfgateway >/dev/null 2>&1; then
        groupadd --system uhfgateway
    fi
    if ! id -u uhfgateway >/dev/null 2>&1; then
        local shell_path=/usr/sbin/nologin
        if [[ ! -x "$shell_path" ]]; then
            shell_path=/bin/false
        fi
        useradd --system --gid uhfgateway --home-dir /nonexistent --shell "$shell_path" uhfgateway
    fi
}

if [[ "$no_systemd" == false ]]; then
    ensure_runtime_account
fi
first_install=false
if [[ -z "$old_current_target" ]]; then
    first_install=true
fi
preflight_args=(--release "$package_dir")
if [[ "$skip_arch" == true ]]; then
    preflight_args+=(--skip-arch)
fi
if [[ "$skip_hardware" == true ]]; then
    preflight_args+=(--skip-hardware)
fi
if [[ "$first_install" == true && "$no_systemd" == false ]]; then
    preflight_args+=(--allow-legacy)
fi
if [[ "$first_install" == false && "$no_systemd" == false ]]; then
    preflight_args+=(--allow-current-product)
fi
bash "${script_dir}/preflight.sh" "${preflight_args[@]}"

mkdir -p "$releases_root"
if [[ -e "$release_dir" || -L "$release_dir" || -e "$temporary_release" || -L "$temporary_release" ]]; then
    printf 'release already exists: %s\n' "$version" >&2
    exit 1
fi

cleanup() {
    if [[ -d "$temporary_release" ]]; then
        rm -rf -- "$temporary_release"
    fi
}
trap cleanup EXIT
mkdir "$temporary_release"
cp -a "${package_dir}/." "$temporary_release/"
preflight_copy_args=(--release "$temporary_release" --skip-hardware)
if [[ "$skip_arch" == true ]]; then
    preflight_copy_args+=(--skip-arch)
fi
bash "${script_dir}/preflight.sh" "${preflight_copy_args[@]}"
mv -T "$temporary_release" "$release_dir"

mkdir -p "${path_prefix}/etc/uhf-gateway" \
    "${path_prefix}/etc/systemd/system" \
    "${path_prefix}/etc/systemd/system/uhf-gateway.service.d" \
    "${path_prefix}/etc/systemd/timesyncd.conf.d" \
    "${path_prefix}/etc/rsyslog.d" \
    "${path_prefix}/etc/logrotate.d" \
    "${path_prefix}/usr/lib/uhf-gateway" \
    "$state_dir" \
    "${path_prefix}/var/lib/uhf-privileged" \
    "${path_prefix}/var/log/uhf-gateway"
chmod 0755 "${path_prefix}/etc/uhf-gateway" "${path_prefix}/usr/lib/uhf-gateway" \
    "$state_dir" "${path_prefix}/var/lib/uhf-privileged"
chmod 0700 "$state_dir"
if [[ "$no_systemd" == false ]]; then
    chown uhfgateway:uhfgateway "$state_dir"
    # rsyslog normally drops privileges to the syslog account before opening
    # the product log.  Keep the directory private and pre-create the active
    # file so rsyslog never needs write access to the directory itself.
    if getent passwd syslog >/dev/null 2>&1; then
        syslog_group=$(id -gn syslog)
        chown "root:${syslog_group}" "${path_prefix}/var/log/uhf-gateway"
        chmod 0750 "${path_prefix}/var/log/uhf-gateway"
        log_file="${path_prefix}/var/log/uhf-gateway/gateway.log"
        if [[ -e "$log_file" ]]; then
            if [[ ! -f "$log_file" || -L "$log_file" ]]; then
                printf 'product log path is not a regular file: %s\n' "$log_file" >&2
                exit 1
            fi
        else
            install -o syslog -g "$syslog_group" -m 0640 /dev/null "$log_file"
        fi
        chown "syslog:${syslog_group}" "$log_file"
        chmod 0640 "$log_file"
    fi
fi

install_provisioning_file() {
    local source=$1
    local target=$2
    if [[ (-e "$target" || -L "$target") && (! -f "$target" || -L "$target") ]]; then
        printf 'v3 provisioning target must be a regular non-link file: %s\n' "$target" >&2
        exit 1
    fi
    if [[ -n "$source" && (! -e "$target" || ! "$source" -ef "$target") ]]; then
        install -m 0600 "$source" "$target"
    fi
    if [[ -e "$target" ]]; then
        if [[ "$no_systemd" == false ]]; then
            chown root:uhfgateway "$target"
            chmod 0640 "$target"
        else
            chmod 0600 "$target"
        fi
    fi
}

install_provisioning_file "$v3_runtime_env_file" "$v3_runtime_env_target"
install_provisioning_file "$v3_device_id_file" "$v3_device_id_target"
install_provisioning_file "$v3_manufacturer_key_file" "$v3_manufacturer_key_target"
install_provisioning_file "$v3_activation_code_file" "$v3_activation_code_target"

write_v3_device_policy() {
    if [[ ! -f "$v3_runtime_env_target" ]]; then
        return
    fi
    local policy="${path_prefix}/etc/systemd/system/uhf-gateway.service.d/v3-devices.conf"
    local temporary="${policy}.tmp.$$"
    local variable=
    local device=
    printf '[Service]\n' >"$temporary"
    for variable in UHF_V3_PD_DEVICE UHF_V3_CURRENT_DEVICE UHF_V3_TEMPERATURE_DEVICE; do
        device=$(grep -E "^${variable}=" "$v3_runtime_env_target")
        device=${device#*=}
        printf 'DeviceAllow=%s rw\n' "$device" >>"$temporary"
    done
    chmod 0644 "$temporary"
    mv -Tf "$temporary" "$policy"
}

write_v3_device_policy

"$release_dir/bin/uhf-auth-init" --state-dir "$state_dir"
"$release_dir/bin/uhf-tls-init" --state-dir "$state_dir"
if [[ "$no_systemd" == false ]]; then
    chown uhfgateway:uhfgateway "$state_dir/auth.json"
    chown root:root "$state_dir/initial-password"
    chmod 0600 "$state_dir/initial-password"
    chown -R uhfgateway:uhfgateway "$state_dir/tls"
fi
install -m 0644 "$release_dir/config/schema.json" "${path_prefix}/etc/uhf-gateway/schema.json"
install -m 0644 "$release_dir/config/defaults.json" "${path_prefix}/etc/uhf-gateway/defaults.json"
network_config_file="${path_prefix}/etc/uhf-gateway/network.json"
if [[ -e "$network_config_file" && ! -f "$network_config_file" ]]; then
    printf 'network configuration path is not a regular file: %s\n' "$network_config_file" >&2
    exit 1
fi
if [[ ! -e "$network_config_file" ]]; then
    install -m 0600 "$release_dir/config/network.json" "$network_config_file"
fi
install -m 0644 "$release_dir/config/UHFPD1.icd" "${path_prefix}/etc/uhf-gateway/UHFPD1.icd"
install -m 0755 "$release_dir/libexec/uhf-gateway/uhf-gateway-hook" \
    "${path_prefix}/usr/lib/uhf-gateway/dhclient-hook"
install -m 0755 "$release_dir/libexec/uhf-gateway/legacy-cutover.sh" \
    "${path_prefix}/usr/lib/uhf-gateway/legacy-cutover.sh"
install -m 0644 "$release_dir/share/uhf-gateway/rsyslog/uhf-gateway.conf" \
    "${path_prefix}/etc/rsyslog.d/uhf-gateway.conf"
install -m 0644 "$release_dir/share/uhf-gateway/logrotate/uhf-gateway" \
    "${path_prefix}/etc/logrotate.d/uhf-gateway"
for unit in "$release_dir"/share/uhf-gateway/systemd/*; do
    install -m 0644 "$unit" "${path_prefix}/etc/systemd/system/$(basename "$unit")"
done

atomic_link() {
    local target=$1
    local link=$2
    local temporary="${link}.tmp.$$"
    if [[ -e "$temporary" || -L "$temporary" ]]; then
        printf 'temporary link already exists: %s\n' "$temporary" >&2
        return 1
    fi
    ln -s "$target" "$temporary"
    mv -Tf "$temporary" "$link"
}

if [[ -n "$old_current_target" ]]; then
    atomic_link "$old_current_target" "$previous_link"
fi
atomic_link "$release_dir" "$current_link"

write_state() {
    local previous=
    if [[ -n "$old_current_target" ]]; then
        previous=$(basename "$old_current_target")
    fi
    local temporary="${state_file}.tmp.$$"
    printf '{"version":1,"current":"%s","previous":"%s","pending":""}\n' \
        "$version" "$previous" >"$temporary"
    chmod 0600 "$temporary"
    mv -Tf "$temporary" "$state_file"
}

restart_release_service() {
    local unit=$1
    local binary=$2
    local expected_executable=
    local main_pid=
    local actual_executable=
    local attempt=
    expected_executable=$(readlink -f "${release_dir}/bin/${binary}" 2>/dev/null || true)
    if [[ -z "$expected_executable" ]]; then
        printf 'installed executable is missing: %s\n' "${release_dir}/bin/${binary}" >&2
        exit 1
    fi
    systemctl restart "$unit"
    for attempt in 1 2 3 4 5; do
        main_pid=$(systemctl show "$unit" -p MainPID --value 2>/dev/null || true)
        if [[ "$main_pid" =~ ^[1-9][0-9]*$ ]]; then
            actual_executable=$(readlink -f "/proc/${main_pid}/exe" 2>/dev/null || true)
            if [[ "$actual_executable" == "$expected_executable" ]]; then
                return 0
            fi
        fi
        sleep 1
    done
    systemctl status "$unit" --no-pager --lines=20 >&2 || true
    printf 'service %s is not running the installed release executable: expected %s, got %s (pid %s)\n' \
        "$unit" "$expected_executable" "${actual_executable:-unknown}" "${main_pid:-unknown}" >&2
    exit 1
}

write_state
if [[ "$no_systemd" == false ]]; then
    if [[ "$first_install" == true && -d "${path_prefix}/data" ]]; then
        bash "${path_prefix}/usr/lib/uhf-gateway/legacy-cutover.sh"
        postflight_args=(--release "$release_dir")
        if [[ "$skip_arch" == true ]]; then
            postflight_args+=(--skip-arch)
        fi
        if [[ "$skip_hardware" == true ]]; then
            postflight_args+=(--skip-hardware)
        fi
        bash "${script_dir}/preflight.sh" "${postflight_args[@]}"
    fi
    systemctl disable --now uhf-release-guard.timer uhf-release-guard.service >/dev/null 2>&1 || true
    systemctl disable --now uhf-legacy-recovery.service >/dev/null 2>&1 || true
    rm -f -- \
        "${path_prefix}/etc/systemd/system/uhf-release-guard.service" \
        "${path_prefix}/etc/systemd/system/uhf-release-guard.timer" \
        "${path_prefix}/usr/lib/uhf-gateway/release-guard.sh" \
        "${path_prefix}/etc/systemd/system/uhf-legacy-recovery.service" \
        "${path_prefix}/usr/lib/uhf-gateway/legacy-recovery.sh"
    rm -f -- "${path_prefix}/var/lib/uhf-gateway/legacy/recovery-enabled"
    systemctl daemon-reload
    systemctl enable --now uhf-network-recovery.service
    systemctl enable --now uhf-network-rollback.timer
    systemctl enable uhf-privileged.service
    restart_release_service uhf-privileged.service uhf-privilegedd
    systemctl enable uhf-gateway.service
    restart_release_service uhf-gateway.service uhf-gatewayd
else
    write_state
fi
printf 'release installed: %s\n' "$release_dir"
