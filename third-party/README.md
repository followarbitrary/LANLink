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

Qt 6 and OpenSSL are intentionally not copied into this repository:

- Qt 6 must match the target compiler and architecture and includes platform
  deployment tools such as `windeployqt` or `macdeployqt`.
- OpenSSL is an optional TLS backend and is discovered through CMake's package
  configuration.

Use a pinned Qt/OpenSSL SDK or toolchain on CI and pass its CMake prefix path
when a desktop or TLS build is required. Release packaging disables the TLS
backend and can produce a CLI-only archive when Qt 6 is unavailable.
