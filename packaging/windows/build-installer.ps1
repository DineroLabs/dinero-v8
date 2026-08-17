# Stage all the daemon binaries + dinero-qt + Qt6 runtime via
# windeployqt, then run makensis to produce the Windows user installer.
# By default this also emits the headless operator zip via
# package-daemon-stack.ps1 so release day produces both Windows artifacts:
#
#   dist/Dinero-<VERSION>-windows-x86_64-Setup.exe
#   dist/dinero-v<VERSION>-windows-x86_64-msvc.zip
#
# Phase 3 of the v8.0.0 monorepo consolidation: defaults now point at
# the in-tree qt/ subdir build output (build-msvc-native\qt\Release)
# instead of the pre-consolidation sibling repo (..\dinero-qt). Operators
# building from a pre-v8 layout can still override via -QtBuildDir /
# -QtRepoDir to point at the old sibling-repo paths.
#
# Prerequisites (one-time per machine):
#   - Visual Studio 2022 Build Tools (provides cl.exe + nmake)
#   - Qt 6.9.x MSVC2022 64-bit at C:\Qt\6.9.x\msvc2022_64
#       (override with -QtBin if installed elsewhere)
#   - NSIS at "C:\Program Files (x86)\NSIS\makensis.exe"
#       (winget install NSIS.NSIS)
#   - The Dinero monorepo stack already built under
#       build-msvc-native\ via:
#       cmake -S . -B build-msvc-native -G "Visual Studio 17 2022" -A x64 ^
#             -DCMAKE_BUILD_TYPE=Release ^
#             -DDINERO_BUILD_QT=ON -DDINERO_BUILD_MINER=ON ^
#             -DMINER_ENABLE_CUDA=ON -DMINER_ENABLE_OPENCL=ON ^
#             -DENABLE_GPU_MINING=ON -DENABLE_GRPC=OFF ^
#             -DENABLE_HARDWARE_WALLETS=OFF ^
#             -DDINERO_ENABLE_QUIC=ON ^
#             -DCMAKE_PREFIX_PATH="C:\Qt\6.9.1\msvc2022_64"
#       cmake --build build-msvc-native --config Release ^
#             --target dinerod dinero-cli dinero-solo-miner-cli dinero-qt ^
#                      dinero-seeder dinero-miner dinero-stratum-worker ^
#                      dinero-gpu-miner dinero-wallet-cli
#
# Usage:
#   .\packaging\windows\build-installer.ps1 -Version 8.0.0
#   .\packaging\windows\build-installer.ps1 -Version 8.0.0-rc1
#   .\packaging\windows\build-installer.ps1 -QtBin "C:\Qt\6.9.2\msvc2022_64\bin"

[CmdletBinding()]
param(
    [string]$Version       = '8.0.0-dev',
    [string]$DaemonBuildDir = '',
    [string]$QtBuildDir    = '',
    [string]$QtRepoDir     = '',
    [string]$QtBin         = 'C:\Qt\6.9.1\msvc2022_64\bin',
    [string]$VcpkgBin      = "$env:USERPROFILE\vcpkg\installed\x64-windows\bin",
    [string]$Makensis      = 'C:\Program Files (x86)\NSIS\makensis.exe',
    # DineroLabs/dinero-sv2 release-build output. Defaults to the conventional
    # sibling-repo layout (..\dinero-sv2\target\release). If the two SV2 miner
    # binaries are present, they're bundled alongside dinero-qt.exe so the
    # qt UI's SV2 Pool tab discovers them out-of-the-box. Missing -> soft-warn
    # and skip (operator cuts without a Rust toolchain still produce a valid
    # installer, just without the SV2 Pool lane).
    [string]$Sv2BuildDir   = '',
    [switch]$SkipOperatorZip,
    # AssumeUTXO primary + exact-lifecycle fallback bundled next to dinero-qt.exe.
    # Their installed names are derived from qt/src/main.cpp, preventing drift
    # between the package and the paths the GUI passes to dinerod. Source
    # precedence is explicit path > dist cache > release download. The fallback
    # stays on the release that originally published the previous desktop anchor.
    [string]$SnapshotFileName   = '',
    [string]$SnapshotPath       = '',
    [string]$SnapshotManifestPath = '',
    [string]$SnapshotReleaseTag = 'v8.1.2',
    [string]$SnapshotFallbackFileName   = '',
    [string]$SnapshotFallbackPath       = '',
    [string]$SnapshotFallbackManifestPath = '',
    [string]$SnapshotFallbackReleaseTag = 'v8.0.17',
    [switch]$NoSnapshot
)

