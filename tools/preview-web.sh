#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
web_dir="$(cd -- "${script_dir}/../web" && pwd)"
preview_host="${UHF_PREVIEW_HOST:-0.0.0.0}"
preview_port="${UHF_PREVIEW_PORT:-8080}"

if [[ ! -f "${web_dir}/index.html" ]]; then
    printf 'web preview assets are missing: %s\n' "${web_dir}" >&2
    exit 1
fi
if [[ ! "${preview_port}" =~ ^[0-9]+$ ]] ||
    (( preview_port < 1 || preview_port > 65535 )); then
    printf 'invalid UHF_PREVIEW_PORT: %s\n' "${preview_port}" >&2
    exit 1
fi

printf 'serving local UI preview at http://%s:%s/\n' \
    "${preview_host}" "${preview_port}"
printf 'this is a mock preview without authentication; press Ctrl-C to stop\n'
exec python3 -m http.server "${preview_port}" \
    --bind "${preview_host}" \
    --directory "${web_dir}"
