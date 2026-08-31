#!/usr/bin/env bash
set -euo pipefail

usage() {
    printf 'usage: %s --build-dir DIR --version VERSION --output DIR\n' "$0" >&2
}

build_dir=
version=
output_dir=
while [[ $# -gt 0 ]]; do
    case "$1" in
        --build-dir)
            [[ $# -ge 2 ]] || { usage; exit 2; }
            build_dir=$2
            shift 2
            ;;
        --version)
            [[ $# -ge 2 ]] || { usage; exit 2; }
            version=$2
            shift 2
            ;;
        --output)
            [[ $# -ge 2 ]] || { usage; exit 2; }
            output_dir=$2
            shift 2
            ;;
        *)
            usage
            exit 2
            ;;
    esac
done

if [[ -z "$build_dir" || -z "$version" || -z "$output_dir" ||
      ! -f "$build_dir/cmake_install.cmake" ]]; then
    usage
    exit 2
fi
if [[ ! "$version" =~ ^[A-Za-z0-9][A-Za-z0-9._-]{0,47}$ ]]; then
    printf 'invalid release version\n' >&2
    exit 2
fi

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
repo_dir=$(cd -- "${script_dir}/.." && pwd)
source_commit=$(git -C "$repo_dir" rev-parse --short=12 HEAD 2>/dev/null || printf 'unknown')
temporary_root=$(mktemp -d "${TMPDIR:-/tmp}/uhf-release.XXXXXX")
trap 'rm -rf -- "$temporary_root"' EXIT
stage_dir="${temporary_root}/stage"
cmake --install "$build_dir" --prefix "$stage_dir"

mkdir -p "$output_dir"
release_name="uhf-gateway-${version}"
release_dir="${output_dir}/${release_name}"
temporary_release="${output_dir}/.${release_name}.tmp"
archive_path="${output_dir}/${release_name}.tar.gz"
if [[ -e "$release_dir" || -e "$archive_path" || -e "$temporary_release" ]]; then
    printf 'release output already exists: %s\n' "$release_name" >&2
    exit 1
fi
mkdir "$temporary_release"
cp -a "${stage_dir}/." "$temporary_release/"
cp -p "${repo_dir}/packaging/install.sh" "${temporary_release}/install.sh"
cp -p "${repo_dir}/packaging/preflight.sh" "${temporary_release}/preflight.sh"
cat >"${temporary_release}/RELEASE" <<EOF
product=uhf-gateway
version=${version}
source_commit=${source_commit}
EOF
mv -T "$temporary_release" "$release_dir"
tar -C "$output_dir" -czf "$archive_path" "$release_name"
printf 'release created: %s\n' "$release_dir"