$ErrorActionPreference = 'Stop'

$ScriptDir   = Split-Path -Parent $MyInvocation.MyCommand.Path
$ProjectRoot = (Resolve-Path (Join-Path $ScriptDir '..\..')).Path

if (-not $DaemonBuildDir) {
    $DaemonBuildDir = Join-Path $ProjectRoot 'build-msvc-native\Release'
}
if (-not $QtBuildDir) {
    # v8 monorepo: dinero-qt is built in-tree under qt/. The Visual Studio
    # generator's multi-config output puts it at <build>\qt\Release\.
    $QtBuildDir = Join-Path $ProjectRoot 'build-msvc-native\qt\Release'
}
if (-not $QtRepoDir) {
    # v8 monorepo: qt/ is a subdir of the dinero repo, not a sibling.
    $QtRepoDir = Join-Path $ProjectRoot 'qt'
}

$Stage   = Join-Path $ScriptDir 'dist\installer-stage'
$DistDir = Join-Path $ScriptDir 'dist'

# Derive both installed filenames from the GUI's compiled-in constants.
if ((-not $SnapshotFileName -or -not $SnapshotFallbackFileName) -and -not $NoSnapshot) {
    $qtMain = Join-Path $ProjectRoot 'qt\src\main.cpp'
    if (Test-Path $qtMain) {
        if (-not $SnapshotFileName) {
            $m = Select-String -Path $qtMain -Pattern 'kBundledSnapshotPrimary\[\]\s*=\s*"([^"]+)"' | Select-Object -First 1
            if ($m) { $SnapshotFileName = $m.Matches[0].Groups[1].Value }
        }
        if (-not $SnapshotFallbackFileName) {
            $m = Select-String -Path $qtMain -Pattern 'kBundledSnapshotFallback\[\]\s*=\s*"([^"]+)"' | Select-Object -First 1
            if ($m) { $SnapshotFallbackFileName = $m.Matches[0].Groups[1].Value }
        }
        Write-Host "Bundled snapshot names (from qt/src/main.cpp): primary=$SnapshotFileName fallback=$SnapshotFallbackFileName"
    }
}

function Resolve-BundledSnapshot {
    param(
        [string]$FileName,
        [string]$ExplicitPath,
        [string]$ReleaseTag,
        [string]$Role
    )
    if ($NoSnapshot -or -not $FileName) { return $null }
    if ($ExplicitPath) {
        if (Test-Path $ExplicitPath) { return (Resolve-Path $ExplicitPath).Path }
        Write-Host "WARN: $Role snapshot path not found ($ExplicitPath)" -ForegroundColor Yellow
        return $null
    }
    if (-not (Test-Path $DistDir)) { New-Item $DistDir -ItemType Directory | Out-Null }
    $cached = Join-Path $DistDir $FileName
    if (Test-Path $cached) { return $cached }
    if ($ReleaseTag) {
        $url = "https://github.com/DineroLabs/dinero-v8/releases/download/$ReleaseTag/$FileName"
        Write-Host "Downloading $Role AssumeUTXO snapshot from $url..."
        try { Invoke-WebRequest $url -OutFile $cached -ErrorAction Stop; return $cached }
        catch { Write-Host "WARN: $Role snapshot download failed ($_)" -ForegroundColor Yellow; return $null }
    }
    Write-Host "WARN: no source for $Role snapshot $FileName" -ForegroundColor Yellow
    return $null
}

function Resolve-BundledSnapshotManifest {
    param(
        [string]$FileName,
        [string]$SnapshotSourcePath,
        [string]$ExplicitManifestPath,
        [string]$ReleaseTag,
        [string]$Role
    )
    if ($NoSnapshot -or -not $FileName -or -not $SnapshotSourcePath) { return $null }
    if ($ExplicitManifestPath) {
        if (Test-Path $ExplicitManifestPath) { return (Resolve-Path $ExplicitManifestPath).Path }
        Write-Host "WARN: $Role manifest path not found ($ExplicitManifestPath)" -ForegroundColor Yellow
        return $null
    }

    # Historical assets used both <snapshot>.dat.manifest.json and
    # <snapshot>.manifest.json. Accept either source name, but always stage it
    # beside the installed data as <installed snapshot>.manifest.json.
    $sourceCandidates = @(
        "$SnapshotSourcePath.manifest.json",
        (([IO.Path]::Combine([IO.Path]::GetDirectoryName($SnapshotSourcePath),
            ([IO.Path]::GetFileName($SnapshotSourcePath) -replace '\.dat$', '.manifest.json'))))
    ) | Select-Object -Unique
    foreach ($candidate in $sourceCandidates) {
        if (Test-Path $candidate) { return (Resolve-Path $candidate).Path }
    }

    if ($ReleaseTag) {
        $assetCandidates = @(
            "$FileName.manifest.json",
            ($FileName -replace '\.dat$', '.manifest.json')
        ) | Select-Object -Unique
        foreach ($asset in $assetCandidates) {
            $cached = Join-Path $DistDir $asset
            if (Test-Path $cached) { return $cached }
            $url = "https://github.com/DineroLabs/dinero-v8/releases/download/$ReleaseTag/$asset"
            try {
                Invoke-WebRequest $url -OutFile $cached -ErrorAction Stop
                return $cached
            } catch {
                if (Test-Path $cached) { Remove-Item $cached -Force }
            }
        }
    }
    Write-Host "WARN: no adjacent manifest found for $Role snapshot $FileName" -ForegroundColor Yellow
    return $null
}

