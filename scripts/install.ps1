# Installs the nyan Real Virtual Display Driver on this machine (run as
# Administrator):
#   1. trusts the publisher certificate (Root + TrustedPublisher)
#   2. stages/installs the driver package (pnputil)
#   3. creates the persistent device node (nyanvddctl install-device)
#   4. registers a scheduled task that recreates that node at boot/logon
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

# The installer .exe refuses ARM64 via ArchitecturesAllowed, but the portable
# ZIP runs this script directly. ARM64 Windows emulates x64 user-mode code well
# enough to run everything here — including staging the package and creating
# the device node — but it cannot load an x64 driver, which would end in a
# device with no driver and no hint why. OSArchitecture is immune to emulation
# (PROCESSOR_ARCHITECTURE is not: it reports x64 inside an emulated process).
$Arch = [System.Runtime.InteropServices.RuntimeInformation]::OSArchitecture
if ("$Arch" -ne 'X64') {
    throw "this driver package is x64-only; this machine's OS architecture is $Arch."
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

$RebootRequired = $false
pnputil /add-driver $Inf /install
# 3010 == ERROR_SUCCESS_REBOOT_REQUIRED, which is a success.
if ($LASTEXITCODE -eq 3010) {
    Write-Host 'driver staged; a reboot is required to complete the installation'
    $RebootRequired = $true
} elseif ($LASTEXITCODE -eq 259) {
    # 259 == ERROR_NO_MORE_ITEMS, which pnputil returns when it added nothing
    # because the package is already staged and up to date ("Driver package
    # added successfully. (Already exists in the system)" / "Driver packages
    # added: 0"). That is what every re-run looks like — including the repair
    # run that this script exists for when the device node goes missing — so
    # treating it as a failure made the documented recovery path unusable.
    Write-Host 'driver package already present and up to date'
} elseif ($LASTEXITCODE -ne 0) {
    throw "pnputil /add-driver failed ($LASTEXITCODE)"
}

& $Ctl install-device
if ($LASTEXITCODE -ne 0) { throw 'nyanvddctl install-device failed' }

# The device node is meant to outlive reboots (SWDeviceLifetimeParentPresent),
# and normally does — but it has been observed to go non-present on its own
# while the driver package stays installed, around the periodic device/driver
# cleanup ("cleanmgr /autocleanstoragesense"). Nothing in the driver can see
# that: the node is what clients open, so they just report "no nyanvdd device
# found" and fall back to whatever else they support.
#
# So re-create it at every boot and logon. install-device is idempotent —
# SwDeviceCreate reopens an existing instance and re-applies the lifetime — so
# the normal case costs one short process start.
#
# The task must point at a copy that outlives this install: the portable ZIP
# folder can be deleted, and the installer runs this script from {tmp}. The
# driver's own ProgramData directory is the one place both layouts share.
$RestoreDir = Join-Path $env:ProgramData 'nyan-real-vdd'
$RestoreCtl = Join-Path $RestoreDir 'nyanvddctl.exe'
# Keep these two in sync with uninstall.ps1.
$TaskPath = '\nyan Real\'
$TaskName = 'nyanvdd device node'

try {
    New-Item -ItemType Directory -Force $RestoreDir | Out-Null
    Copy-Item $Ctl $RestoreCtl -Force

    # S-1-5-18 rather than 'SYSTEM': the well-known SID is the same on every
    # locale, the display name is not.
    $TaskAction = New-ScheduledTaskAction -Execute $RestoreCtl -Argument 'install-device'
    $TaskTriggers = @(
        (New-ScheduledTaskTrigger -AtStartup),
        (New-ScheduledTaskTrigger -AtLogOn)
    )
    $TaskPrincipal = New-ScheduledTaskPrincipal -UserId 'S-1-5-18' `
        -LogonType ServiceAccount -RunLevel Highest
    # StartWhenAvailable covers a machine that was asleep at boot time; the
    # battery flags stop Windows from skipping the task on a laptop.
    $TaskSettings = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries `
        -DontStopIfGoingOnBatteries -StartWhenAvailable `
        -MultipleInstances IgnoreNew -ExecutionTimeLimit ([TimeSpan]::Zero)

    Register-ScheduledTask -TaskName $TaskName -TaskPath $TaskPath `
        -Action $TaskAction -Trigger $TaskTriggers -Principal $TaskPrincipal `
        -Settings $TaskSettings -Force `
        -Description 'Recreates the nyan Real virtual display device node if it goes missing.' | Out-Null

    Write-Host "device node restore task registered ($TaskPath$TaskName)"
} catch {
    # Not fatal: the driver and the device node are already in place, and this
    # only protects against a later disappearance. Say so loudly instead of
    # failing an otherwise good install.
    Write-Warning "could not register the device node restore task: $($_.Exception.Message)"
    Write-Warning 'The driver works, but a device node that disappears later will not come back on its own.'
}

Write-Host ''
Write-Host "OK - try: `"$Ctl`" plug 1920x1080@120"
Write-Host "         `"$Ctl`" resolve"

# Pass "installed, but the machine needs a reboot" up as ERROR_SUCCESS_REBOOT_
# REQUIRED rather than swallowing it, so a caller (the installer, or a
# deployment tool) can schedule the restart instead of guessing.
if ($RebootRequired) {
    Write-Host ''
    Write-Host 'A reboot is required to finish installing the driver.'
    exit 3010
}

}
finally {
    if ($LogPath) { Stop-Transcript | Out-Null }
}
