# Produces a portable, signed package that can be copied to another machine
# and installed there without the repo, the WDK, or a build, plus an installer
# .exe when Inno Setup is available.
#
# Signing happens here rather than in CI on purpose: the signing key must not
# leave the developer's machine, and a package signed with a fresh key on every
# CI run would ask every test machine to trust a new root certificate.
#
#   scripts\package.ps1              build + sign + package
#   scripts\package.ps1 -SkipBuild   package what is already in out\

param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release',
    [switch]$SkipBuild
)

$ErrorActionPreference = 'Stop'
$RepoRoot = Split-Path -Parent $PSScriptRoot
$OutDir = Join-Path $RepoRoot 'out'

# Everything that can be checked without side effects is checked first, so a
# bad input fails in a second instead of after a build — and, more importantly,
# without having already deleted the previous good artifacts.

# A real version number, not the commit hash: it ends up in Add/Remove
# Programs and is what upgrade checks compare, and Inno falls back to 0.0.0.0
# for VersionInfoVersion when it cannot parse it. The commit only identifies
# the build.
$Version = (Get-Content (Join-Path $RepoRoot 'VERSION') -Raw).Trim()
if ($Version -notmatch '^\d+\.\d+\.\d+$') { throw "VERSION must be a.b.c, got '$Version'" }

# Keep the driver's own version macros from drifting away from VERSION.
$DriverHeader = Get-Content (Join-Path $RepoRoot 'driver\src\Driver.h') -Raw
$HeaderMajor = [regex]::Match($DriverHeader, 'NYANVDD_DRIVER_VERSION_MAJOR\s+(\d+)').Groups[1].Value
$HeaderMinor = [regex]::Match($DriverHeader, 'NYANVDD_DRIVER_VERSION_MINOR\s+(\d+)').Groups[1].Value
$VersionParts = $Version.Split('.')
if ($HeaderMajor -ne $VersionParts[0] -or $HeaderMinor -ne $VersionParts[1]) {
    throw "VERSION ($Version) disagrees with Driver.h ($HeaderMajor.$HeaderMinor)"
}

$Revision = (& git -C $RepoRoot rev-parse --short HEAD 2>$null)
if ($LASTEXITCODE -ne 0 -or -not $Revision) { $Revision = 'nogit' }
$Build = "$Version+g$Revision"
$Name = "nyan-real-vdd-$Build-x64"
$Staging = Join-Path $OutDir $Name

if (-not $SkipBuild) {
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'build.ps1') -Configuration $Configuration
    if ($LASTEXITCODE -ne 0) { throw 'build failed' }
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'sign-dev.ps1')
    if ($LASTEXITCODE -ne 0) { throw 'signing failed' }
}

$PackageSrc = Join-Path $OutDir 'package'
$Cer = Join-Path $OutDir 'nyanvdd-dev.cer'
$Ctl = Join-Path $OutDir 'nyanvddctl.exe'
foreach ($Required in (Join-Path $PackageSrc 'nyanvdd.inf'), (Join-Path $PackageSrc 'nyanvdd.cat'), $Cer, $Ctl) {
    if (-not (Test-Path $Required)) { throw "missing: $Required (run without -SkipBuild)" }
}

# Refuse to ship an unsigned catalog: on the target machine that fails at
# install time with a much less obvious error.
$CatSignature = Get-AuthenticodeSignature (Join-Path $PackageSrc 'nyanvdd.cat')
if ($CatSignature.Status -ne 'Valid' -and $CatSignature.Status -ne 'UnknownError') {
    throw "nyanvdd.cat is not signed (status: $($CatSignature.Status)) — run scripts\sign-dev.ps1"
}

# Everything above here can fail without having destroyed anything. From this
# point on we start replacing artifacts, so drop the ones from earlier builds:
# staging folders as well as archives, because a leftover folder is just as
# good at making you test something you did not build.
Get-ChildItem $OutDir -Filter 'nyan-real-vdd-*' -ErrorAction SilentlyContinue |
    Where-Object { $_.PSIsContainer -or $_.Extension -in '.zip', '.exe' } |
    Remove-Item -Recurse -Force

New-Item -ItemType Directory -Force $Staging | Out-Null

Copy-Item (Join-Path $PackageSrc '*') $Staging -Force
Copy-Item $Cer $Staging -Force
Copy-Item $Ctl $Staging -Force
Copy-Item (Join-Path $PSScriptRoot 'install.ps1') $Staging -Force
Copy-Item (Join-Path $PSScriptRoot 'uninstall.ps1') $Staging -Force

$Signer = $CatSignature.SignerCertificate
$SignerLine = if ($Signer) { "$($Signer.Subject)  (thumbprint $($Signer.Thumbprint))" } else { 'unknown' }

@"
nyan Real VDD - portable package ($Build)
============================================