function Assert-SnapshotManifest {
    param(
        [string]$DataPath,
        [string]$ManifestPath,
        [string]$InstalledName,
        [string]$Role
    )
    $manifest = (Get-Content -Raw $ManifestPath | ConvertFrom-Json).snapshot
    if ($manifest.snapshot_file -ne $InstalledName) {
        throw "$Role manifest snapshot_file '$($manifest.snapshot_file)' does not match '$InstalledName'"
    }
    $actualHash = (Get-FileHash -Algorithm SHA256 $DataPath).Hash.ToLowerInvariant()
    if ([string]$manifest.sha256 -ne $actualHash) {
        throw "$Role snapshot sha256 '$actualHash' does not match manifest '$($manifest.sha256)'"
    }
    $actualBytes = (Get-Item $DataPath).Length
    if ([int64]$manifest.bytes -ne $actualBytes) {
        throw "$Role snapshot length '$actualBytes' does not match manifest '$($manifest.bytes)'"
    }
    Write-Host "Verified $Role snapshot manifest: $actualBytes bytes, sha256 $actualHash"
}

Write-Host '----------------------------------------------------------'
Write-Host "Building Dinero installer -- v$Version"
Write-Host '----------------------------------------------------------'

# Sanity checks
$qtExe = Join-Path $QtBuildDir 'dinero-qt.exe'
if (-not (Test-Path $qtExe)) {
    Write-Host "ERROR: dinero-qt.exe not found at $qtExe" -ForegroundColor Red
    Write-Host "Build it first: cmake --build $(Split-Path $QtBuildDir) --config Release --target dinero-qt" -ForegroundColor Yellow
    exit 1
}
$daemonBinaries = 'dinerod','dinero-cli','dinero-miner','dinero-stratum-worker','dinero-gpu-miner','dinero-wallet-cli'
foreach ($b in $daemonBinaries) {
    if (-not (Test-Path (Join-Path $DaemonBuildDir "$b.exe"))) {
        Write-Host "ERROR: $b.exe not found in $DaemonBuildDir" -ForegroundColor Red
        exit 1
    }
}
$BuildRoot = Split-Path $DaemonBuildDir -Parent

# Default the dinero-sv2 release-build dir to the conventional sibling-repo
# layout. The qt UI's discoverSv2MinerPath() checks for these binaries
# adjacent to dinero-qt.exe first (see dinero-qt mainwindow.cpp:494) - so
# dropping them into the stage is all that's needed for the SV2 Pool tab
# to work out-of-the-box on a fresh install.
if (-not $Sv2BuildDir) {
    $Sv2BuildDir = (Join-Path (Split-Path $ProjectRoot -Parent) 'dinero-sv2\target\release')
} elseif (-not [System.IO.Path]::IsPathRooted($Sv2BuildDir)) {
    $Sv2BuildDir = Join-Path $ProjectRoot $Sv2BuildDir
}

$optionalBinaries = @(
    @{ Name = 'dinero-solo-miner';     Path = Join-Path $BuildRoot 'miner\Release\dinero-solo-miner.exe' },
    @{ Name = 'dinero-seeder';         Path = Join-Path $BuildRoot 'seeder\Release\dinero-seeder.exe' },
    @{ Name = 'dinero-sv2-miner';      Path = Join-Path $Sv2BuildDir 'dinero-sv2-miner.exe' },
    @{ Name = 'dinero-sv2-gpu-miner';  Path = Join-Path $Sv2BuildDir 'dinero-sv2-gpu-miner.exe' }
)
$windeployqt = Join-Path $QtBin 'windeployqt.exe'
if (-not (Test-Path $windeployqt)) {
    Write-Host "ERROR: windeployqt.exe not found at $windeployqt (override with -QtBin)" -ForegroundColor Red
    exit 1
}
if (-not (Test-Path $Makensis)) {
    Write-Host "ERROR: makensis.exe not found at $Makensis (override with -Makensis)" -ForegroundColor Red
    exit 1
}

