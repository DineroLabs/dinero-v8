# Build vendored OpenSSL for DineroCoin - native Windows MSVC.
#
# Companion to scripts/build-openssl-vendored.sh (Linux/macOS). Produces
# the same artifacts (libcrypto + libssl) the bash script does, in the
# configured output directory:
#   libcrypto.lib
#   libssl.lib
# plus a .dinero-build-meta file the CMake guard at line ~752 cross-checks.
#
# Prerequisites the script verifies:
#   * Perl on PATH (Strawberry Perl recommended; Configure has run on
#     others but stdlib modules are easier to get right with Strawberry).
#   * Visual Studio Build Tools 2019+ (cl.exe + nmake). Auto-located via
#     vswhere.exe so the script works from a regular PowerShell - no need
#     to launch a "Developer Command Prompt" first.
#   * NASM is optional. If missing the build falls back to `no-asm` (slower
#     SHA / AES paths, still consensus-correct).
#
# Usage (from a normal PowerShell):
#   .\scripts\build-openssl-vendored.ps1
#
# Build a specific OpenSSL version/source/output:
#   $env:OPENSSL_VERSION = '3.5.6'
#   $env:OPENSSL_OUTPUT_DIR = 'C:\path\to\openssl-prebuilt'
#   .\scripts\build-openssl-vendored.ps1
#
# Force a clean rebuild:
#   $env:OPENSSL_REBUILD = '1'; .\scripts\build-openssl-vendored.ps1
#
# Exit codes: 0 on success, non-zero on any failure (script aborts early).

$ErrorActionPreference = 'Stop'

$ScriptDir   = Split-Path -Parent $MyInvocation.MyCommand.Path
$ProjectRoot = Split-Path -Parent $ScriptDir
$ThirdPartyDir = Join-Path $ProjectRoot 'third_party'
$OpenSSLVersion = if ($env:OPENSSL_VERSION) { $env:OPENSSL_VERSION } else { '3.5.6' }
$OpenSSLDir = if ($env:OPENSSL_SOURCE_DIR) {
    $env:OPENSSL_SOURCE_DIR
} else {
    Join-Path $ThirdPartyDir "openssl-$OpenSSLVersion"
}
$OutputDir = if ($env:OPENSSL_OUTPUT_DIR) { $env:OPENSSL_OUTPUT_DIR } else { $OpenSSLDir }
$MetadataFile = Join-Path $OutputDir '.dinero-build-meta'
$Rebuild     = $env:OPENSSL_REBUILD -eq '1'
$KnownOpenSSLSourceSha256 = @{
    '3.5.6' = 'deae7c80cba99c4b4f940ecadb3c3338b13cb77418409238e57d7f31f2a3b736'
}

function Write-Header($msg) {
    Write-Host ''
    Write-Host '----------------------------------------------------------'
    Write-Host $msg
    Write-Host '----------------------------------------------------------'
}

function Fail($msg) {
    Write-Host "ERROR: $msg" -ForegroundColor Red
    exit 1
}

function Ensure-OpenSSLSource {
    if (Test-Path $OpenSSLDir) {
        return
    }

    if ($env:OPENSSL_SOURCE_DIR) {
        Fail "OPENSSL_SOURCE_DIR was provided but does not exist: $OpenSSLDir"
    }

    if (-not $KnownOpenSSLSourceSha256.ContainsKey($OpenSSLVersion)) {
        Fail "OpenSSL source is missing at $OpenSSLDir and no pinned SHA256 is known for OPENSSL_VERSION=$OpenSSLVersion"
    }

    New-Item -ItemType Directory -Path $ThirdPartyDir -Force | Out-Null
    $Tarball = Join-Path $ThirdPartyDir "openssl-$OpenSSLVersion.tar.gz"
    $Url = "https://github.com/openssl/openssl/releases/download/openssl-$OpenSSLVersion/openssl-$OpenSSLVersion.tar.gz"

    if (-not (Test-Path $Tarball)) {
        Write-Host "Downloading OpenSSL $OpenSSLVersion source..."
        Invoke-WebRequest -UseBasicParsing -Uri $Url -OutFile $Tarball
    }

    $ActualSha256 = (Get-FileHash -Algorithm SHA256 -Path $Tarball).Hash.ToLowerInvariant()
    $ExpectedSha256 = $KnownOpenSSLSourceSha256[$OpenSSLVersion]
    if ($ActualSha256 -ne $ExpectedSha256) {
        Fail "SHA256 mismatch for $Tarball. Expected $ExpectedSha256, got $ActualSha256"
    }

    Write-Host "Extracting OpenSSL $OpenSSLVersion source..."
    & tar.exe -xzf $Tarball -C $ThirdPartyDir
    if ($LASTEXITCODE -ne 0) {
        Fail "tar extraction failed with code $LASTEXITCODE"
    }
    if (-not (Test-Path $OpenSSLDir)) {
        Fail "OpenSSL extraction completed but expected directory is missing: $OpenSSLDir"
    }
}

