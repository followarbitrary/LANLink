#!/usr/bin/env python3
"""Validate the repository contract used by the Windows package script."""

import json
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def main() -> None:
    assert not (ROOT / "vcpkg.json").exists()

    bootstrap = json.loads(
        (ROOT / "third-party" / "vcpkg-bootstrap.json").read_text(encoding="utf-8")
    )
    assert bootstrap == {
        "commit": "ca2f11c400a70cd0295b317c96135663b6f2063e",
        "archive_url": "https://github.com/microsoft/vcpkg/archive/ca2f11c400a70cd0295b317c96135663b6f2063e.zip",
        "sha256": "d32fb34b522fbbd87dd97f3c74405a3ca7c4767f3b128397e2616daef0989d3b",
    }

    cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
    assert "LANLINK_REQUIRE_TLS" in cmake

    script = (ROOT / "scripts" / "package-windows.ps1").read_text(encoding="utf-8")
    for required in (
        "Get-FileHash",
        "Invoke-WebRequest",
        "qtbase[gui,network,testlib,widgets]",
        '"qtsvg"',
        '"openssl"',
        "VCPKG_TARGET_TRIPLET=x64-windows",
        "VCPKG_INSTALLED_DIR",
        "$installedTriplet = Join-Path $vcpkgInstalled \"x64-windows\"",
        "CMAKE_PREFIX_PATH=$installedTriplet",
        "VCPKG_BINARY_SOURCES",
        "VCPKG_DOWNLOADS",
        "VCPKG_MANIFEST_MODE=OFF",
        "VCPKG_MANIFEST_INSTALL=OFF",
        "MultiThreadedDLL",
        "windeployqt.exe",
        "libssl-*.dll",
        "libcrypto-*.dll",
        "$sslDlls = @(Get-ChildItem",
        "$cryptoDlls = @(Get-ChildItem",
        "LANLINK_ENABLE_TLS=ON",
        "LANLINK_REQUIRE_TLS=ON",
    ):
        assert required in script, required
    for forbidden in (
        "$env:QTDIR",
        "qmake.exe",
        "LANLINK_ENABLE_TLS=OFF",
        "Ensure-VcpkgGitRepository",
        "VCPKG_MANIFEST_MODE=ON",
        "--x-manifest-root=",
        "git.exe",
    ):
        assert forbidden not in script, forbidden


if __name__ == "__main__":
    main()
