#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
repo_dir="$(cd -- "${script_dir}/.." && pwd)"
build_root="${UHF_LOCAL_BUILD_ROOT:-${repo_dir}/build}"
host_build="${build_root}/host"
aarch64_build="${build_root}/aarch64"
toolchain_file="${repo_dir}/cmake/toolchains/aarch64-linux-gnu.cmake"

cmake -S "${repo_dir}" -B "${host_build}" \
    -DCMAKE_BUILD_TYPE=Debug \
    -DBUILD_TESTING=ON \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build "${host_build}"
ctest --test-dir "${host_build}" --output-on-failure

cmake -S "${repo_dir}" -B "${aarch64_build}" \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_TESTING=OFF \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
    -DCMAKE_TOOLCHAIN_FILE="${toolchain_file}"
cmake --build "${aarch64_build}"

machine="$(readelf -h "${aarch64_build}/uhf-gatewayd" |
    awk '$1 == "Machine:" { print $2; exit }')"
if [[ "${machine}" != "AArch64" ]]; then
    printf 'unexpected cross-build machine: %s\n' "${machine}" >&2
    exit 1
fi

printf 'local build complete: host tests passed; AArch64 artifact verified\n'