Write-Header "Building vendored OpenSSL $OpenSSLVersion for DineroCoin (Windows MSVC)"

Ensure-OpenSSLSource
New-Item -ItemType Directory -Path $OutputDir -Force | Out-Null
Write-Host "  Source: $OpenSSLDir"
Write-Host "  Output: $OutputDir"

# ----------------------------------------------------------------------
# Locate Perl. Prefer Strawberry Perl over Git-bash's bundled minimal
# Perl — the latter is missing modules Configure occasionally pulls in
# (Text::Template, File::Spec edge cases on the perlasm paths).
# ----------------------------------------------------------------------
Write-Host 'Locating Perl...'
$PerlExe = $null

# Probe in preference order: Strawberry (most reliable for OpenSSL),
# then `perl` already on PATH, then Git for Windows's bundled Perl
# (which lives in usr/bin/ — NOT exposed via system PATH; only `cmd/`
# is. So we have to look directly.).
$PerlCandidates = @(
    'C:\Strawberry\perl\bin\perl.exe',
    "$env:ProgramFiles\Strawberry\perl\bin\perl.exe",
    "$env:ProgramFiles\Git\usr\bin\perl.exe",
    "${env:ProgramFiles(x86)}\Git\usr\bin\perl.exe"
) | Where-Object { Test-Path $_ } | Select-Object -First 1

if ($PerlCandidates) {
    $PerlExe = $PerlCandidates
} else {
    $PerlCmd = Get-Command perl -ErrorAction SilentlyContinue
    if ($PerlCmd) { $PerlExe = $PerlCmd.Source }
}

if (-not $PerlExe) {
    Fail 'No Perl found. Install Strawberry Perl from https://strawberryperl.com/ and retry.'
}

if ($PerlExe -match 'Strawberry') {
    Write-Host "  Strawberry Perl: $PerlExe"
} elseif ($PerlExe -match 'Git\\usr\\bin') {
    Write-Host "  Found Git-for-Windows bundled Perl: $PerlExe"
    Write-Host "  NOTE: OpenSSL's Configure works most reliably with Strawberry Perl." -ForegroundColor Yellow
    Write-Host "  If this run fails with 'Can''t locate ...pm', install Strawberry Perl from strawberryperl.com and retry." -ForegroundColor Yellow
} else {
    Write-Host "  Perl: $PerlExe"
}

# ----------------------------------------------------------------------
# Locate Visual Studio Build Tools and bootstrap the cl.exe/nmake env
# into this PowerShell session via vcvars64.bat. vswhere is the
# Microsoft-supported way to find an arbitrary VS install non-interactively.
# ----------------------------------------------------------------------
Write-Host 'Locating Visual Studio Build Tools (x64)...'

