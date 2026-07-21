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
$Elevated = "pnputil /add-driver '$Inf' /install; & '$Ctl' remove-device; Start-Sleep 2; & '$Ctl' install-device"

$Process = Start-Process pwsh -ArgumentList '-NoProfile', '-Command', $Elevated -Verb RunAs -PassThru -Wait
if ($Process.ExitCode -ne 0) { throw "elevated update failed ($($Process.ExitCode))" }

Start-Sleep 2
& $Ctl status
