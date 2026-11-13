#!/usr/bin/env bash

set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd -- "${script_dir}/.." && pwd)"
build_dir="${repo_root}/build/release-macos"
dist_dir="${repo_root}/dist"
staging_root="${dist_dir}/staging"
architecture="$(uname -m)"
package_name="lanlink-macos-${architecture}"
package_dir="${staging_root}/${package_name}"
archive="${dist_dir}/${package_name}.tar.gz"

if [[ "${architecture}" != "arm64" && "${architecture}" != "x86_64" ]]; then
  printf 'Unsupported macOS architecture: %s\n' "${architecture}" >&2
  exit 1
fi

rm -rf "${build_dir}"
cmake -S "${repo_root}" \
  -B "${build_dir}" \
  -G "Unix Makefiles" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CXX_EXTENSIONS=OFF \
  -DLANLINK_ENABLE_TLS=OFF
cmake --build "${build_dir}" --parallel
ctest --test-dir "${build_dir}" --repeat until-pass:10 --output-on-failure

rm -rf "${package_dir}"
rm -f "${archive}"
mkdir -p "${package_dir}"
install -m 755 "${build_dir}/lanlink_app" "${package_dir}/lanlink_app"
cp "${repo_root}/README.md" "${package_dir}/README.md"

for document in LICENSE LICENSE.md; do
  if [[ -f "${repo_root}/${document}" ]]; then
    cp "${repo_root}/${document}" "${package_dir}/${document}"
  fi
done

tar -czf "${archive}" -C "${staging_root}" "${package_name}"
printf 'Created %s\n' "${archive}"
