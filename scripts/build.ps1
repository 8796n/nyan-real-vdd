# Builds the driver package and the control CLI.
# Requires: Visual Studio (MSVC), Windows SDK, and the WDK with its Visual
# Studio extension (provides the WindowsUserModeDriver10.0 toolset).
#
# Output: out\package\ (nyanvdd.inf / nyanvdd.dll / nyanvdd.cat, test-signed
# by the WDK build), out\nyanvddctl.exe. Re-sign with scripts\sign-dev.ps1
# before distributing.

param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release'
)

$ErrorActionPreference = 'Stop'
$RepoRoot = Split-Path -Parent $PSScriptRoot

$VsWhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path $VsWhere)) {
    throw 'vswhere.exe not found — install Visual Studio first.'
}
$MsBuild = & $VsWhere -latest -requires Microsoft.Component.MSBuild -find 'MSBuild\**\Bin\MSBuild.exe' | Select-Object -First 1
if (-not $MsBuild) {
    throw 'MSBuild not found via vswhere.'
}

& $MsBuild (Join-Path $RepoRoot 'nyan-real-vdd.sln') `
    /m /v:minimal /nologo `
    /p:Configuration=$Configuration /p:Platform=x64
if ($LASTEXITCODE -ne 0) {
    Write-Host ''
    Write-Host 'Build failed. If the error mentions a missing toolset' -ForegroundColor Yellow
    Write-Host '(WindowsUserModeDriver10.0), install the WDK and its VS extension:' -ForegroundColor Yellow
    Write-Host '  https://learn.microsoft.com/windows-hardware/drivers/download-the-wdk' -ForegroundColor Yellow
    exit 1
}

$OutDir = Join-Path $RepoRoot 'out'
$PackageOut = Join-Path $OutDir 'package'
New-Item -ItemType Directory -Force $PackageOut | Out-Null

# The WDK build stages the driver package (inf + dll + cat) into a folder
# named after the project under the build output directory.
$PackageSrc = Join-Path $RepoRoot "driver\x64\$Configuration\nyanvdd"
if (-not (Test-Path (Join-Path $PackageSrc 'nyanvdd.inf'))) {
    throw "driver package not found at $PackageSrc"
}
Copy-Item (Join-Path $PackageSrc '*') $PackageOut -Force

Copy-Item (Join-Path $RepoRoot "cli\x64\$Configuration\nyanvddctl.exe") $OutDir -Force

Write-Host ''
Write-Host "OK: $PackageOut (driver package), $(Join-Path $OutDir 'nyanvddctl.exe')"
