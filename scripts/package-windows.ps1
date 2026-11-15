[CmdletBinding()]
param(
    [ValidateSet("Release")]
    [string]$Configuration = "Release"
)

$ErrorActionPreference = "Stop"

function Invoke-Checked {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Command,
        [Parameter(Mandatory = $true)]
        [string[]]$Arguments
    )

    & $Command @Arguments | ForEach-Object { Write-Host $_ }
    if ($LASTEXITCODE -ne 0) {
        throw "$Command failed with exit code $LASTEXITCODE"
    }
}

function Read-VcpkgBootstrapConfig {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    if (-not (Test-Path -LiteralPath $Path)) {
        throw "Missing vcpkg bootstrap metadata: $Path"
    }

    $config = Get-Content -LiteralPath $Path -Raw | ConvertFrom-Json
    foreach ($property in @("commit", "archive_url", "sha256")) {
        if ([string]::IsNullOrWhiteSpace([string]$config.$property)) {
            throw "vcpkg bootstrap metadata is missing '$property'"
        }
    }
    if ($config.commit -notmatch "^[0-9a-f]{40}$") {
        throw "vcpkg bootstrap commit is not a 40-character SHA"
    }
    if ($config.sha256 -notmatch "^[0-9a-fA-F]{64}$") {
        throw "vcpkg bootstrap SHA-256 is not a 64-character hex value"
    }
    return $config
}

function Ensure-Vcpkg {
    param(
        [Parameter(Mandatory = $true)]
        [string]$CacheRoot,
        [Parameter(Mandatory = $true)]
        [psobject]$BootstrapConfig
    )

    New-Item -ItemType Directory -Force -Path $CacheRoot | Out-Null
    $sourceRoot = Join-Path $CacheRoot "vcpkg-$($BootstrapConfig.commit)"
    $archivePath = Join-Path $CacheRoot "vcpkg-$($BootstrapConfig.commit).zip"
    $vcpkgExe = Join-Path $sourceRoot "vcpkg.exe"
    $vcpkgToolchainInSource = Join-Path $sourceRoot "scripts\buildsystems\vcpkg.cmake"
    $bootstrapScript = Join-Path $sourceRoot "bootstrap-vcpkg.bat"

    if (-not (Test-Path -LiteralPath $archivePath)) {
        Write-Host "Downloading pinned vcpkg $($BootstrapConfig.commit)..."
        try {
            Invoke-WebRequest -Uri $BootstrapConfig.archive_url -OutFile $archivePath -UseBasicParsing
        } catch {
            throw "Unable to download pinned vcpkg archive from $($BootstrapConfig.archive_url): $($_.Exception.Message)"
        }
    }

    $actualHash = (Get-FileHash -LiteralPath $archivePath -Algorithm SHA256).Hash.ToLowerInvariant()
    $expectedHash = $BootstrapConfig.sha256.ToLowerInvariant()
    if ($actualHash -ne $expectedHash) {
        throw "Pinned vcpkg archive SHA-256 mismatch. Expected $expectedHash, got $actualHash. Delete $archivePath and retry."
    }

    $needsBootstrap = (-not (Test-Path -LiteralPath $vcpkgExe)) -or
        (-not (Test-Path -LiteralPath $vcpkgToolchainInSource)) -or
        (-not (Test-Path -LiteralPath $bootstrapScript))
    if ($needsBootstrap) {
        if (Test-Path -LiteralPath $sourceRoot) {
            Remove-Item -LiteralPath $sourceRoot -Recurse -Force
        }
        Expand-Archive -LiteralPath $archivePath -DestinationPath $CacheRoot -Force
        if (-not (Test-Path -LiteralPath $sourceRoot)) {
            throw "Pinned vcpkg archive did not extract to $sourceRoot"
        }
    }

    if ($needsBootstrap) {
        Push-Location $sourceRoot
        try {
            Invoke-Checked $bootstrapScript @("-disableMetrics")
        } finally {
            Pop-Location
        }
    }

    if (-not (Test-Path -LiteralPath $vcpkgExe)) {
        throw "Pinned vcpkg bootstrap did not produce $vcpkgExe"
    }
    return $vcpkgExe
}

function Copy-OpenSslRuntime {
    param(
        [Parameter(Mandatory = $true)]
        [string]$InstalledDir,
        [Parameter(Mandatory = $true)]
        [string]$PackageDir
    )

    $binDir = Join-Path $InstalledDir "bin"
    $sslDlls = @(Get-ChildItem -LiteralPath $binDir -Filter "libssl-*.dll" -File -ErrorAction SilentlyContinue)
    $cryptoDlls = @(Get-ChildItem -LiteralPath $binDir -Filter "libcrypto-*.dll" -File -ErrorAction SilentlyContinue)
    if ($sslDlls.Count -lt 1 -or $cryptoDlls.Count -lt 1) {
        throw "Pinned OpenSSL runtime DLLs were not found under $binDir"
    }
    $runtimeDlls = @($sslDlls) + @($cryptoDlls)

    foreach ($runtimeDll in $runtimeDlls) {
        Copy-Item -LiteralPath $runtimeDll.FullName -Destination (Join-Path $PackageDir $runtimeDll.Name) -Force
    }
}

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$buildDir = Join-Path $repoRoot "build\windows-release"
$distDir = Join-Path $repoRoot "dist"
$stagingRoot = Join-Path $distDir "staging"
$packageName = "lanlink-windows-x64"
$packageDir = Join-Path $stagingRoot $packageName
$archive = Join-Path $distDir "$packageName.zip"
$bootstrapMetadata = Join-Path $repoRoot "third-party\vcpkg-bootstrap.json"
$vcpkgCache = Join-Path $repoRoot "third-party\.cache"
$vcpkgInstalled = Join-Path $repoRoot "third-party\vcpkg_installed"
$installedTriplet = Join-Path $vcpkgInstalled "x64-windows"

