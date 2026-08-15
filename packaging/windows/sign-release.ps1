<#
Sign Windows release binaries with DineroLabs' Azure Artifact Signing
(Trusted Signing) account. Signs IN PLACE with Authenticode SHA256 + an
RFC3161 timestamp, then verifies. Publisher shown by Windows:
"Mirsad Hajdarevic" (Microsoft ID Verified, chained to the Microsoft
Identity Verification Root that ships in Windows).

One-time prereqs on the build box:
  1. Azure CLI:   winget install -e --id Microsoft.AzureCLI
  2. az login     (as haydarevich69@gmail.com - the account holding the
                   "Artifact Signing Certificate Profile Signer" role)
  3. signtool.exe from the Windows 10/11 SDK >= 10.0.22621
                  (installed with Visual Studio / Build Tools)
  4. .NET 8 runtime (the signing dlib needs it):
                  winget install -e --id Microsoft.DotNet.Runtime.8

Usage (from a Developer PowerShell, after building a release):
  .\sign-release.ps1 ..\..\build\dinero-qt.exe Dinero-Server-Setup.exe

The Microsoft.Trusted.Signing.Client package (the signtool dlib) is
downloaded automatically on first run and cached under %LOCALAPPDATA%.
Azure infra this talks to (provisioned 2026-08-15): account
"dinerolabs-signing" / profile "dinerolabs" in resource group
dinerolabs-signing, endpoint https://eus.codesigning.azure.net/ -
the same profile that signs the dinero-sv2 miner releases in CI.
#>
param(
    [Parameter(Mandatory = $true, ValueFromRemainingArguments = $true)]
    [string[]]$Files
)
$ErrorActionPreference = "Stop"

$Endpoint    = "https://eus.codesigning.azure.net/"
$Account     = "dinerolabs-signing"
$CertProfile = "dinerolabs"

# --- locate the newest signtool.exe from the Windows SDK ---
$signtool = Get-ChildItem "${env:ProgramFiles(x86)}\Windows Kits\10\bin\*\x64\signtool.exe" `
    -ErrorAction SilentlyContinue | Sort-Object FullName -Descending | Select-Object -First 1
if (-not $signtool) {
    throw "signtool.exe not found - install the Windows 10/11 SDK (ships with VS Build Tools)"
}

# --- fetch the Trusted Signing dlib once, cache locally ---
$toolDir = Join-Path $env:LOCALAPPDATA "TrustedSigning"
$dlib = Join-Path $toolDir "bin\x64\Azure.CodeSigning.Dlib.dll"
if (-not (Test-Path $dlib)) {
    Write-Host "Downloading Microsoft.Trusted.Signing.Client (first run only)..."
    New-Item -ItemType Directory -Force -Path $toolDir | Out-Null
    $zip = Join-Path $toolDir "Microsoft.Trusted.Signing.Client.zip"
    Invoke-WebRequest "https://www.nuget.org/api/v2/package/Microsoft.Trusted.Signing.Client" -OutFile $zip
    Expand-Archive $zip -DestinationPath $toolDir -Force
    Remove-Item $zip
}
if (-not (Test-Path $dlib)) { throw "Azure.CodeSigning.Dlib.dll missing after download ($dlib)" }

# --- metadata handed to the dlib (auth comes from `az login`) ---
$meta = Join-Path $toolDir "dinerolabs-metadata.json"
@{
    Endpoint               = $Endpoint
    CodeSigningAccountName = $Account
    CertificateProfileName = $CertProfile
} | ConvertTo-Json | Set-Content $meta -Encoding ascii

foreach ($f in $Files) {
    if (-not (Test-Path $f)) { throw "no such file: $f" }
    Write-Host "signing $f ..."
    & $signtool.FullName sign /v /fd SHA256 /tr "http://timestamp.acs.microsoft.com" /td SHA256 `
        /dlib $dlib /dmdf $meta $f
    if ($LASTEXITCODE -ne 0) { throw "signing FAILED for $f (is `az login` current?)" }
    & $signtool.FullName verify /pa /v $f
    if ($LASTEXITCODE -ne 0) { throw "post-sign verification FAILED for $f" }
}
Write-Host "Signed and verified $($Files.Count) file(s) as 'Mirsad Hajdarevic' via Azure Artifact Signing."
