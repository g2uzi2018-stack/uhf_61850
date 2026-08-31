#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
repo_dir="$(cd -- "${script_dir}/.." && pwd)"
build_root="${UHF_LOCAL_BUILD_ROOT:-${repo_dir}/build}"
host_build="${build_root}/host"
web_listen="${UHF_WEB_LISTEN:-0.0.0.0:8080}"

cmake -S "${repo_dir}" -B "${host_build}" \
    -DCMAKE_BUILD_TYPE=Debug \
    -DBUILD_TESTING=ON
cmake --build "${host_build}"

exec "${host_build}/uhf-gatewayd" \
    --web \
    --web-root "${repo_dir}/web" \
    --listen "${web_listen}"