# If cl.exe is already on PATH the user is in a Developer shell; skip bootstrap.
$ClCmd = Get-Command cl.exe -ErrorAction SilentlyContinue
if ($ClCmd) {
    Write-Host "  cl.exe already on PATH ($($ClCmd.Source)); reusing current environment."
} else {
    $VsWhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (-not (Test-Path $VsWhere)) {
        Fail "vswhere.exe not found at $VsWhere. Install Visual Studio 2019+ Build Tools."
    }
    $VsInstall = & $VsWhere -latest -property installationPath -products * `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
        | Select-Object -First 1
    if (-not $VsInstall) {
        Fail 'No Visual Studio install with MSVC x64 toolset found via vswhere.'
    }
    $VcVars = Join-Path $VsInstall 'VC\Auxiliary\Build\vcvars64.bat'
    if (-not (Test-Path $VcVars)) {
        Fail "vcvars64.bat not found at $VcVars (under $VsInstall)."
    }
    Write-Host "  Importing env from $VcVars"
    # Run vcvars64 in a cmd shim, dump the resulting env, and replay it into
    # the current PowerShell session. Standard pattern — vcvars64 only
    # affects the cmd it runs in, so we capture and forward.
    $envBlock = & cmd /c "`"$VcVars`" >nul 2>&1 && set"
    foreach ($line in $envBlock) {
        if ($line -match '^([^=]+)=(.*)$') {
            Set-Item -Path "Env:\$($Matches[1])" -Value $Matches[2]
        }
    }
    $ClCmd = Get-Command cl.exe -ErrorAction SilentlyContinue
    if (-not $ClCmd) {
        Fail 'cl.exe still not on PATH after importing vcvars64. Check VS Build Tools install.'
    }
    Write-Host "  cl.exe now available: $($ClCmd.Source)"
}

$NmakeCmd = Get-Command nmake.exe -ErrorAction SilentlyContinue
if (-not $NmakeCmd) {
    Fail 'nmake.exe not on PATH after VS env import. Check VS Build Tools install.'
}

# ----------------------------------------------------------------------
# NASM is optional. Without it Configure must be told no-asm.
# ----------------------------------------------------------------------
$NasmCmd = Get-Command nasm.exe -ErrorAction SilentlyContinue
$NoAsm = $false
if ($NasmCmd) {
    Write-Host "  NASM: $($NasmCmd.Source)"
} else {
    Write-Host '  NASM not found on PATH. Falling back to no-asm.' -ForegroundColor Yellow
    Write-Host '    (Optional: install NASM from https://www.nasm.us/ for AES/SHA hardware acceleration.)' -ForegroundColor Yellow
    $NoAsm = $true
}

