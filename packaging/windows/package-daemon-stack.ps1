# Package the native MSVC daemon stack into a headless operator ZIP.
#
# Output: packaging/windows/dist/dinero-<VERSION>-windows-x86_64-msvc.zip
# Contents:
#   bin/                        - 7 daemon/operator binaries
#     dinerod.exe, dinero-cli.exe, dinero-miner.exe,
#     dinero-stratum-worker.exe, dinero-gpu-miner.exe,
#     dinero-wallet-cli.exe, dinero-seeder.exe
#   LICENSE
#   README.txt                  - short Windows-MSVC-build notes
#   SHA256SUMS.txt              - per-file hash manifest
#
# A second SHA256 (of the ZIP itself) is printed to stdout for the
# release page. This is the Windows counterpart to the Linux/macOS
# operator archives: no GUI wallet, just command-line node tools.
#
# Usage from a normal PowerShell:
#   .\packaging\windows\package-daemon-stack.ps1
#   .\packaging\windows\package-daemon-stack.ps1 -Version v2.2.6-rc2-msvc1
#   .\packaging\windows\package-daemon-stack.ps1 -BuildDir build-msvc-native -Version v0.9.8-msvc
#
# Note: PS 5.1 default file encoding for Set-Content/Out-File is UTF-16 LE.
# When this script writes README.txt and SHA256SUMS.txt it explicitly
# specifies ASCII so a downstream sha256sum -c works without surprises.
# No non-ASCII chars in this file itself, also for PS 5.1 parser safety.

[CmdletBinding()]
param(
    [string]$BuildDir = 'build-msvc-native',
    [string]$Version  = '',
    [string]$OutputDir = '',
    # DineroLabs/dinero-sv2 release-build output. Defaults to the conventional
    # sibling-repo layout. If the two SV2 miner binaries are present they go
    # into bin/ alongside the daemon stack; missing → soft-warn and skip.
    [string]$Sv2BuildDir = ''
)

$ErrorActionPreference = 'Stop'

$ScriptDir   = Split-Path -Parent $MyInvocation.MyCommand.Path
$ProjectRoot = (Resolve-Path (Join-Path $ScriptDir '..\..')).Path

if (-not [System.IO.Path]::IsPathRooted($BuildDir)) {
    $BuildDir = Join-Path $ProjectRoot $BuildDir
}
$ReleaseBinDir = Join-Path $BuildDir 'Release'

if (-not $OutputDir) {
    $OutputDir = Join-Path $ScriptDir 'dist'
}

# Derive a version string. Prefer explicit -Version, else git-describe,
# else the short commit. Strip leading "v" then add it back so the
# archive name is always vX.Y.Z-... (matches Linux v0.9.7 form).
if (-not $Version) {
    try {
        Push-Location $ProjectRoot
        $Version = (& git describe --tags --always 2>$null).Trim()
        if (-not $Version) {
            $Version = "g$(& git rev-parse --short HEAD)"
        }
    } finally {
        Pop-Location
    }
}
$Version = $Version.TrimStart('v','V')

Write-Host '----------------------------------------------------------'
Write-Host "Packaging Dinero operator stack -- windows-x86_64-msvc"
Write-Host '----------------------------------------------------------'
Write-Host "Version:    v$Version"
Write-Host "Build dir:  $BuildDir"
Write-Host "Output dir: $OutputDir"

# Verify the command-line operator binaries exist. The GUI wallet ships
# in the Windows user installer; this ZIP is for node operators and
# power users that want the raw daemon stack.
$Binaries = @(
    'dinerod.exe',
    'dinero-cli.exe',
    'dinero-miner.exe',
    'dinero-stratum-worker.exe',
    'dinero-gpu-miner.exe',
    'dinero-wallet-cli.exe',
    'dinero-seeder.exe'
)

function Resolve-OperatorBinaryPath([string]$BinaryName) {
    $primary = Join-Path $ReleaseBinDir $BinaryName
    if (Test-Path $primary) {
        return $primary
    }
    if ($BinaryName -eq 'dinero-seeder.exe') {
        $seederPath = Join-Path $BuildDir 'seeder\Release\dinero-seeder.exe'
        if (Test-Path $seederPath) {
            return $seederPath
        }
    }
    return $primary
}

