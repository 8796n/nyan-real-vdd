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

$Revision = (& git -C $RepoRoot rev-parse --short HEAD 2>$null)
if ($LASTEXITCODE -ne 0 -or -not $Revision) { $Revision = 'nogit' }
$Name = "nyan-real-vdd-x64-$Revision"
$Staging = Join-Path $OutDir $Name

if (Test-Path $Staging) { Remove-Item $Staging -Recurse -Force }
New-Item -ItemType Directory -Force $Staging | Out-Null

Copy-Item (Join-Path $PackageSrc '*') $Staging -Force
Copy-Item $Cer $Staging -Force
Copy-Item $Ctl $Staging -Force
Copy-Item (Join-Path $PSScriptRoot 'install.ps1') $Staging -Force
Copy-Item (Join-Path $PSScriptRoot 'uninstall.ps1') $Staging -Force

$Signer = $CatSignature.SignerCertificate
$SignerLine = if ($Signer) { "$($Signer.Subject)  (thumbprint $($Signer.Thumbprint))" } else { 'unknown' }

@"
nyan Real VDD - portable package ($Revision)
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
    $Iss = Join-Path $RepoRoot 'installer\nyan-real-vdd.iss'
    & $Iscc "/DAppVersion=$Revision" "/DStageDir=$Staging" "/O$OutDir" $Iss | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "ISCC failed ($LASTEXITCODE)" }

    $Setup = Join-Path $OutDir "nyan-real-vdd-$Revision-windows-x64-installer.exe"
    Write-Host ''
    Write-Host "OK: $Setup"
    Write-Host '    (unsigned: sign it before handing it to anyone else, or'
    Write-Host '     SmartScreen will warn on first run)'
} else {
    Write-Warning 'Inno Setup (ISCC.exe) not found - skipped the installer (portable ZIP still built).'
}
