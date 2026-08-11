# Development iteration helper: build + sign (no elevation), then bundle the
# elevation-requiring steps — pnputil driver update and device-node recycle —
# into ONE elevated child process (single UAC prompt).
#
# Day-to-day control (plug/unplug/list/status) never needs elevation; only
# driver servicing does.

param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release',
    [switch]$SkipBuild
)

$ErrorActionPreference = 'Stop'
$RepoRoot = Split-Path -Parent $PSScriptRoot

if (-not $SkipBuild) {
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'build.ps1') -Configuration $Configuration
    if ($LASTEXITCODE -ne 0) { throw 'build failed' }
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'sign-dev.ps1')
    if ($LASTEXITCODE -ne 0) { throw 'signing failed' }
}

$Inf = Join-Path $RepoRoot 'out\package\nyanvdd.inf'
$Ctl = Join-Path $RepoRoot 'out\nyanvddctl.exe'
$Elevated = @"
& pnputil.exe /add-driver '$Inf' /install
`$PnPResult = `$LASTEXITCODE
if (`$PnPResult -eq 3010) { exit 3010 }
if (`$PnPResult -ne 0 -and `$PnPResult -ne 259) { exit `$PnPResult }

& '$Ctl' remove-device
`$CtlResult = `$LASTEXITCODE
if (`$CtlResult -ne 0) { exit `$CtlResult }
Start-Sleep -Seconds 2
& '$Ctl' install-device
`$CtlResult = `$LASTEXITCODE
if (`$CtlResult -ne 0) { exit `$CtlResult }
exit 0
"@

$Encoded = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($Elevated))
$Process = Start-Process pwsh -ArgumentList '-NoProfile', '-EncodedCommand', $Encoded -Verb RunAs -PassThru -Wait
if ($Process.ExitCode -eq 3010) {
    Write-Host 'Driver staged; reboot Windows to load the update.'
    exit 3010
}
if ($Process.ExitCode -ne 0) { throw "elevated update failed ($($Process.ExitCode))" }

Start-Sleep 2
& $Ctl status
if ($LASTEXITCODE -ne 0) { throw 'updated device did not become ready' }
