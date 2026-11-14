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
dmg_staging="${dist_dir}/dmg/${package_name}"
dmg="${dist_dir}/${package_name}.dmg"
qt_prefix=""

if [[ "${architecture}" != "arm64" && "${architecture}" != "x86_64" ]]; then
  printf 'Unsupported macOS architecture: %s\n' "${architecture}" >&2
  exit 1
fi

if command -v qmake6 >/dev/null 2>&1 && command -v macdeployqt >/dev/null 2>&1; then
  qt_prefix="$(qmake6 -query QT_INSTALL_PREFIX 2>/dev/null || true)"
fi

cmake_args=(
  -S "${repo_root}"
  -B "${build_dir}"
  -G "Unix Makefiles"
  -DCMAKE_BUILD_TYPE=Release
  -DCMAKE_CXX_EXTENSIONS=OFF
  -DLANLINK_ENABLE_TLS=OFF
)
if [[ -n "${qt_prefix}" && -d "${qt_prefix}/lib/cmake/Qt6" ]]; then
  cmake_args+=("-DCMAKE_PREFIX_PATH=${qt_prefix}")
  printf 'Qt6 detected at %s; the desktop app will be packaged.\n' "${qt_prefix}"
else
  printf 'Qt6 was not detected; packaging the CLI only.\n'
fi

rm -rf "${build_dir}"
cmake "${cmake_args[@]}"
cmake --build "${build_dir}" --parallel
ctest --test-dir "${build_dir}" --repeat until-pass:10 --output-on-failure

rm -rf "${package_dir}"
rm -f "${archive}"
mkdir -p "${package_dir}"
install -m 755 "${build_dir}/lanlink_app" "${package_dir}/lanlink_app"
if [[ -d "${build_dir}/lanlink_desktop.app" ]]; then
  if ! command -v macdeployqt >/dev/null 2>&1; then
    printf 'Qt6 desktop app was built, but macdeployqt is unavailable.\n' >&2
    exit 1
  fi
  macdeployqt "${build_dir}/lanlink_desktop.app" -always-overwrite
  # macdeployqt rewrites nested Qt library paths, which invalidates any
  # signatures embedded in those libraries. Re-sign the complete bundle so
  # macOS can launch it directly from the DMG.
  codesign --force --deep --sign - "${build_dir}/lanlink_desktop.app"
  ditto "${build_dir}/lanlink_desktop.app" "${package_dir}/lanlink_desktop.app"
elif [[ -x "${build_dir}/lanlink_desktop" ]]; then
  install -m 755 "${build_dir}/lanlink_desktop" "${package_dir}/lanlink_desktop"
fi
cp "${repo_root}/README.md" "${package_dir}/README.md"

for document in LICENSE LICENSE.md; do
  if [[ -f "${repo_root}/${document}" ]]; then
    cp "${repo_root}/${document}" "${package_dir}/${document}"
  fi
done

tar -czf "${archive}" -C "${staging_root}" "${package_name}"
printf 'Created %s\n' "${archive}"

if [[ -d "${package_dir}/lanlink_desktop.app" ]]; then
  rm -rf "${dmg_staging}" "${dmg}"
  mkdir -p "${dmg_staging}"
  ditto "${package_dir}/lanlink_desktop.app" "${dmg_staging}/LANLink.app"
  ln -s /Applications "${dmg_staging}/Applications"
  cp "${package_dir}/README.md" "${dmg_staging}/README.md"
  hdiutil create \
    -volname "LANLink" \
    -srcfolder "${dmg_staging}" \
    -ov \
    -format UDZO \
    "${dmg}"
  printf 'Created %s\n' "${dmg}"
else
  printf 'Qt6 desktop app is unavailable; skipping DMG creation.\n'
fi
