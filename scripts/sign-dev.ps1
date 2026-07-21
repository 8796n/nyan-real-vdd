# Signs the driver package in out\package with a self-signed development /
# distribution certificate (signing path (1) in docs/signing.ja.md).
#
# Creates the certificate on first run (CurrentUser\My) and exports the
# public part to out\nyanvdd-dev.cer — installers must add that .cer to the
# machine's Root and TrustedPublisher stores (scripts\install.ps1 does).
#
# For store-clean distribution, replace this step with EV + Microsoft
# attestation signing; nothing else in the pipeline changes.

param(
    [string]$CertSubject = 'CN=nyan Real Driver Publisher',
    [string]$PackageDir = '',
    [switch]$NoTimestamp
)

$ErrorActionPreference = 'Stop'
$RepoRoot = Split-Path -Parent $PSScriptRoot
if (-not $PackageDir) { $PackageDir = Join-Path $RepoRoot 'out\package' }

if (-not (Test-Path (Join-Path $PackageDir 'nyanvdd.inf'))) {
    throw "driver package not found in $PackageDir — run scripts\build.ps1 first"
}

# --- locate WDK/SDK tools ---
$KitsBin = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\bin'
$SignTool = Get-ChildItem -Path $KitsBin -Recurse -Filter signtool.exe -ErrorAction SilentlyContinue |
    Where-Object { $_.FullName -match '\\x64\\' } | Sort-Object FullName -Descending | Select-Object -First 1
$Inf2Cat = Get-ChildItem -Path $KitsBin -Recurse -Filter Inf2Cat.exe -ErrorAction SilentlyContinue |
    Sort-Object FullName -Descending | Select-Object -First 1
if (-not $SignTool) { throw 'signtool.exe not found under Windows Kits' }
if (-not $Inf2Cat) { throw 'Inf2Cat.exe not found under Windows Kits (WDK required)' }

# --- get or create the signing certificate ---
$Cert = Get-ChildItem Cert:\CurrentUser\My -CodeSigningCert |
    Where-Object { $_.Subject -eq $CertSubject } | Select-Object -First 1
if (-not $Cert) {
    Write-Host "creating self-signed certificate: $CertSubject"
    $Cert = New-SelfSignedCertificate -Type CodeSigningCert -Subject $CertSubject `
        -CertStoreLocation Cert:\CurrentUser\My -KeyExportPolicy Exportable `
        -NotAfter (Get-Date).AddYears(10) -HashAlgorithm SHA256
}
$CerPath = Join-Path $RepoRoot 'out\nyanvdd-dev.cer'
Export-Certificate -Cert $Cert -FilePath $CerPath | Out-Null

# --- sign the binary, regenerate the catalog, sign the catalog ---
$TimestampArgs = @()
if (-not $NoTimestamp) {
    $TimestampArgs = @('/tr', 'http://timestamp.digicert.com', '/td', 'sha256')
}

& $SignTool.FullName sign /fd sha256 /sha1 $Cert.Thumbprint @TimestampArgs (Join-Path $PackageDir 'nyanvdd.dll')
if ($LASTEXITCODE -ne 0) { throw 'signtool failed on nyanvdd.dll' }

# /uselocaltime: DriverVer is stamped with the local date, but Inf2Cat checks
# it against UTC by default and rejects it as postdated during the first hours
# of a local day in east-of-UTC time zones.
& $Inf2Cat.FullName /driver:$PackageDir /os:10_x64 /uselocaltime
if ($LASTEXITCODE -ne 0) { throw 'Inf2Cat failed' }

& $SignTool.FullName sign /fd sha256 /sha1 $Cert.Thumbprint @TimestampArgs (Join-Path $PackageDir 'nyanvdd.cat')
if ($LASTEXITCODE -ne 0) { throw 'signtool failed on nyanvdd.cat' }

Write-Host ''
Write-Host "OK: package signed with '$CertSubject'"
Write-Host "    public cert exported to $CerPath"
