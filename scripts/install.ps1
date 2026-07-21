# Installs the nyan Real VDD on this machine (run as Administrator):
#   1. trusts the publisher certificate (Root + TrustedPublisher)
#   2. stages/installs the driver package (pnputil)
#   3. creates the persistent device node (nyanvddctl install-device)
#
# Prerequisites: scripts\build.ps1 then scripts\sign-dev.ps1 (or a package
# signed via attestation, in which case -SkipCert).

param(
    [string]$PackageDir = '',
    [string]$CerPath = '',
    [switch]$SkipCert
)

$ErrorActionPreference = 'Stop'
$RepoRoot = Split-Path -Parent $PSScriptRoot
if (-not $PackageDir) { $PackageDir = Join-Path $RepoRoot 'out\package' }
if (-not $CerPath) { $CerPath = Join-Path $RepoRoot 'out\nyanvdd-dev.cer' }

if (-not ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'run this script from an elevated (Administrator) PowerShell'
}

$Inf = Join-Path $PackageDir 'nyanvdd.inf'
if (-not (Test-Path $Inf)) { throw "driver package not found: $Inf" }

if (-not $SkipCert) {
    if (-not (Test-Path $CerPath)) { throw "certificate not found: $CerPath (run sign-dev.ps1)" }
    certutil -addstore -f root $CerPath | Out-Null
    certutil -addstore -f TrustedPublisher $CerPath | Out-Null
    Write-Host 'certificate trusted (Root + TrustedPublisher)'
}

pnputil /add-driver $Inf /install
if ($LASTEXITCODE -ne 0) { throw "pnputil /add-driver failed ($LASTEXITCODE)" }

$Ctl = Join-Path $RepoRoot 'out\nyanvddctl.exe'
if (-not (Test-Path $Ctl)) { throw "nyanvddctl.exe not found: $Ctl" }
& $Ctl install-device
if ($LASTEXITCODE -ne 0) { throw 'nyanvddctl install-device failed' }

Write-Host ''
Write-Host 'OK — try: out\nyanvddctl.exe plug 1920x1080@120'
