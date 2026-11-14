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

    & $Command @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "$Command failed with exit code $LASTEXITCODE"
    }
}

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$buildDir = Join-Path $repoRoot "build\windows-release"
$distDir = Join-Path $repoRoot "dist"
$stagingRoot = Join-Path $distDir "staging"
$packageName = "lanlink-windows-x64"
$packageDir = Join-Path $stagingRoot $packageName
$archive = Join-Path $distDir "$packageName.zip"

if (Test-Path $buildDir) {
    Remove-Item -Recurse -Force $buildDir
}

$qtPrefix = $env:QTDIR
$qmakeCommand = Get-Command "qmake.exe" -ErrorAction SilentlyContinue
if ([string]::IsNullOrWhiteSpace($qtPrefix) -and $null -ne $qmakeCommand) {
    $qtPrefix = (& $qmakeCommand.Source "-query" "QT_INSTALL_PREFIX").Trim()
    if ($LASTEXITCODE -ne 0) {
        $qtPrefix = $null
    }
}

$cmakeArguments = @(
    "-S", $repoRoot,
    "-B", $buildDir,
    "-G", "Visual Studio 17 2022",
    "-A", "x64",
    "-DCMAKE_CXX_EXTENSIONS=OFF",
    "-DLANLINK_BUILD_DESKTOP=ON",
    "-DLANLINK_ENABLE_TLS=OFF",
    "-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded"
)
if (-not [string]::IsNullOrWhiteSpace($qtPrefix) -and
    (Test-Path (Join-Path $qtPrefix "lib\cmake\Qt6"))) {
    $cmakeArguments += "-DCMAKE_PREFIX_PATH=$qtPrefix"
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

$desktopBuild = Join-Path $buildDir "$Configuration\lanlink_desktop.exe"
if (-not (Test-Path $desktopBuild)) {
    throw "lanlink_desktop.exe was not built. Install Qt6 Widgets and rerun the Windows packaging script."
}

$windeployqtPath = $null
$windeployqtCommand = Get-Command "windeployqt.exe" -ErrorAction SilentlyContinue
if ($null -ne $windeployqtCommand) {
    $windeployqtPath = $windeployqtCommand.Source
} elseif ($env:QTDIR) {
    $qtdirWindeployqt = Join-Path $env:QTDIR "bin\windeployqt.exe"
    if (Test-Path $qtdirWindeployqt) {
        $windeployqtPath = $qtdirWindeployqt
    }
}
if ($null -eq $windeployqtPath) {
    throw "lanlink_desktop.exe was built, but windeployqt.exe was not found. Add Qt6's bin directory to PATH or set QTDIR."
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

Copy-Item (Join-Path $repoRoot "README.md") (Join-Path $packageDir "README.md")

foreach ($document in @("LICENSE", "LICENSE.md")) {
    $source = Join-Path $repoRoot $document
    if (Test-Path $source) {
        Copy-Item $source (Join-Path $packageDir $document)
    }
}

Compress-Archive -Path $packageDir -DestinationPath $archive -Force
Write-Host "Created $archive"
