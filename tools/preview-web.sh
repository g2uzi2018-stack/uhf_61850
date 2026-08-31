#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
preview_host="${UHF_PREVIEW_HOST:-0.0.0.0}"
preview_port="${UHF_PREVIEW_PORT:-8080}"

if [[ ! "${preview_port}" =~ ^[0-9]+$ ]] ||
    (( preview_port < 1 || preview_port > 65535 )); then
    printf 'invalid UHF_PREVIEW_PORT: %s\n' "${preview_port}" >&2
    exit 2
fi

printf 'starting the authenticated C++ development service at http://%s:%s/\n' \
    "${preview_host}" "${preview_port}"
UHF_WEB_LISTEN="${preview_host}:${preview_port}" \
    exec bash "${script_dir}/run-web-local.sh"