# ----------------------------------------------------------------------
# Clean (only when asked). OpenSSL leaves a Makefile + configdata.pm; if
# the previous build was a different config (e.g. shared instead of
# no-shared), Configure refuses to overwrite without distclean.
# ----------------------------------------------------------------------
Push-Location $OpenSSLDir
try {
    if ($Rebuild) {
        Write-Host 'OPENSSL_REBUILD=1 -- running nmake distclean...'
        # Don't use 2>&1 here: on PS 5.1 it wraps each stderr line in an
        # ErrorRecord and the $ErrorActionPreference='Stop' at the top of
        # this script trips on the first one. nmake distclean's stderr
        # ("don't know how to make ...") is also harmless when there's no
        # prior Makefile to clean, which is the common case.
        $oldEAP = $ErrorActionPreference
        $ErrorActionPreference = 'Continue'
        try {
            & nmake.exe distclean *> $null
        } catch { }
        $ErrorActionPreference = $oldEAP
        Get-ChildItem -Path . -Recurse -Include '*.obj','*.lib','*.pdb' -ErrorAction SilentlyContinue `
            | Remove-Item -Force -ErrorAction SilentlyContinue
        Remove-Item -LiteralPath (Join-Path $OutputDir 'libcrypto.lib') -Force -ErrorAction SilentlyContinue
        Remove-Item -LiteralPath (Join-Path $OutputDir 'libssl.lib') -Force -ErrorAction SilentlyContinue
    }

    # ------------------------------------------------------------------
    # Configure. Flags mirror the bash script's CONFIGURE_FLAGS so the
    # resulting libcrypto/libssl have the same crypto feature set on
    # every platform.
    # ------------------------------------------------------------------
    Write-Header 'Configuring OpenSSL (VC-WIN64A)...'
    $ConfigureFlags = @(
        'VC-WIN64A',
        'no-shared',
        'no-tests',
        'no-apps',
        'enable-ec',
        'enable-ecdh',
        'enable-ecdsa'
    )
    if ($NoAsm) { $ConfigureFlags += 'no-asm' }

    # PS 5.1 wraps every native-command stderr line in an ErrorRecord
    # under $ErrorActionPreference='Stop' and aborts. Both perl Configure
    # and nmake build_libs write progress to stderr, so guard them
    # explicitly and judge success by $LASTEXITCODE instead.
    $oldEAP = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        & $PerlExe Configure @ConfigureFlags
        $configureExit = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $oldEAP
    }
    if ($configureExit -ne 0) {
        Fail "perl Configure exited with code $configureExit"
    }

    # ------------------------------------------------------------------
    # Build. nmake without /J is single-threaded; OpenSSL's Makefile
    # doesn't always tolerate parallel nmake on Windows reliably (the
    # Unix Makefile has correct deps; the nmake variant historically
    # has subtle races). Use serial.
    # ------------------------------------------------------------------
    Write-Header 'Building OpenSSL with nmake (this takes 5-10 minutes)...'
    $oldEAP = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        & nmake.exe build_libs
        $buildExit = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $oldEAP
    }
    if ($buildExit -ne 0) {
        Fail "nmake build_libs exited with code $buildExit"
    }

    # ------------------------------------------------------------------
    # Verify output. VC-WIN64A no-shared produces libcrypto.lib and
    # libssl.lib at the source root.
    # ------------------------------------------------------------------
    if (-not (Test-Path 'libcrypto.lib')) {
        Fail "Build appeared to succeed but libcrypto.lib is missing in $OpenSSLDir."
    }
    if (-not (Test-Path 'libssl.lib')) {
        Fail "Build appeared to succeed but libssl.lib is missing in $OpenSSLDir."
    }

    $crypto = Get-Item 'libcrypto.lib'
    $ssl    = Get-Item 'libssl.lib'

    if ((Resolve-Path $OutputDir).Path -ne (Resolve-Path $OpenSSLDir).Path) {
        Copy-Item -LiteralPath $crypto.FullName -Destination (Join-Path $OutputDir 'libcrypto.lib') -Force
        Copy-Item -LiteralPath $ssl.FullName -Destination (Join-Path $OutputDir 'libssl.lib') -Force
        $crypto = Get-Item (Join-Path $OutputDir 'libcrypto.lib')
        $ssl = Get-Item (Join-Path $OutputDir 'libssl.lib')
    }

    Write-Host ''
    Write-Host 'Libraries created:' -ForegroundColor Green
    Write-Host ("  libcrypto.lib: {0:N0} bytes" -f $crypto.Length)
    Write-Host ("  libssl.lib:    {0:N0} bytes" -f $ssl.Length)

    # ------------------------------------------------------------------
    # Write build metadata. CMakeLists.txt cross-checks OS/ARCH at
    # configure time and refuses to use a vendored libcrypto that was
    # built for a different platform — prevents accidentally linking a
    # Linux libcrypto into a Windows build after switching machines.
    # ------------------------------------------------------------------
    $now = (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ')
    $meta = @"
OS=Windows
ARCH=AMD64
OPENSSL_VERSION=$OpenSSLVersion
SOURCE_DIR=$OpenSSLDir
BUILT_AT_UTC=$now
"@
    Set-Content -Path $MetadataFile -Value $meta -Encoding ASCII -NoNewline:$false
    Write-Host "  Metadata: $MetadataFile"
}
finally {
    Pop-Location
}

Write-Header 'Vendored OpenSSL ready for DineroCoin Windows MSVC builds'
Write-Host 'You can now (re-)configure the DineroCoin build:' -ForegroundColor Green
Write-Host "  cmake -S . -B build-msvc-native -G `"Visual Studio 17 2022`" -A x64 -DDINERO_VENDORED_OPENSSL_DIR=`"$OutputDir`" -DDINERO_VENDORED_OPENSSL_SOURCE_DIR=`"$OpenSSLDir`"" -ForegroundColor Cyan
Write-Host '  cmake --build build-msvc-native --config Release --target dinerod dinero-cli' -ForegroundColor Cyan
