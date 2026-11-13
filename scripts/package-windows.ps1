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

Invoke-Checked "cmake" @(
    "-S", $repoRoot,
    "-B", $buildDir,
    "-G", "Visual Studio 17 2022",
    "-A", "x64",
    "-DCMAKE_CXX_EXTENSIONS=OFF",
    "-DLANLINK_ENABLE_TLS=OFF",
    "-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded"
)
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
Copy-Item (Join-Path $repoRoot "README.md") (Join-Path $packageDir "README.md")

foreach ($document in @("LICENSE", "LICENSE.md")) {
    $source = Join-Path $repoRoot $document
    if (Test-Path $source) {
        Copy-Item $source (Join-Path $packageDir $document)
    }
}

Compress-Archive -Path $packageDir -DestinationPath $archive -Force
Write-Host "Created $archive"
