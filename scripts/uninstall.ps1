# Removes the nyan Real VDD from this machine (run as Administrator):
# unplugs monitors, removes the device node, deletes every staged copy of the
# driver package, and (with -RemoveCert) untrusts the publisher certificate.
#
# Works both from the repo and from a portable package (see install.ps1).

param(
    [switch]$RemoveCert,
    [string]$CertSubject = 'CN=nyan Real Driver Publisher'
)

$ErrorActionPreference = 'Stop'

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
        pnputil /delete-driver $Oem /uninstall /force
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

Write-Host 'OK — uninstalled'
