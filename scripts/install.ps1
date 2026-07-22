# Installs the nyan Real VDD on this machine (run as Administrator):
#   1. trusts the publisher certificate (Root + TrustedPublisher)
#   2. stages/installs the driver package (pnputil)
#   3. creates the persistent device node (nyanvddctl install-device)
#
# Works in two layouts:
#   - from the repo, after scripts\build.ps1 and scripts\sign-dev.ps1
#   - from a portable package produced by scripts\package.ps1, where this
#     script sits next to the driver files (nothing else is needed)

param(
    [string]$PackageDir = '',
    [string]$CerPath = '',
    [switch]$SkipCert,
    # Where to record everything this script printed. The installer passes this
    # so a failure during an unattended install leaves something to read.
    [string]$LogPath = ''
)

$ErrorActionPreference = 'Stop'

# Re-launch natively when started from a 32-bit host. Inno Setup's [Code] runs
# 32-bit, so Exec('powershell.exe') gets the SysWOW64 copy, where %WINDIR%\
# System32 is redirected and pnputil.exe does not exist at all (certutil does,
# which makes the failure look like it comes from nowhere). Sysnative is the
# alias that reaches the real System32 from a 32-bit process.
if (-not [Environment]::Is64BitProcess -and [Environment]::Is64BitOperatingSystem) {
    $Native = Join-Path $env:WINDIR 'Sysnative\WindowsPowerShell\v1.0\powershell.exe'
    if (Test-Path $Native) {
        $Forward = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $PSCommandPath)
        foreach ($Entry in $PSBoundParameters.GetEnumerator()) {
            if ($Entry.Value -is [switch]) {
                if ($Entry.Value.IsPresent) { $Forward += "-$($Entry.Key)" }
            } else {
                $Forward += "-$($Entry.Key)"
                $Forward += [string]$Entry.Value
            }
        }
        & $Native @Forward
        exit $LASTEXITCODE
    }
    throw 'running 32-bit on a 64-bit system and the native PowerShell was not found'
}

if ($LogPath) {
    New-Item -ItemType Directory -Force (Split-Path -Parent $LogPath) | Out-Null
    Start-Transcript -Path $LogPath -Force | Out-Null
}
try {

# Portable package: the driver files live next to this script.
$Portable = Test-Path (Join-Path $PSScriptRoot 'nyanvdd.inf')
if ($Portable) {
    if (-not $PackageDir) { $PackageDir = $PSScriptRoot }
    if (-not $CerPath) { $CerPath = Join-Path $PSScriptRoot 'nyanvdd-dev.cer' }
    $Ctl = Join-Path $PSScriptRoot 'nyanvddctl.exe'
} else {
    $RepoRoot = Split-Path -Parent $PSScriptRoot
    if (-not $PackageDir) { $PackageDir = Join-Path $RepoRoot 'out\package' }
    if (-not $CerPath) { $CerPath = Join-Path $RepoRoot 'out\nyanvdd-dev.cer' }
    $Ctl = Join-Path $RepoRoot 'out\nyanvddctl.exe'
}

if (-not ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'run this script from an elevated (Administrator) PowerShell'
}

# The INF only binds on Windows 11 24H2 and later, but staging the package and
# creating the device node succeed anywhere — which would leave a device with
# no driver behind it and no explanation. Refuse up front instead.
$Build = [Environment]::OSVersion.Version.Build
if ($Build -lt 26100) {
    throw "Windows 11 24H2 (build 26100) or later is required; this machine is build $Build."
}

$Inf = Join-Path $PackageDir 'nyanvdd.inf'
if (-not (Test-Path $Inf)) { throw "driver package not found: $Inf" }
if (-not (Test-Path $Ctl)) { throw "nyanvddctl.exe not found: $Ctl" }

if (-not $SkipCert) {
    if (-not (Test-Path $CerPath)) { throw "certificate not found: $CerPath (run sign-dev.ps1)" }
    Write-Host "Trusting $CerPath as a code-signing root on this machine."
    Write-Host 'Any driver or binary signed with that key will be accepted from now on.'
    certutil -addstore -f root $CerPath | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "certutil failed to add the certificate to Root ($LASTEXITCODE)" }
    certutil -addstore -f TrustedPublisher $CerPath | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "certutil failed to add the certificate to TrustedPublisher ($LASTEXITCODE)" }
    Write-Host 'certificate trusted (Root + TrustedPublisher)'
}

pnputil /add-driver $Inf /install
# 3010 == ERROR_SUCCESS_REBOOT_REQUIRED, which is a success.
if ($LASTEXITCODE -eq 3010) {
    Write-Host 'driver staged; a reboot is required to complete the installation'
} elseif ($LASTEXITCODE -ne 0) {
    throw "pnputil /add-driver failed ($LASTEXITCODE)"
}

& $Ctl install-device
if ($LASTEXITCODE -ne 0) { throw 'nyanvddctl install-device failed' }

Write-Host ''
Write-Host "OK - try: `"$Ctl`" plug 1920x1080@120"
Write-Host "         `"$Ctl`" resolve"

}
finally {
    if ($LogPath) { Stop-Transcript | Out-Null }
}