$missing = @()
foreach ($b in $Binaries) {
    $p = Resolve-OperatorBinaryPath $b
    if (-not (Test-Path $p)) { $missing += $b }
}
if ($missing.Count -gt 0) {
    Write-Host "ERROR: Missing binaries in ${ReleaseBinDir}:" -ForegroundColor Red
    $missing | ForEach-Object { Write-Host "  - $_" -ForegroundColor Red }
    Write-Host "Build them with:" -ForegroundColor Red
    Write-Host "  cmake --build $BuildDir --config Release --target dinerod dinero-cli dinero-miner dinero-stratum-worker dinero-gpu-miner dinero-wallet-cli dinero-seeder" -ForegroundColor Yellow
    exit 1
}
Write-Host "All 7 daemon/operator binaries found." -ForegroundColor Green

# dinero-sv2 binaries are OPTIONAL: bundled when present, soft-warn-skip
# when absent. Operator cuts on a builder without a Rust toolchain still
# produce a valid ZIP without the SV2 Pool lane.
if (-not $Sv2BuildDir) {
    $Sv2BuildDir = (Join-Path (Split-Path $ProjectRoot -Parent) 'dinero-sv2\target\release')
} elseif (-not [System.IO.Path]::IsPathRooted($Sv2BuildDir)) {
    $Sv2BuildDir = Join-Path $ProjectRoot $Sv2BuildDir
}
$Sv2Binaries = @('dinero-sv2-miner.exe', 'dinero-sv2-gpu-miner.exe')
$Sv2Present  = @()
foreach ($s in $Sv2Binaries) {
    $p = Join-Path $Sv2BuildDir $s
    if (Test-Path $p) {
        $Sv2Present += $s
    } else {
        Write-Host "  NOTE: optional SV2 binary $s not found at $Sv2BuildDir (skipping)" -ForegroundColor Yellow
    }
}

$StageName = "dinero-v$Version-windows-x86_64-msvc"
$StageDir  = Join-Path $OutputDir $StageName
$BinSubDir = Join-Path $StageDir 'bin'

if (Test-Path $StageDir) {
    Write-Host "Removing prior stage: $StageDir"
    Remove-Item -Path $StageDir -Recurse -Force
}
New-Item -Path $BinSubDir -ItemType Directory -Force | Out-Null

Write-Host 'Copying binaries...'
foreach ($b in $Binaries) {
    $src = Resolve-OperatorBinaryPath $b
    $dst = Join-Path $BinSubDir $b
    Copy-Item -Path $src -Destination $dst
    $sz = (Get-Item $dst).Length
    Write-Host ('  bin\{0}  {1} bytes' -f $b, $sz)
}
foreach ($b in $Sv2Present) {
    $src = Join-Path $Sv2BuildDir $b
    $dst = Join-Path $BinSubDir $b
    Copy-Item -Path $src -Destination $dst
    $sz = (Get-Item $dst).Length
    Write-Host ('  bin\{0}  {1} bytes' -f $b, $sz)
}

# Top-level LICENSE + README.txt. LICENSE pulled from repo root.
$LicensePath = Join-Path $ProjectRoot 'LICENSE'
if (Test-Path $LicensePath) {
    Copy-Item -Path $LicensePath -Destination (Join-Path $StageDir 'LICENSE')
    Write-Host '  LICENSE'
} else {
    Write-Host '  WARNING: LICENSE not found at repo root.' -ForegroundColor Yellow
}

