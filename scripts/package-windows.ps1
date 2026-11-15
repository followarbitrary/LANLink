[CmdletBinding()]
param(
    [ValidateSet("Release")]
    [string]$Configuration = "Release",
    [switch]$RequireDesktop
)

$ErrorActionPreference = "Stop"

function Invoke-Checked {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Command,
        [Parameter(Mandatory = $true)]
        [string[]]$Arguments
    )

    & $Command @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "$Command failed with exit code $LASTEXITCODE"
    }
}

function Test-QtPrefix {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Prefix
    )

    return Test-Path (Join-Path $Prefix "lib\cmake\Qt6")
}

function Resolve-QtPrefix {
    $candidates = @()
    if (-not [string]::IsNullOrWhiteSpace($env:QTDIR)) {
        $candidates += $env:QTDIR
    }

    $qmakeCommand = Get-Command "qmake.exe" -ErrorAction SilentlyContinue
    if ($null -ne $qmakeCommand) {
        $qmakePrefix = (& $qmakeCommand.Source "-query" "QT_INSTALL_PREFIX" 2>$null | Out-String).Trim()
        if ($LASTEXITCODE -eq 0 -and -not [string]::IsNullOrWhiteSpace($qmakePrefix)) {
            $candidates += $qmakePrefix
        }
    }

    $windeployqtCommand = Get-Command "windeployqt.exe" -ErrorAction SilentlyContinue
    if ($null -ne $windeployqtCommand) {
        $qtBin = Split-Path $windeployqtCommand.Source -Parent
        $candidates += Split-Path $qtBin -Parent
    }

    foreach ($candidate in ($candidates | Select-Object -Unique)) {
        if ([string]::IsNullOrWhiteSpace($candidate)) {
            continue
        }
        try {
            $resolved = (Resolve-Path -LiteralPath $candidate -ErrorAction Stop).Path
        } catch {
            continue
        }
        if (Test-QtPrefix $resolved) {
            return $resolved
        }

        # QTDIR is sometimes set to C:\Qt rather than to a specific kit.
        $versionDirs = Get-ChildItem -LiteralPath $resolved -Directory -ErrorAction SilentlyContinue
        foreach ($versionDir in $versionDirs) {
            if (Test-QtPrefix $versionDir.FullName) {
                return $versionDir.FullName
            }
            $kitDirs = Get-ChildItem -LiteralPath $versionDir.FullName -Directory -ErrorAction SilentlyContinue
            foreach ($kitDir in $kitDirs) {
                if (Test-QtPrefix $kitDir.FullName) {
                    return $kitDir.FullName
                }
            }
        }
    }

    return $null
}

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$buildDir = Join-Path $repoRoot "build\windows-release"
$distDir = Join-Path $repoRoot "dist"
$stagingRoot = Join-Path $distDir "staging"
$packageName = "lanlink-windows-x64"
$packageDir = Join-Path $stagingRoot $packageName
$archive = Join-Path $distDir "$packageName.zip"

$qtPrefix = Resolve-QtPrefix
$windeployqtPath = $null
$windeployqtCommand = Get-Command "windeployqt.exe" -ErrorAction SilentlyContinue
if ($null -ne $windeployqtCommand) {
    $windeployqtPath = $windeployqtCommand.Source
} elseif (-not [string]::IsNullOrWhiteSpace($qtPrefix)) {
    $qtdirWindeployqt = Join-Path $qtPrefix "bin\windeployqt.exe"
    if (Test-Path $qtdirWindeployqt) {
        $windeployqtPath = $qtdirWindeployqt
    }
}

$desktopEnabled = (-not [string]::IsNullOrWhiteSpace($qtPrefix)) -and
    ($null -ne $windeployqtPath)
if ($RequireDesktop -and -not $desktopEnabled) {
    throw "Qt6 Widgets and windeployqt.exe are required. Install a Qt6 MSVC x64 kit, add its bin directory to PATH, or set QTDIR."
}

if (Test-Path $buildDir) {
    Remove-Item -Recurse -Force $buildDir
}

$cmakeArguments = @(
    "-S", $repoRoot,
    "-B", $buildDir,
    "-G", "Visual Studio 17 2022",
    "-A", "x64",
    "-DCMAKE_CXX_EXTENSIONS=OFF",
    "-DLANLINK_ENABLE_TLS=OFF",
    "-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded"
)
if ($desktopEnabled) {
    $cmakeArguments += "-DLANLINK_BUILD_DESKTOP=ON"
    $cmakeArguments += "-DCMAKE_PREFIX_PATH=$qtPrefix"
    Write-Host "Qt6 detected at $qtPrefix; the desktop app will be packaged."
} else {
    $cmakeArguments += "-DLANLINK_BUILD_DESKTOP=OFF"
    Write-Warning "Qt6 Widgets or windeployqt.exe was not detected; creating a CLI-only Windows archive. Install Qt6 and rerun to include the desktop app."
}
Invoke-Checked "cmake" $cmakeArguments
Invoke-Checked "cmake" @("--build", $buildDir, "--config", $Configuration, "--parallel")
Invoke-Checked "ctest" @("--test-dir", $buildDir, "-C", $Configuration, "--repeat", "until-pass:10", "--output-on-failure")

if (Test-Path $packageDir) {
    Remove-Item -Recurse -Force $packageDir
}
if (Test-Path $archive) {
    Remove-Item -Force $archive
}
New-Item -ItemType Directory -Force -Path $packageDir | Out-Null
Copy-Item (Join-Path $buildDir "$Configuration\lanlink_app.exe") (Join-Path $packageDir "lanlink_app.exe")

if ($desktopEnabled) {
    $desktopBuild = Join-Path $buildDir "$Configuration\lanlink_desktop.exe"
    if (-not (Test-Path $desktopBuild)) {
        throw "Qt6 was detected, but lanlink_desktop.exe was not built. Check the Qt6 MSVC x64 installation."
    }

    $desktopPackage = Join-Path $packageDir "lanlink_desktop.exe"
    Copy-Item $desktopBuild $desktopPackage
    Invoke-Checked $windeployqtPath @(
        "--release",
        "--no-translations",
        "--dir", $packageDir,
        $desktopPackage
    )
    Write-Host "Packaged lanlink_desktop.exe with Qt runtime dependencies."
}

Copy-Item (Join-Path $repoRoot "README.md") (Join-Path $packageDir "README.md")

foreach ($document in @("LICENSE", "LICENSE.md")) {
    $source = Join-Path $repoRoot $document
    if (Test-Path $source) {
        Copy-Item $source (Join-Path $packageDir $document)
    }
}

Compress-Archive -Path $packageDir -DestinationPath $archive -Force
Write-Host "Created $archive"
