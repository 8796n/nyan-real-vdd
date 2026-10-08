# Run from an elevated console with Spatial Wall stopped. Only this script's
# temporary cookie is removed; existing virtual displays make the trial abort.
param(
    [switch]$UpdateDriver,
    [ValidateRange(2, 120)][int]$Seconds = 10,
    [ValidateRange(1, 10)][int]$Repeats = 3,
    [string]$LogDir = ''
)
$ErrorActionPreference = 'Stop'
$Repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$Ctl = Join-Path $Repo 'out/nyanvddctl.exe'
$Probe = Join-Path $Repo 'x64/Release/dirty_probe.exe'
if (-not $LogDir) { $LogDir = Join-Path $Repo ('out/diagnostics/capture-' + (Get-Date -Format yyyyMMdd-HHmmss)) }
if (-not ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
        [Security.Principal.WindowsBuiltInRole]::Administrator)) { throw 'Run elevated.' }
if (Get-Process spatial-wall -ErrorAction SilentlyContinue) { throw 'Stop Spatial Wall before measuring.' }
New-Item -ItemType Directory -Force $LogDir | Out-Null
Start-Transcript -Path (Join-Path $LogDir 'trial.log') -Force | Out-Null
$Cookie = '0x51C0FFEE'
$Plugged = $false
try {
    $Before = & $Ctl status
    if ($LASTEXITCODE -ne 0 -or ($Before -join "`n") -notmatch 'monitors\s+: 0 /') {
        throw 'Trial requires an installed adapter with no virtual monitors.'
    }
    if ($UpdateDriver) {
        Get-PnpDeviceProperty -InstanceId 'SWD\NYANVDD\NYANVDD' -KeyName DEVPKEY_Device_DriverInfPath |
            Select-Object -ExpandProperty Data | Set-Content (Join-Path $LogDir 'previous-inf.txt')
        & pnputil.exe /add-driver (Join-Path $Repo 'out/package/nyanvdd.inf') /install
        if ($LASTEXITCODE -notin @(0, 259)) { throw "Driver staging returned $LASTEXITCODE; no automatic reboot." }
        & $Ctl remove-device
        if ($LASTEXITCODE -ne 0) { throw 'Device removal failed.' }
        Start-Sleep -Seconds 2
        & $Ctl install-device
        if ($LASTEXITCODE -ne 0) { throw 'Device creation failed.' }
        Start-Sleep -Seconds 3
    }
    $Status = & $Ctl status
    $Status | Out-Host
    if ($LASTEXITCODE -ne 0 -or ($Status -join "`n") -notmatch 'protocol\s+: v4' -or
        ($Status -join "`n") -notmatch 'adapter\s+: ready') { throw 'v4 adapter not ready.' }
    Get-PnpDeviceProperty -InstanceId 'SWD\NYANVDD\NYANVDD' -KeyName DEVPKEY_Device_DriverInfPath |
        Select-Object -ExpandProperty Data | Set-Content (Join-Path $LogDir 'trial-inf.txt')
    foreach ($Width in @(1920, 3840)) {
        $Height = if ($Width -eq 1920) { 1080 } else { 2160 }
        & $Ctl plug "${Width}x${Height}@60" --cookie $Cookie
        if ($LASTEXITCODE -ne 0) { throw 'Probe monitor creation failed.' }
        $Plugged = $true
        Start-Sleep -Seconds 4
        $Resolved = & $Ctl resolve
        $Resolved | Out-Host
        $Line = $Resolved | Where-Object { $_ -match $Cookie } | Select-Object -First 1
        if (-not $Line -or $Line -notmatch '(DISPLAY\d+)\s+' -or $Line -notmatch "${Width}x${Height}@60") {
            throw 'Probe monitor did not resolve at the requested size.'
        }
        $Monitor = [regex]::Match($Line, 'DISPLAY\d+').Value
        $Warm = & $Probe --capture-bench shared --cookie $Cookie --monitor $Monitor --seconds 2 --stimulus 64@60
        $Result = $LASTEXITCODE
        $Warm | Tee-Object -FilePath (Join-Path $LogDir "${Width}-shared-check.log") | Out-Host
        if ($Result -ne 0) { throw 'Shared transport correctness check failed.' }
        $DriverId = [regex]::Match(($Warm -join "`n"), 'driver_pid=(\d+)').Groups[1].Value
        if (-not $DriverId) { throw 'Driver PID missing.' }
        foreach ($Stimulus in @('static', '64@60', "${Width}@60")) {
            for ($Repeat = 1; $Repeat -le $Repeats; ++$Repeat) {
                $Order = if ($Repeat % 2) { @('wgc', 'shared') } else { @('shared', 'wgc') }
                foreach ($Transport in $Order) {
                    $ProbeArgs = @('--capture-bench', $Transport, '--cookie', $Cookie, '--monitor', $Monitor,
                        '--seconds', $Seconds, '--driver-pid', $DriverId)
                    if ($Stimulus -ne 'static') { $ProbeArgs += @('--stimulus', $Stimulus) }
                    $Name = "${Width}-${Stimulus}-${Repeat}-${Transport}"
                    Write-Host "Measuring $Name"
                    $Output = & $Probe @ProbeArgs
                    $Result = $LASTEXITCODE
                    $Output | Tee-Object -FilePath (Join-Path $LogDir "$Name.log") | Out-Host
                    if ($Result -ne 0) { throw "Measurement failed: $Name" }
                }
            }
        }
        & $Ctl unplug $Cookie
        if ($LASTEXITCODE -ne 0) { throw 'Probe monitor cleanup failed.' }
        $Plugged = $false
    }
    'PASS' | Set-Content (Join-Path $LogDir 'result.txt')
} catch {
    "FAIL: $_" | Set-Content (Join-Path $LogDir 'result.txt')
    throw
} finally {
    if ($Plugged) { & $Ctl unplug $Cookie }
    Stop-Transcript | Out-Null
}