$bootstrapConfig = Read-VcpkgBootstrapConfig $bootstrapMetadata
$vcpkgExe = Ensure-Vcpkg $vcpkgCache $bootstrapConfig
$vcpkgRoot = Split-Path $vcpkgExe -Parent
$vcpkgToolchain = Join-Path $vcpkgRoot "scripts\buildsystems\vcpkg.cmake"
$binaryCache = Join-Path $vcpkgCache "archives"
$downloadCache = Join-Path $vcpkgCache "downloads"
$binarySource = "clear;files,$binaryCache,readwrite"

if (-not (Test-Path -LiteralPath $vcpkgToolchain)) {
    throw "Pinned vcpkg toolchain is missing: $vcpkgToolchain"
}

New-Item -ItemType Directory -Force -Path $binaryCache | Out-Null
New-Item -ItemType Directory -Force -Path $downloadCache | Out-Null
$env:VCPKG_BINARY_SOURCES = $binarySource
$env:VCPKG_DOWNLOADS = $downloadCache
Invoke-Checked $vcpkgExe @(
    "install",
    "qtbase[gui,network,testlib,widgets]",
    "qtsvg",
    "openssl",
    "--x-install-root=$vcpkgInstalled",
    "--triplet=x64-windows",
    "--host-triplet=x64-windows",
    "--binarysource=$binarySource",
    "--downloads-root=$downloadCache",
    "--no-print-usage"
)

$qtConfigDir = Join-Path $installedTriplet "lib\cmake\Qt6"
$windeployqtPath = Join-Path $installedTriplet "tools\Qt6\bin\windeployqt.exe"
if (-not (Test-Path -LiteralPath $qtConfigDir)) {
    throw "Pinned Qt6 CMake package is missing: $qtConfigDir"
}
if (-not (Test-Path -LiteralPath $windeployqtPath)) {
    throw "Pinned Qt6 windeployqt.exe is missing: $windeployqtPath"
}

if (Test-Path -LiteralPath $buildDir) {
    Remove-Item -LiteralPath $buildDir -Recurse -Force
}

$cmakeArguments = @(
    "-S", $repoRoot,
    "-B", $buildDir,
    "-G", "Visual Studio 17 2022",
    "-A", "x64",
    "-DCMAKE_CXX_EXTENSIONS=OFF",
    "-DCMAKE_TOOLCHAIN_FILE=$vcpkgToolchain",
    "-DVCPKG_MANIFEST_MODE=OFF",
    "-DVCPKG_MANIFEST_INSTALL=OFF",
    "-DVCPKG_INSTALLED_DIR=$vcpkgInstalled",
    "-DVCPKG_TARGET_TRIPLET=x64-windows",
    "-DCMAKE_PREFIX_PATH=$installedTriplet",
    "-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreadedDLL",
    "-DLANLINK_BUILD_DESKTOP=ON",
    "-DLANLINK_ENABLE_TLS=ON",
    "-DLANLINK_REQUIRE_TLS=ON"
)
Invoke-Checked "cmake" $cmakeArguments
Invoke-Checked "cmake" @("--build", $buildDir, "--config", $Configuration, "--parallel")
Invoke-Checked "ctest" @("--test-dir", $buildDir, "-C", $Configuration, "--repeat", "until-pass:10", "--output-on-failure")

if (Test-Path -LiteralPath $packageDir) {
    Remove-Item -LiteralPath $packageDir -Recurse -Force
}
if (Test-Path -LiteralPath $archive) {
    Remove-Item -LiteralPath $archive -Force
}
New-Item -ItemType Directory -Force -Path $packageDir | Out-Null
Copy-Item (Join-Path $buildDir "$Configuration\lanlink_app.exe") (Join-Path $packageDir "lanlink_app.exe")

$desktopBuild = Join-Path $buildDir "$Configuration\lanlink_desktop.exe"
if (-not (Test-Path -LiteralPath $desktopBuild)) {
    throw "Pinned Qt6 build did not produce lanlink_desktop.exe: $desktopBuild"
}

$desktopPackage = Join-Path $packageDir "lanlink_desktop.exe"
Copy-Item -LiteralPath $desktopBuild -Destination $desktopPackage
Invoke-Checked $windeployqtPath @(
    "--release",
    "--no-translations",
    "--dir", $packageDir,
    $desktopPackage
)
Copy-OpenSslRuntime $installedTriplet $packageDir
Write-Host "Packaged lanlink_desktop.exe and pinned OpenSSL runtime dependencies."

Copy-Item (Join-Path $repoRoot "README.md") (Join-Path $packageDir "README.md")
Copy-Item (Join-Path $repoRoot "third-party\README.md") (Join-Path $packageDir "THIRD_PARTY_NOTICES.md")

foreach ($document in @("LICENSE", "LICENSE.md")) {
    $source = Join-Path $repoRoot $document
    if (Test-Path -LiteralPath $source) {
        Copy-Item -LiteralPath $source -Destination (Join-Path $packageDir $document)
    }
}

Compress-Archive -Path $packageDir -DestinationPath $archive -Force
Write-Host "Created $archive"