# Stage
if (Test-Path $Stage) {
    Write-Host "Cleaning prior stage..."
    Remove-Item $Stage -Recurse -Force
}
New-Item $Stage -ItemType Directory | Out-Null

Write-Host "Copying dinero-qt + daemon binaries..."
Copy-Item $qtExe (Join-Path $Stage 'dinero-qt.exe')
foreach ($b in $daemonBinaries) {
    Copy-Item (Join-Path $DaemonBuildDir "$b.exe") (Join-Path $Stage "$b.exe")
}
foreach ($entry in $optionalBinaries) {
    $optionalPath = $entry['Path']
    $optionalName = $entry['Name']
    if (Test-Path $optionalPath) {
        Copy-Item $optionalPath (Join-Path $Stage "$optionalName.exe")
        Write-Host "  $optionalName.exe"
    } else {
        Write-Host "  WARNING: optional $optionalName.exe not found at $optionalPath" -ForegroundColor Yellow
    }
}
Copy-Item (Join-Path $ProjectRoot 'LICENSE') (Join-Path $Stage 'LICENSE')

# Bundle the dinero-qt window icon. mainwindow.cpp calls
# resolveBundledAssetPath("Dinero-Coin.png") which checks first for an
# adjacent file before any Qt resource fallback, so the icon needs to
# live next to dinero-qt.exe.
$iconSrc = Join-Path $QtRepoDir 'Dinero-Coin.png'
if (Test-Path $iconSrc) {
    Copy-Item $iconSrc (Join-Path $Stage 'Dinero-Coin.png')
    Write-Host '  Dinero-Coin.png (window icon)'
} else {
    Write-Host "  WARNING: $iconSrc not found; window icon will be missing." -ForegroundColor Yellow
}

# User lane is fully static via vendored libcurl+OpenSSL 3.5.7 (Task 4).
# The CURL::libcurl CMake target (cmake/VendoredCurl.cmake) links against
# static .lib files so no libcurl/openssl/zlib DLLs are needed at runtime.
# windeployqt handles Qt DLLs; everything else is Windows system or VC++ runtime.
Write-Host "Copying runtime DLLs from vcpkg (if required)..."
$vcpkgDlls = @()  # user lane is fully static (vendored libcurl+OpenSSL 3.5.7 + zlib-off)
foreach ($d in $vcpkgDlls) {
    $src = Join-Path $VcpkgBin $d
    if (Test-Path $src) {
        Copy-Item $src (Join-Path $Stage $d)
        Write-Host "  $d"
    } else {
        Write-Host "  WARNING: $d not found at $src" -ForegroundColor Yellow
    }
}

# Bundle the Microsoft Visual C++ 2015-2022 x64 Redistributable and let the NSIS
# installer run it (see EnsureVCRedist in dinero-installer.nsi). Qt6Core.dll +
# dinero-qt.exe depend on VCRUNTIME140/MSVCP140; without the redist a clean
# machine fails to launch with a "Qt6Core.dll" error. Reuse the cached copy in
# dist\ (shared with build-server-installer.ps1), else fetch the official one.
$vcRedist = Join-Path $DistDir 'vc_redist.x64.exe'
if (-not (Test-Path $vcRedist)) {
    Write-Host 'Downloading vc_redist.x64.exe...'
    Invoke-WebRequest -UseBasicParsing 'https://aka.ms/vs/17/release/vc_redist.x64.exe' -OutFile $vcRedist
}
Copy-Item $vcRedist (Join-Path $Stage 'vc_redist.x64.exe')
Write-Host ("  vc_redist.x64.exe ({0:N1} MB)" -f ((Get-Item $vcRedist).Length / 1MB))

Write-Host "Running windeployqt..."
$oldEAP = $ErrorActionPreference
$ErrorActionPreference = 'Continue'
try {
    & $windeployqt --release --no-translations (Join-Path $Stage 'dinero-qt.exe')
} finally {
    $ErrorActionPreference = $oldEAP
}

