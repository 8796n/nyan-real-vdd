# Removes the nyan Real VDD from this machine (run as Administrator):
# unplugs monitors, removes the device node, deletes the driver package,
# and (optionally, -RemoveCert) untrusts the dev certificate.

param(
    [switch]$RemoveCert
)

$ErrorActionPreference = 'Stop'
$RepoRoot = Split-Path -Parent $PSScriptRoot

if (-not ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'run this script from an elevated (Administrator) PowerShell'
}

$Ctl = Join-Path $RepoRoot 'out\nyanvddctl.exe'
if (Test-Path $Ctl) {
    & $Ctl unplug all 2>$null
    & $Ctl remove-device
}

# Find the staged oemNN.inf whose original name is nyanvdd.inf.
$Published = (pnputil /enum-drivers) -join "`n" -split "(?m)^Published Name:" |
    Where-Object { $_ -match 'nyanvdd\.inf' }
foreach ($Block in $Published) {
    if ($Block -match '(oem\d+\.inf)') {
        $Oem = $Matches[1]
        Write-Host "deleting driver package $Oem"
        pnputil /delete-driver $Oem /uninstall /force
    }
}

if ($RemoveCert) {
    foreach ($Store in 'Root', 'TrustedPublisher') {
        Get-ChildItem "Cert:\LocalMachine\$Store" |
            Where-Object { $_.Subject -eq 'CN=nyan Real Driver Publisher' } |
            ForEach-Object {
                Write-Host "removing cert from $Store"
                Remove-Item $_.PSPath
            }
    }
}

Write-Host 'OK — uninstalled'
