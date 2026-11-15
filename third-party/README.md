# Vendored third-party dependencies

This directory contains source dependencies whose versions must stay stable
across clean machines and offline builds.

## GoogleTest

`googletest/` is GoogleTest **1.14.0**, used by the project's CTest targets.
It is distributed under the BSD 3-Clause license; see
[`googletest/LICENSE`](googletest/LICENSE).

The top-level CMake project builds this copy directly and does not download
GoogleTest during configuration.

## Platform SDK dependencies

The Windows packaging flow pins and bootstraps Qt 6 and OpenSSL through the
repository-level [`vcpkg.json`](../vcpkg.json). It downloads the vcpkg source
only at build time when the verified checkout is absent, installs the pinned
`x64-windows` graph into the ignored `third-party/vcpkg_installed` directory,
and uses that prefix for both CMake discovery and runtime deployment. Source
downloads and binary archives are also kept under `third-party/.cache`.

The SDK binaries are intentionally not copied into this repository:

- Qt 6 must match the target compiler and architecture and includes platform
  deployment tools such as `windeployqt` or `macdeployqt`.
- OpenSSL remains optional for local builds that explicitly disable TLS; the
  Windows release script requires the pinned package and enables TLS.

The first Windows packaging run needs network access unless the vcpkg archive
and package binary caches are already present. A checksum mismatch, failed
bootstrap, or failed package install stops packaging; the script does not fall
back to a machine-global Qt or OpenSSL installation. Git for Windows is also
required during the first bootstrap so vcpkg can materialize its versioned
port trees into the repository-local checkout; subsequent runs reuse that
checkout without re-fetching the registry objects.