$builtOn = (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ')
$readmeLines = @(
    "Dinero v$Version -- windows-x86_64-msvc operator archive",
    "========================================",
    "",
    "Build:      Microsoft Visual Studio 2022 (MSVC 14.44), x86_64",
    "Built UTC:  $builtOn",
    "Linkage:    Vendored OpenSSL 3.5.7 (static, no-shared)",
    "Source:     https://github.com/DineroLabs/dinero-v8",
    "",
    "Included operator binaries (bin/):",
    "  dinerod.exe                Full node daemon",
    "  dinero-cli.exe             CLI RPC client",
    "  dinero-miner.exe           CPU miner",
    "  dinero-stratum-worker.exe  Stratum worker client",
    "  dinero-gpu-miner.exe       GPU miner (OpenCL; runtime unvalidated on Windows)",
    "  dinero-wallet-cli.exe      Reference wallet CLI",
    "  dinero-seeder.exe          DNS seeder / peer crawler"
)
if ($Sv2Present -contains 'dinero-sv2-miner.exe') {
    $readmeLines += "  dinero-sv2-miner.exe       Stratum V2 pool miner (CPU; DineroLabs/dinero-sv2)"
}
if ($Sv2Present -contains 'dinero-sv2-gpu-miner.exe') {
    $readmeLines += "  dinero-sv2-gpu-miner.exe   Stratum V2 pool miner (GPU; OpenCL + CUDA on NVIDIA)"
}
$readmeLines += @(
    "",
    "Not included:",
    "  dinero-qt                  Qt6 GUI wallet (use the Windows installer)",
    "  dinero-solo-miner          Desktop/user mining tool",
    "  dinero-stratum (server)    separate binary",
    "",
    "Quick start:",
    "  cd bin",
    "  dinerod.exe                       # mainnet, default datadir %APPDATA%\dinero",
    "  dinerod.exe -regtest               # local regression test network",
    "  dinero-cli.exe getblockchaininfo   # query a running daemon",
    "",
    "SHA256SUMS.txt at the archive root has per-file hashes for verification.",
    "The archive itself has its own SHA256, listed on the GitHub release page."
)
$readmeLines -join "`r`n" | Set-Content -Path (Join-Path $StageDir 'README.txt') -Encoding ASCII
Write-Host '  README.txt'

# SHA256SUMS.txt. Format mirrors sha256sum(1) so the file can be
# verified on POSIX with sha256sum -c.
Write-Host 'Computing SHA256SUMS...'
$sums = New-Object System.Collections.Generic.List[string]
Push-Location $StageDir
try {
    $files = @('README.txt', 'LICENSE')
    foreach ($b in $Binaries) {
        $files += "bin/$b"
    }
    foreach ($b in $Sv2Present) {
        $files += "bin/$b"
    }
    foreach ($rel in $files) {
        $full = $rel -replace '/', [IO.Path]::DirectorySeparatorChar
        if (-not (Test-Path $full)) { continue }
        $hash = (Get-FileHash -Path $full -Algorithm SHA256).Hash.ToLower()
        $line = "$hash  $rel"
        $sums.Add($line)
        Write-Host "  $line"
    }
    ($sums -join "`n") + "`n" | Set-Content -Path 'SHA256SUMS.txt' -Encoding ASCII
} finally {
    Pop-Location
}

$ZipPath = Join-Path $OutputDir ($StageName + '.zip')
if (Test-Path $ZipPath) { Remove-Item -Path $ZipPath -Force }

Write-Host "Creating $ZipPath ..."
Compress-Archive -Path (Join-Path $StageDir '*') -DestinationPath $ZipPath -CompressionLevel Optimal

$zipHash = (Get-FileHash -Path $ZipPath -Algorithm SHA256).Hash.ToLower()
$zipSize = (Get-Item $ZipPath).Length
$zipMB   = [math]::Round($zipSize / 1MB, 2)

Write-Host ''
Write-Host '----------------------------------------------------------'
Write-Host 'Release archive ready' -ForegroundColor Green
Write-Host '----------------------------------------------------------'
Write-Host "  Path:   $ZipPath"
Write-Host "  Size:   $zipSize bytes ($zipMB MB)"
Write-Host "  SHA256: $zipHash"
Write-Host ''
Write-Host 'Upload to a GitHub release:' -ForegroundColor Cyan
Write-Host "  gh release upload v$Version `"$ZipPath`" --repo DineroLabs/dinero-v8" -ForegroundColor Cyan
