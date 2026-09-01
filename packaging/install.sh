#!/usr/bin/env bash
set -euo pipefail

usage() {
    printf 'usage: %s --package DIR [--root DIR] [--version VERSION] [--no-systemd] [--skip-arch] [--skip-hardware]\n' "$0" >&2
}

package_dir=
root_dir=/
version=
no_systemd=false
skip_arch=false
skip_hardware=false
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
if [[ "$no_systemd" == false && "$root_prefix" != "/" ]]; then
    printf '%s\n' '--no-systemd is required when --root is not /' >&2
    exit 2
fi
install_root="${root_prefix}/opt/uhf-gateway"
releases_root="${install_root}/releases"
release_dir="${releases_root}/${version}"
temporary_release="${releases_root}/.${version}.tmp.$$"
current_link="${install_root}/current"
previous_link="${install_root}/previous"
state_dir="${root_prefix}/var/lib/uhf-gateway"
state_file="${state_dir}/release-state.json"
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
bash "${script_dir}/preflight.sh" "${preflight_args[@]}"

mkdir -p "$releases_root"
if [[ -e "$release_dir" || -L "$release_dir" || -e "$temporary_release" || -L "$temporary_release" ]]; then
    printf 'release already exists: %s\n' "$version" >&2
    exit 1
fi

legacy_cutover_done=false
cleanup() {
    if [[ "$legacy_cutover_done" == true ]]; then
        bash "${root_prefix}/usr/lib/uhf-gateway/legacy-recovery.sh" >/dev/null 2>&1 || true
    fi
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

mkdir -p "${root_prefix}/etc/uhf-gateway" \
    "${root_prefix}/etc/systemd/system" \
    "${root_prefix}/etc/rsyslog.d" \
    "${root_prefix}/etc/logrotate.d" \
    "${root_prefix}/usr/lib/uhf-gateway" \
    "$state_dir" \
    "${root_prefix}/var/lib/uhf-privileged" \
    "${root_prefix}/var/log/uhf-gateway"
chmod 0755 "${root_prefix}/etc/uhf-gateway" "${root_prefix}/usr/lib/uhf-gateway" \
    "$state_dir" "${root_prefix}/var/lib/uhf-privileged"
chmod 0700 "$state_dir"
if [[ "$no_systemd" == false ]]; then
    chown uhfgateway:uhfgateway "$state_dir"
fi
"$release_dir/bin/uhf-auth-init" --state-dir "$state_dir"
"$release_dir/bin/uhf-tls-init" --state-dir "$state_dir"
if [[ "$no_systemd" == false ]]; then
    chown uhfgateway:uhfgateway "$state_dir/auth.json"
    chown root:root "$state_dir/initial-password"
    chmod 0600 "$state_dir/initial-password"
    chown -R uhfgateway:uhfgateway "$state_dir/tls"
fi
install -m 0644 "$release_dir/config/schema.json" "${root_prefix}/etc/uhf-gateway/schema.json"
install -m 0644 "$release_dir/config/defaults.json" "${root_prefix}/etc/uhf-gateway/defaults.json"
network_config_file="${root_prefix}/etc/uhf-gateway/network.json"
if [[ -e "$network_config_file" && ! -f "$network_config_file" ]]; then
    printf 'network configuration path is not a regular file: %s\n' "$network_config_file" >&2
    exit 1
fi
if [[ ! -e "$network_config_file" ]]; then
    install -m 0600 "$release_dir/config/network.json" "$network_config_file"
fi
install -m 0644 "$release_dir/config/UHFPD1.icd" "${root_prefix}/etc/uhf-gateway/UHFPD1.icd"
install -m 0755 "$release_dir/libexec/uhf-gateway/uhf-gateway-hook" \
    "${root_prefix}/usr/lib/uhf-gateway/dhclient-hook"
install -m 0755 "$release_dir/libexec/uhf-gateway/release-guard.sh" \
    "${root_prefix}/usr/lib/uhf-gateway/release-guard.sh"
install -m 0755 "$release_dir/libexec/uhf-gateway/legacy-cutover.sh" \
    "${root_prefix}/usr/lib/uhf-gateway/legacy-cutover.sh"
install -m 0755 "$release_dir/libexec/uhf-gateway/legacy-recovery.sh" \
    "${root_prefix}/usr/lib/uhf-gateway/legacy-recovery.sh"
install -m 0644 "$release_dir/share/uhf-gateway/rsyslog/uhf-gateway.conf" \
    "${root_prefix}/etc/rsyslog.d/uhf-gateway.conf"
install -m 0644 "$release_dir/share/uhf-gateway/logrotate/uhf-gateway" \
    "${root_prefix}/etc/logrotate.d/uhf-gateway"
for unit in "$release_dir"/share/uhf-gateway/systemd/*; do
    install -m 0644 "$unit" "${root_prefix}/etc/systemd/system/$(basename "$unit")"
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
    local pending=$1
    local previous=
    if [[ -n "$old_current_target" ]]; then
        previous=$(basename "$old_current_target")
    fi
    local temporary="${state_file}.tmp.$$"
    printf '{"version":1,"current":"%s","previous":"%s","pending":"%s"}\n' \
        "$version" "$previous" "$pending" >"$temporary"
    chmod 0600 "$temporary"
    mv -Tf "$temporary" "$state_file"
}

write_state "$version"
if [[ "$no_systemd" == false ]]; then
    if [[ "$first_install" == true && -d "${root_prefix}/data" ]]; then
        bash "${root_prefix}/usr/lib/uhf-gateway/legacy-cutover.sh"
        legacy_cutover_done=true
        postflight_args=(--release "$release_dir")
        if [[ "$skip_arch" == true ]]; then
            postflight_args+=(--skip-arch)
        fi
        if [[ "$skip_hardware" == true ]]; then
            postflight_args+=(--skip-hardware)
        fi
        bash "${script_dir}/preflight.sh" "${postflight_args[@]}"
    fi
    systemctl daemon-reload
    systemctl enable --now uhf-network-recovery.service
    systemctl enable --now uhf-network-rollback.timer
    systemctl enable --now uhf-release-guard.timer
    systemctl enable --now uhf-privileged.service
    systemctl enable uhf-gateway.service
    systemctl restart uhf-gateway.service
    legacy_cutover_done=false
else
    write_state ""
fi
printf 'release installed: %s\n' "$release_dir"