# Bundle the newest primary and the previous desktop anchor. The latter is not
# chosen on a fresh install; it exists only so an interrupted older lifecycle can
# restart after an installer upgrade replaces Program Files.
$snap = Resolve-BundledSnapshot $SnapshotFileName $SnapshotPath $SnapshotReleaseTag 'primary'
if ($snap) {
    Copy-Item $snap (Join-Path $Stage $SnapshotFileName)
    Write-Host ("Bundled AssumeUTXO snapshot: {0} ({1:N1} MB)" -f $SnapshotFileName, ((Get-Item $snap).Length / 1MB))
    $manifest = Resolve-BundledSnapshotManifest $SnapshotFileName $snap $SnapshotManifestPath $SnapshotReleaseTag 'primary'
    if ($manifest) {
        Assert-SnapshotManifest $snap $manifest $SnapshotFileName 'primary'
        Copy-Item $manifest (Join-Path $Stage "$SnapshotFileName.manifest.json")
        Write-Host "Bundled AssumeUTXO primary manifest"
    } else {
        throw 'Primary snapshot has no adjacent manifest; refusing to produce the installer.'
    }
} else {
    Write-Host "No AssumeUTXO snapshot bundled - fresh GUI users will sync from genesis." -ForegroundColor Yellow
}
$fallbackSnap = Resolve-BundledSnapshot $SnapshotFallbackFileName $SnapshotFallbackPath $SnapshotFallbackReleaseTag 'fallback'
if ($fallbackSnap) {
    Copy-Item $fallbackSnap (Join-Path $Stage $SnapshotFallbackFileName)
    Write-Host ("Bundled AssumeUTXO fallback: {0} ({1:N1} MB)" -f $SnapshotFallbackFileName, ((Get-Item $fallbackSnap).Length / 1MB))
    $fallbackManifest = Resolve-BundledSnapshotManifest $SnapshotFallbackFileName $fallbackSnap $SnapshotFallbackManifestPath $SnapshotFallbackReleaseTag 'fallback'
    if ($fallbackManifest) {
        Assert-SnapshotManifest $fallbackSnap $fallbackManifest $SnapshotFallbackFileName 'fallback'
        Copy-Item $fallbackManifest (Join-Path $Stage "$SnapshotFallbackFileName.manifest.json")
        Write-Host "Bundled AssumeUTXO fallback manifest"
    } else {
        throw 'Fallback snapshot has no adjacent manifest; refusing to produce the installer.'
    }
} elseif (-not $NoSnapshot) {
    Write-Host "WARN: no exact-lifecycle fallback bundled - interrupted older desktop syncs cannot rehydrate after upgrade." -ForegroundColor Yellow
}

$totalSize = (Get-ChildItem $Stage -Recurse | Measure-Object Length -Sum).Sum
$fileCount = (Get-ChildItem $Stage -Recurse -File).Count
Write-Host ("Stage: $fileCount files, {0:N2} MB" -f ($totalSize / 1MB))

Write-Host "Running makensis (LZMA solid compression, takes ~1 min)..."
$numericVersion = if ($Version -match '^(\d+)\.(\d+)\.(\d+)') {
    "$($Matches[1]).$($Matches[2]).$($Matches[3]).0"
} else {
    '0.0.0.0'
}
Push-Location $ScriptDir
try {
    $oldEAP = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        & $Makensis "/DVERSION=$Version" "/DVERSION_NUMERIC=$numericVersion" 'dinero-installer.nsi' | Out-Null
    } finally {
        $ErrorActionPreference = $oldEAP
    }
} finally {
    Pop-Location
}

$installerPath = Join-Path $ScriptDir "dist\Dinero-$Version-windows-x86_64-Setup.exe"
if (-not (Test-Path $installerPath)) {
    Write-Host "ERROR: makensis ran but $installerPath not produced." -ForegroundColor Red
    exit 1
}
$installerSize = (Get-Item $installerPath).Length
$installerHash = (Get-FileHash $installerPath -Algorithm SHA256).Hash.ToLower()

Write-Host ''
Write-Host '----------------------------------------------------------'
Write-Host 'Installer ready' -ForegroundColor Green
Write-Host '----------------------------------------------------------'
Write-Host "  Path:   $installerPath"
Write-Host ("  Size:   $installerSize bytes ({0:N2} MB)" -f ($installerSize / 1MB))
Write-Host "  SHA256: $installerHash"

if (-not $SkipOperatorZip) {
    Write-Host ''
    Write-Host 'Producing Windows operator zip...'
    & (Join-Path $ScriptDir 'package-daemon-stack.ps1') `
        -BuildDir $BuildRoot `
        -Version $Version `
        -OutputDir (Join-Path $ScriptDir 'dist')
}
