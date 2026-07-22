# Removes the nyan Real Virtual Display Driver from this machine (run as
# Administrator):
# unplugs monitors, removes the device node, deletes every staged copy of the
# driver package, and (with -RemoveCert) untrusts the publisher certificate.
#
# Works both from the repo and from a portable package (see install.ps1).

param(
    [switch]$RemoveCert,
    [string]$CertSubject = 'CN=nyan Real Driver Publisher',
    [string]$LogPath = ''
)

$ErrorActionPreference = 'Stop'

# See install.ps1: a 32-bit host cannot see pnputil.exe at all.
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

$Portable = Test-Path (Join-Path $PSScriptRoot 'nyanvdd.inf')
if ($Portable) {
    $Ctl = Join-Path $PSScriptRoot 'nyanvddctl.exe'
} else {
    $Ctl = Join-Path (Split-Path -Parent $PSScriptRoot) 'out\nyanvddctl.exe'
}

if (-not ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'run this script from an elevated (Administrator) PowerShell'
}

if (Test-Path $Ctl) {
    & $Ctl unplug all
    & $Ctl remove-device
} else {
    Write-Warning "nyanvddctl.exe not found ($Ctl); removing the driver package only."
    Write-Warning 'The device node may be left behind — remove it from Device Manager if so.'
}

# Find every staged copy by its original INF name. Get-WindowsDriver reports
# OriginalFileName and Driver (the oemNN.inf name) as data, so this does not
# depend on the console language the way parsing pnputil output would.
$Staged = @(Get-WindowsDriver -Online -All |
    Where-Object { $_.OriginalFileName -match 'nyanvdd\.inf$' })

if ($Staged.Count -eq 0) {
    Write-Host 'no staged nyanvdd driver package found'
} else {
    foreach ($Driver in $Staged) {
        $Oem = Split-Path $Driver.Driver -Leaf
        Write-Host "deleting driver package $Oem"
        # No /force: pnputil reports that it ignores /force when deleting with
        # /uninstall, and it would only mask a package that is still in use.
        pnputil /delete-driver $Oem /uninstall
        if ($LASTEXITCODE -ne 0) {
            Write-Warning "pnputil /delete-driver $Oem failed ($LASTEXITCODE)"
        }
    }
}

if ($RemoveCert) {
    foreach ($Store in 'Root', 'TrustedPublisher') {
        Get-ChildItem "Cert:\LocalMachine\$Store" |
            Where-Object { $_.Subject -eq $CertSubject } |
            ForEach-Object {
                Write-Host "removing certificate from $Store"
                Remove-Item $_.PSPath -Force
            }
    }
}

Write-Host 'OK - uninstalled'

}
finally {
    if ($LogPath) { Stop-Transcript | Out-Null }
}
