# Build vendored STATIC libcurl for DineroCoin — native Windows MSVC.
# Links the vendored static OpenSSL 3.5.7 so no libcurl/openssl DLLs ship.
$ErrorActionPreference = 'Stop'

$ScriptDir     = Split-Path -Parent $MyInvocation.MyCommand.Path
$ProjectRoot   = Split-Path -Parent $ScriptDir
$ThirdPartyDir = Join-Path $ProjectRoot 'third_party'
$CurlVersion    = if ($env:CURL_VERSION)    { $env:CURL_VERSION }    else { '8.11.1' }
$OpenSSLVersion = if ($env:OPENSSL_VERSION) { $env:OPENSSL_VERSION } else { '3.5.7' }
$CurlDir   = if ($env:CURL_SOURCE_DIR) { $env:CURL_SOURCE_DIR } else { Join-Path $ThirdPartyDir "curl-$CurlVersion" }
$OutputDir = if ($env:CURL_OUTPUT_DIR) { $env:CURL_OUTPUT_DIR } else { Join-Path $CurlDir 'prebuilt\windows-x86_64-msvc' }
$OpenSSLPrebuilt = Join-Path $ThirdPartyDir "openssl-$OpenSSLVersion\prebuilt\windows-x86_64-msvc"
$MetadataFile = Join-Path $OutputDir '.dinero-build-meta'
$Rebuild = $env:CURL_REBUILD -eq '1'
$KnownCurlSourceSha256 = @{
    '8.11.1' = 'a889ac9dbba3644271bd9d1302b5c22a088893719b72be3487bc3d401e5c4e80'
}

function Fail($m) { Write-Host "ERROR: $m" -ForegroundColor Red; exit 1 }

if (-not (Test-Path (Join-Path $OpenSSLPrebuilt 'libcrypto.lib'))) {
    Fail "Vendored static OpenSSL $OpenSSLVersion not found at $OpenSSLPrebuilt. Run scripts/build-openssl-vendored.ps1 first."
}

# --- Ensure source (download + SHA-pin + extract) ---
if (-not (Test-Path $CurlDir)) {
    if (-not $KnownCurlSourceSha256.ContainsKey($CurlVersion)) { Fail "No pinned SHA256 for curl $CurlVersion" }
    New-Item -ItemType Directory -Path $ThirdPartyDir -Force | Out-Null
    $Tarball = Join-Path $ThirdPartyDir "curl-$CurlVersion.tar.gz"
    $Url = "https://curl.se/download/curl-$CurlVersion.tar.gz"
    if (-not (Test-Path $Tarball)) { Invoke-WebRequest -UseBasicParsing -Uri $Url -OutFile $Tarball }
    $actual = (Get-FileHash -Algorithm SHA256 -Path $Tarball).Hash.ToLowerInvariant()
    if ($actual -ne $KnownCurlSourceSha256[$CurlVersion]) { Fail "SHA256 mismatch for $Tarball (got $actual)" }
    & tar.exe -xzf $Tarball -C $ThirdPartyDir
    if ($LASTEXITCODE -ne 0) { Fail "tar extraction failed" }
}
New-Item -ItemType Directory -Path $OutputDir -Force | Out-Null

# --- Bootstrap MSVC env if cl.exe not already on PATH (vswhere + vcvars64) ---
if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
    $VsWhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (-not (Test-Path $VsWhere)) { Fail "vswhere.exe not found" }
    $VsInstall = & $VsWhere -latest -property installationPath -products * `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 | Select-Object -First 1
    $VcVars = Join-Path $VsInstall 'VC\Auxiliary\Build\vcvars64.bat'
    if (-not (Test-Path $VcVars)) { Fail "vcvars64.bat not found under $VsInstall" }
    $envBlock = & cmd /c "`"$VcVars`" >nul 2>&1 && set"
    foreach ($line in $envBlock) { if ($line -match '^([^=]+)=(.*)$') { Set-Item -Path "Env:\$($Matches[1])" -Value $Matches[2] } }
    if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) { Fail "cl.exe still not on PATH after vcvars64" }
}

# --- Configure + build static libcurl against the vendored static OpenSSL ---
$BuildDir = Join-Path $CurlDir 'build-msvc-static'
if ($Rebuild -and (Test-Path $BuildDir)) { Remove-Item $BuildDir -Recurse -Force }
$configureArgs = @(
    '-S', $CurlDir, '-B', $BuildDir, '-G', 'Visual Studio 17 2022', '-A', 'x64',
    '-DBUILD_SHARED_LIBS=OFF', '-DBUILD_CURL_EXE=OFF', '-DBUILD_STATIC_LIBS=ON',
    '-DCURL_USE_OPENSSL=ON', '-DCURL_USE_SCHANNEL=OFF',
    '-DCURL_ZLIB=OFF', '-DCURL_BROTLI=OFF', '-DCURL_ZSTD=OFF',
    '-DUSE_LIBIDN2=OFF', '-DCURL_USE_LIBSSH2=OFF', '-DCURL_USE_LIBPSL=OFF',
    '-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreadedDLL',
    "-DOPENSSL_ROOT_DIR=$OpenSSLPrebuilt",
    "-DOPENSSL_INCLUDE_DIR=$OpenSSLPrebuilt\include",
    "-DOPENSSL_CRYPTO_LIBRARY=$OpenSSLPrebuilt\libcrypto.lib",
    "-DOPENSSL_SSL_LIBRARY=$OpenSSLPrebuilt\libssl.lib"
)
& cmake @configureArgs; if ($LASTEXITCODE -ne 0) { Fail "curl configure failed" }
$env:MSBUILDDISABLENODEREUSE = '1'
# curl 8.11.1 CMake names the static-only target "libcurl_static" (not "libcurl")
& cmake --build $BuildDir --config Release --target libcurl_static -- /nodeReuse:false
if ($LASTEXITCODE -ne 0) { Fail "curl build failed" }

# --- Collect outputs (CMake emits the static archive as libcurl.lib under lib\Release) ---
$builtLib = Get-ChildItem -Path $BuildDir -Recurse -Filter 'libcurl*.lib' |
    Where-Object { $_.FullName -match '\\Release\\' } | Select-Object -First 1
if (-not $builtLib) { Fail "static libcurl.lib not found under $BuildDir" }
New-Item -ItemType Directory -Path (Join-Path $OutputDir 'lib') -Force | Out-Null
Copy-Item $builtLib.FullName (Join-Path $OutputDir 'lib\libcurl.lib') -Force

$incDst = Join-Path $OutputDir 'include\curl'
if (Test-Path $incDst) { Remove-Item $incDst -Recurse -Force }
New-Item -ItemType Directory -Path $incDst -Force | Out-Null
Copy-Item (Join-Path $CurlDir 'include\curl\*.h') $incDst -Force

$now = (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ')
@"
OS=Windows
ARCH=AMD64
CURL_VERSION=$CurlVersion
OPENSSL_VERSION=$OpenSSLVersion
BUILT_AT_UTC=$now
"@ | Set-Content -Path $MetadataFile -Encoding ASCII

Write-Host "Vendored static libcurl ready: $(Join-Path $OutputDir 'lib\libcurl.lib')" -ForegroundColor Green