A virtual display driver for Windows 11 24H2 and later (x64).
Source and issues: https://github.com/8796n/nyan-real-vdd

Install (elevated PowerShell, from this folder):

    .\install.ps1

That trusts the signing certificate below, installs the driver, and creates
the device node. Then, from any normal (non-elevated) prompt:

    .\nyanvddctl.exe status
    .\nyanvddctl.exe plug 1920x1080@120
    .\nyanvddctl.exe resolve
    .\nyanvddctl.exe unplug all

Remove everything again:

    .\uninstall.ps1 -RemoveCert

WHAT install.ps1 TRUSTS
-----------------------
This package is signed with a self-signed development certificate:

    $SignerLine

install.ps1 adds it to the machine's Trusted Root and Trusted Publishers
stores. From then on this machine accepts ANY binary signed with that key,
so only install this on machines you are willing to extend that trust to,
and remove it with 'uninstall.ps1 -RemoveCert' when you are done.

NOTES
-----
* Requires Windows 11 24H2 (build 26100) or later; install.ps1 checks.
* Over Remote Desktop the monitors are created on the CONSOLE session's
  desktop and are not visible in the remote session. nyanvddctl says so
  explicitly when that happens. Test from the machine's own console.
* Driver log: C:\ProgramData\nyan-real-vdd\driver.log
"@ | Set-Content (Join-Path $Staging 'README.txt') -Encoding UTF8

$Zip = Join-Path $OutDir "$Name.zip"
if (Test-Path $Zip) { Remove-Item $Zip -Force }
Compress-Archive -Path (Join-Path $Staging '*') -DestinationPath $Zip

Write-Host ''
Write-Host "OK: $Zip"
Get-ChildItem $Staging | ForEach-Object { Write-Host ("    {0,-22} {1,9:N0} bytes" -f $_.Name, $_.Length) }

# Installer (Inno Setup), if ISCC is present. The installer lays down the same
# staged payload and runs the same install.ps1, so it is packaging only.
$Iscc = @(
    "${env:ProgramFiles}\Inno Setup 7\ISCC.exe",
    "${env:ProgramFiles(x86)}\Inno Setup 7\ISCC.exe",
    "${env:ProgramFiles}\Inno Setup 6\ISCC.exe",
    "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe"
) | Where-Object { Test-Path $_ } | Select-Object -First 1

if ($Iscc) {
    # The installer needs its own README: the portable one talks about running
    # scripts out of an unpacked folder, which is wrong once Setup has done it.
    $InstalledReadme = Join-Path $OutDir 'README-installed.txt'
    @"
nyan Real VDD ($Build)
======================

Installed. https://github.com/8796n/nyan-real-vdd

USE IT
------
nyanvddctl.exe lives in this folder. From any normal (non-elevated) prompt:

    "%ProgramFiles%\nyan Real VDD\nyanvddctl.exe" status
    "%ProgramFiles%\nyan Real VDD\nyanvddctl.exe" plug 1920x1080@120
    "%ProgramFiles%\nyan Real VDD\nyanvddctl.exe" resolve
    "%ProgramFiles%\nyan Real VDD\nyanvddctl.exe" unplug all

Adding and removing monitors does not need administrator rights.

REMOVE IT
---------
Settings > Apps > Installed apps > nyan Real VDD > Uninstall.
That also removes the driver and the signing certificate below.

WHAT THIS MACHINE NOW TRUSTS
----------------------------
The driver is signed with a self-signed development certificate:

    $SignerLine

Setup added it to this machine's Trusted Root and Trusted Publishers stores,
so the machine now accepts ANY binary signed with that key. Uninstalling
removes it again.

NOTES
-----
* Over Remote Desktop the monitors are created on the CONSOLE session's
  desktop and are not visible in the remote session. nyanvddctl says so
  explicitly when that happens. Test from the machine's own console.
* Driver log:    C:\ProgramData\nyan-real-vdd\driver.log
* Setup logs:    C:\ProgramData\nyan-real-vdd\install.log
                 C:\ProgramData\nyan-real-vdd\uninstall.log
"@ | Set-Content $InstalledReadme -Encoding UTF8

    $Iss = Join-Path $RepoRoot 'installer\nyan-real-vdd.iss'
    & $Iscc "/DAppVersion=$Version" "/DBuildId=$Build" "/DStageDir=$Staging" `
            "/DInstalledReadme=$InstalledReadme" "/O$OutDir" $Iss | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "ISCC failed ($LASTEXITCODE)" }

    $Setup = Join-Path $OutDir "nyan-real-vdd-$Build-windows-x64-installer.exe"
    Write-Host ''
    Write-Host "OK: $Setup"
    Write-Host '    (unsigned: sign it before handing it to anyone else, or'
    Write-Host '     SmartScreen will warn on first run)'
} else {
    Write-Warning 'Inno Setup (ISCC.exe) not found - skipped the installer (portable ZIP still built).'
}
