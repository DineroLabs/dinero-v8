# Vendored Static libcurl on OpenSSL 3.5.7 (Windows MSVC) — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace vcpkg's dynamic libcurl (which drags OpenSSL 3.6.2 DLLs into the Windows package) with a vendored **static** libcurl linked against the vendored static OpenSSL 3.5.7, so `check_openssl_version.ps1` passes green on both Windows MSVC lanes.

**Architecture:** A new `scripts/build-curl-vendored.ps1` builds static `libcurl.lib` against `third_party/openssl-3.5.7/prebuilt/windows-x86_64-msvc`. A new `cmake/VendoredCurl.cmake` (included right after `ThirdParty.cmake`) defines an IMPORTED STATIC `CURL::libcurl` with the vendored OpenSSL static libs + system libs on its interface and `CURL_STATICLIB` defined. All `find_package(CURL)` sites are guarded so the vendored target wins on MSVC while Unix keeps `-lcurl`. Packaging scripts stop bundling the curl/openssl DLLs.

**Tech Stack:** Windows, MSVC (VS 2022 BuildTools), CMake (VS generator), PowerShell 5.1, curl source build (CMake), vendored static OpenSSL 3.5.7, NSIS.

## Global Constraints

- **Crypto baseline: OpenSSL exactly `3.5.7`.** No other OpenSSL version may appear in any shipped binary/DLL.
- **Curl must link the vendored static OpenSSL**, not vcpkg's and not system: pass `OPENSSL_ROOT_DIR` / `OPENSSL_*_LIBRARY` pointing at `third_party/openssl-3.5.7/prebuilt/windows-x86_64-msvc`.
- **Dynamic CRT (`/MD`)** — match the daemon (`MSVCP140.dll`). Curl build: `-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreadedDLL`.
- **No system/vcpkg fallback for release configs** — a missing vendored curl prebuilt is a hard CMake error (mirror the OpenSSL policy).
- **MSBuild node-reuse:** export `MSBUILDDISABLENODEREUSE=1` before every `cmake --build`.
- **Fresh build dirs when changing vendored deps** — wipe `build-msvc-server` / `build-msvc-native` before reconfigure (a stale `CMakeCache.txt` pins the old dep; this exact trap already bit the 3.5.6→3.5.7 switch).
- **Do not commit build dirs, `third_party/curl-*` sources, or installer artifacts.** Only source/script/cmake/doc files.
- Unix curl path (`set(DINERO_CURL_TARGET curl)`) must remain untouched.

---

## Preliminaries (one-time, before Task 1)

- [ ] **Commit the already-made assertion extension** (currently uncommitted on branch `win-openssl357-verify`), so the branch is clean before new work:

```bash
cd "C:/Users/Dina Hajdarevic/DineroLabs/dinero-v8"
git add scripts/ci/check_openssl_version.ps1
git commit -m "ci(windows): scan every bundled DLL/EXE for a non-baseline OpenSSL"
```

---

## Task 1: Vendoring script `scripts/build-curl-vendored.ps1`

**Files:**
- Create: `scripts/build-curl-vendored.ps1`

**Interfaces:**
- Produces: `third_party/curl-<ver>/prebuilt/windows-x86_64-msvc/lib/libcurl.lib`, `.../include/curl/*.h`, and `.../.dinero-build-meta` (`OS=Windows`, `ARCH=AMD64`, `CURL_VERSION=<ver>`, `OPENSSL_VERSION=3.5.7`, `BUILT_AT_UTC=...`).
- Consumes: vendored static OpenSSL at `third_party/openssl-3.5.7/prebuilt/windows-x86_64-msvc` (built by `scripts/build-openssl-vendored.ps1`).

- [ ] **Step 1: Pin the curl source SHA256.** Target **curl 8.21.0**. Fetch the authoritative hash and record it (do not guess):

```powershell
# One-off: read the official hash to paste into the script's map below.
Invoke-WebRequest -UseBasicParsing https://curl.se/download/curl-8.21.0.tar.gz.sha256 | Select-Object -ExpandProperty Content
```
Expected: a line like `<64-hex>  curl-8.21.0.tar.gz`. Use that hex in Step 2's `$KnownCurlSourceSha256`.

- [ ] **Step 2: Write the script.** Create `scripts/build-curl-vendored.ps1`:

```powershell
# Build vendored STATIC libcurl for DineroCoin — native Windows MSVC.
# Links the vendored static OpenSSL 3.5.7 so no libcurl/openssl DLLs ship.
$ErrorActionPreference = 'Stop'

$ScriptDir     = Split-Path -Parent $MyInvocation.MyCommand.Path
$ProjectRoot   = Split-Path -Parent $ScriptDir
$ThirdPartyDir = Join-Path $ProjectRoot 'third_party'
$CurlVersion    = if ($env:CURL_VERSION)    { $env:CURL_VERSION }    else { '8.21.0' }
$OpenSSLVersion = if ($env:OPENSSL_VERSION) { $env:OPENSSL_VERSION } else { '3.5.7' }
$CurlDir   = if ($env:CURL_SOURCE_DIR) { $env:CURL_SOURCE_DIR } else { Join-Path $ThirdPartyDir "curl-$CurlVersion" }
$OutputDir = if ($env:CURL_OUTPUT_DIR) { $env:CURL_OUTPUT_DIR } else { Join-Path $CurlDir 'prebuilt\windows-x86_64-msvc' }
$OpenSSLPrebuilt = Join-Path $ThirdPartyDir "openssl-$OpenSSLVersion\prebuilt\windows-x86_64-msvc"
$MetadataFile = Join-Path $OutputDir '.dinero-build-meta'
$Rebuild = $env:CURL_REBUILD -eq '1'
$KnownCurlSourceSha256 = @{
    '8.21.0' = 'd9b327997999045a24cda50f3983e69e51c516bd8be6ef9842fc7f99135e33bb'
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
& cmake --build $BuildDir --config Release --target libcurl -- /nodeReuse:false
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
```

- [ ] **Step 3: Run the script.**

```powershell
Set-Location 'C:\Users\Dina Hajdarevic\DineroLabs\dinero-v8'
.\scripts\build-curl-vendored.ps1
```
Expected: ends with `Vendored static libcurl ready: ...\third_party\curl-8.21.0\prebuilt\windows-x86_64-msvc\lib\libcurl.lib`.

- [ ] **Step 4: Verify the artifact.**

```powershell
$p = 'third_party\curl-8.21.0\prebuilt\windows-x86_64-msvc'
Test-Path "$p\lib\libcurl.lib"; Test-Path "$p\include\curl\curl.h"; Get-Content "$p\.dinero-build-meta"
```
Expected: `True`, `True`, and meta showing `CURL_VERSION=8.21.0` + `OPENSSL_VERSION=3.5.7`.

- [ ] **Step 5: Commit** (script only — not the downloaded source or prebuilt).

```bash
git add scripts/build-curl-vendored.ps1
git commit -m "build(windows): vendor static libcurl against static OpenSSL 3.5.7"
```

---

## Task 2: CMake integration — `cmake/VendoredCurl.cmake` + guarded call sites

**Files:**
- Create: `cmake/VendoredCurl.cmake`
- Modify: `CMakeLists.txt` (add `include(cmake/VendoredCurl.cmake)` after line 179; guard the `WIN32` block at 1112–1117)
- Modify: `miner/CMakeLists.txt:46`
- Modify: `tests/integration/CMakeLists.txt:60`

**Interfaces:**
- Consumes: Task 1's `third_party/curl-*/prebuilt/windows-x86_64-msvc/lib/libcurl.lib`; `OPENSSL_ROOT_DIR` and `DINERO_VENDORED_OPENSSL_VERSION` set by `ThirdParty.cmake`.
- Produces: IMPORTED STATIC target `CURL::libcurl` (interface: vendored `libssl.lib`;`libcrypto.lib`;system libs; `CURL_STATICLIB`). Consumers keep using `CURL::libcurl` / `${DINERO_CURL_TARGET}` unchanged.

- [ ] **Step 1: Create `cmake/VendoredCurl.cmake`:**

```cmake
# Vendored STATIC libcurl for the Windows MSVC lanes. Defines CURL::libcurl as
# an imported static target linked against the vendored static OpenSSL 3.5.7,
# so no libcurl/openssl DLLs are needed at runtime. Include AFTER ThirdParty.cmake
# (which sets OPENSSL_ROOT_DIR / DINERO_VENDORED_OPENSSL_VERSION) and BEFORE any
# curl consumer subdirectory.
option(DINERO_VENDORED_CURL "Use vendored static libcurl instead of find_package(CURL)" ${MSVC})
set(DINERO_VENDORED_CURL_DIR "" CACHE PATH "Override dir containing prebuilt vendored static libcurl")

if(DINERO_VENDORED_CURL AND NOT TARGET CURL::libcurl)
  if(DINERO_VENDORED_CURL_DIR)
    set(_curl_root "${DINERO_VENDORED_CURL_DIR}")
  else()
    # CORRECTED 2026-07-31 — do NOT discover the prebuilt with a glob.
    # file(GLOB curl-*) + list(SORT) + list(REVERSE) + list(GET 0) means
    # "lexicographically highest wins", which is not version ordering: with
    # curl-8.21.0, curl-8.11.1 and curl-8.9 present it picks curl-8.9 ("9" > "2").
    # Clean CI has one directory so this is invisible there, but a packaging
    # machine with a leftover curl-* tree would silently link an unintended,
    # possibly vulnerable, curl into a shipped binary.
    #
    # Resolve the EXACT pinned path and then verify what is in it:
    #   1. third_party/curl-${DINERO_VENDORED_CURL_VERSION}/prebuilt/windows-x86_64-msvc
    #   2. include/curl/curlver.h declares exactly that LIBCURL_VERSION
    #   3. .dinero-build-meta agrees on CURL_VERSION *and* OPENSSL_VERSION
    # The DINERO_VENDORED_CURL_DIR override is validated too, so it cannot be
    # used to smuggle in an unverified tree.
    set(_curl_root
      "${CMAKE_SOURCE_DIR}/third_party/curl-${DINERO_VENDORED_CURL_VERSION}/prebuilt/windows-x86_64-msvc")
    # ... see cmake/VendoredCurl.cmake for the three gates in full.

  add_library(CURL::libcurl STATIC IMPORTED GLOBAL)
  set_target_properties(CURL::libcurl PROPERTIES
    IMPORTED_LOCATION "${_curl_lib}"
    INTERFACE_INCLUDE_DIRECTORIES "${_curl_inc}"
    INTERFACE_COMPILE_DEFINITIONS "CURL_STATICLIB"
    INTERFACE_LINK_LIBRARIES "${_ossl}/libssl.lib;${_ossl}/libcrypto.lib;ws2_32;crypt32;wldap32;bcrypt;normaliz;advapi32")
  set(CURL_FOUND TRUE)
  message(STATUS "Using vendored STATIC libcurl: ${_curl_lib}")
  message(STATUS "  linked against vendored OpenSSL ${DINERO_VENDORED_OPENSSL_VERSION}")
endif()
```

- [ ] **Step 2: Include it after `ThirdParty.cmake`.** In `CMakeLists.txt`, immediately after line 179 (`include(cmake/ThirdParty.cmake)`), add:

```cmake
# Vendored static libcurl (MSVC) — must precede any curl consumer subdirectory.
include(cmake/VendoredCurl.cmake)
```

- [ ] **Step 3: Guard the top-level WIN32 block.** Replace `CMakeLists.txt:1112-1117` with:

```cmake
if(WIN32)
  if(NOT TARGET CURL::libcurl)
    find_package(CURL REQUIRED)
  endif()
  set(DINERO_CURL_TARGET CURL::libcurl)
else()
  set(DINERO_CURL_TARGET curl)
endif()
```

- [ ] **Step 4: Guard the miner site.** In `miner/CMakeLists.txt:46`, replace `find_package(CURL REQUIRED)` with:

```cmake
if(NOT TARGET CURL::libcurl)
  find_package(CURL REQUIRED)
endif()
```

- [ ] **Step 5: Guard the tests site.** In `tests/integration/CMakeLists.txt:60`, replace `find_package(CURL QUIET)` with:

```cmake
if(NOT TARGET CURL::libcurl)
  find_package(CURL QUIET)
endif()
```

- [ ] **Step 6: Verify configure picks the vendored static curl** (fresh server build dir):

```powershell
Set-Location 'C:\Users\Dina Hajdarevic\DineroLabs\dinero-v8'
if (Test-Path build-msvc-server) { Remove-Item build-msvc-server -Recurse -Force }
$env:MSBUILDDISABLENODEREUSE = '1'
cmake -S . -B build-msvc-server -G "Visual Studio 17 2022" -A x64 `
  -DDINERO_WINDOWS_SERVER_BUILD=ON -DDINERO_BUILD_SEEDER=ON -DDINERO_BUILD_MINER=OFF -DDINERO_ENABLE_QUIC=ON `
  -DCMAKE_TOOLCHAIN_FILE="$env:USERPROFILE\vcpkg\scripts\buildsystems\vcpkg.cmake" 2>&1 |
  Select-String -Pattern 'vendored STATIC libcurl|vendored OpenSSL 3\.5\.7'
```
Expected: prints `Using vendored STATIC libcurl: ...` and `linked against vendored OpenSSL 3.5.7`.

- [ ] **Step 7: Commit.**

```bash
git add cmake/VendoredCurl.cmake CMakeLists.txt miner/CMakeLists.txt tests/integration/CMakeLists.txt
git commit -m "build(windows): resolve CURL::libcurl to vendored static libcurl on MSVC"
```

---

## Task 3: Server lane — drop DLL bundling, rebuild, assert green

**Files:**
- Modify: `packaging/windows/build-server-installer.ps1` (the `$vcpkgDlls` list, ~L247)

**Interfaces:**
- Consumes: Task 2's configured `build-msvc-server`.
- Produces: a server build + installer whose payload contains no `libcurl.dll` / `libcrypto-3-x64.dll` / `libssl-3-x64.dll`.

- [ ] **Step 1: See the check fail first (red).** With the pre-change (vcpkg-curl) server build still present from earlier verification, the extended assertion fails on 3.6.2. Confirm the baseline red state is understood:

```powershell
# Against the OLD build (if still present) this prints the 3.6.2 offender.
# If build-msvc-server was already wiped in Task 2 Step 6, skip — the point is
# only to remember: RED = a bundled DLL embeds OpenSSL 3.6.2.
```
Expected understanding: RED = `libcrypto-3-x64.dll: OpenSSL 3.6.2` offender.

- [ ] **Step 2: Remove the DLL copy.** In `packaging/windows/build-server-installer.ps1`, change the `$vcpkgDlls` line (~L247) from:

```powershell
$vcpkgDlls = 'libcurl.dll','libcrypto-3-x64.dll','libssl-3-x64.dll','z.dll'
```
to (static curl+openssl+zlib-off means none are needed):

```powershell
$vcpkgDlls = @()
```

- [ ] **Step 3: Rebuild the server lane + installer.**

```powershell
Set-Location 'C:\Users\Dina Hajdarevic\DineroLabs\dinero-v8'
$env:MSBUILDDISABLENODEREUSE = '1'
.\packaging\windows\build-server-installer.ps1 -Version '8.0.18-openssl357-ci' -BuildDir 'build-msvc-server'
```
Expected: `Server installer ready` (exit 0). Because `build-msvc-server` was freshly configured in Task 2, it links the vendored static curl.

- [ ] **Step 4: Run the extended assertion — expect GREEN.**

```powershell
.\scripts\ci\check_openssl_version.ps1 -BuildDir 'build-msvc-server' -ExpectedVersion '3.5.7'
```
Expected: `libcrypto embeds exactly OpenSSL 3.5.7`, `dinerod embeds exactly OpenSSL 3.5.7`, `scanned bundled DLLs/EXEs across ... payload root(s); N embed an OpenSSL version, all exactly 3.5.7`, `openssl version assertion OK ... (daemon + bundled payload)`, exit 0.

- [ ] **Step 5: Confirm no curl/openssl DLL imports and none in the stage.**

```powershell
$dumpbin = (Get-ChildItem "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC" -Recurse -Filter dumpbin.exe | Where-Object FullName -match 'Hostx64\\x64' | Select-Object -First 1).FullName
foreach ($b in 'dinerod.exe','dinero-wallet-cli.exe','dinero-miner.exe') {
  $deps = & $dumpbin /dependents "build-msvc-server\Release\$b" | Select-String 'libcurl|libcrypto|libssl'
  "$b -> $(if($deps){$deps}else{'no curl/openssl DLL imports'})"
}
Get-ChildItem 'packaging\windows\dist\server-installer-stage' -Filter '*.dll' | Select-Object -ExpandProperty Name
```
Expected: each binary → `no curl/openssl DLL imports`; the stage lists **no** `libcurl.dll`/`libcrypto-3-x64.dll`/`libssl-3-x64.dll`.

- [ ] **Step 6: Commit.**

```bash
git add packaging/windows/build-server-installer.ps1
git commit -m "packaging(windows): stop bundling curl/openssl DLLs (server lane now fully static)"
```

---

## Task 4: User/GUI lane — drop DLL bundling, rebuild, verify

**Files:**
- Modify: `packaging/windows/build-installer.ps1` (remove curl/openssl DLL bundling; exact lines found in Step 1)

**Interfaces:**
- Consumes: Task 2's CMake wiring (applies to `build-msvc-native` too).
- Produces: a user installer payload with no curl/openssl DLLs; `dinero-qt` has no `libcurl.dll` runtime dependency.

- [ ] **Step 1: Locate the DLL bundling in the user installer script.**

```powershell
Select-String -Path 'packaging\windows\build-installer.ps1' -Pattern 'libcurl|libcrypto|libssl|windeployqt|vcpkg.*bin'
```
Expected: identifies where `libcurl.dll` / openssl DLLs are copied (either an explicit list like the server script, or via `windeployqt` copying `dinero-qt`'s DLL deps).

- [ ] **Step 2: Remove the explicit curl/openssl DLL copies** found in Step 1 (delete those filenames from the copy list, mirroring Task 3 Step 2). If the script relies solely on `windeployqt`, no edit is needed — the static `CURL::libcurl` target removes the runtime dependency, so `windeployqt` won't copy `libcurl.dll`. Record which case applies in the commit message.

- [ ] **Step 3: Rebuild the user lane fresh** (heavy — Qt, `--parallel 2`).

```powershell
Set-Location 'C:\Users\Dina Hajdarevic\DineroLabs\dinero-v8'
if (Test-Path build-msvc-native) { Remove-Item build-msvc-native -Recurse -Force }
$env:MSBUILDDISABLENODEREUSE = '1'
cmake -S . -B build-msvc-native -G "Visual Studio 17 2022" -A x64 `
  -DDINERO_ENABLE_QUIC=ON -DCMAKE_TOOLCHAIN_FILE="$env:USERPROFILE\vcpkg\scripts\buildsystems\vcpkg.cmake"
cmake --build build-msvc-native --config Release --target dinerod dinero-cli dinero-wallet-cli --parallel 4 -- /nodeReuse:false
cmake --build build-msvc-native --config Release --target dinero-qt --parallel 2 -- /nodeReuse:false
```
Expected: builds succeed; configure log shows `Using vendored STATIC libcurl`.

- [ ] **Step 4: Verify Qt deploy carries no libcurl.dll.**

```powershell
Select-String -Path 'build-msvc-native\.qt\QtDeployTargets-Release.cmake' -Pattern 'libcurl' -SimpleMatch
& $dumpbin /dependents 'build-msvc-native\bin\Release\dinero-qt.exe' | Select-String 'libcurl|libcrypto|libssl'
```
Expected: no `libcurl` match in the deploy manifest; `dinero-qt.exe` has no curl/openssl DLL imports.

- [ ] **Step 5: Build the user installer and scan its stage for DLLs.**

```powershell
.\packaging\windows\build-installer.ps1 -Version '8.0.18-openssl357-ci' -QtBuildDir 'C:\Users\Dina Hajdarevic\DineroLabs\dinero-v8\build-msvc-native\bin\Release'
# then inspect the user installer stage dir it reports for libcurl/libcrypto/libssl DLLs (expect none)
```
Expected: installer builds; no curl/openssl DLLs in the user stage.

- [ ] **Step 6: Commit.**

```bash
git add packaging/windows/build-installer.ps1
git commit -m "packaging(windows): user lane fully static — drop curl/openssl DLLs"
```

---

## Task 5: Runtime + smoke verification (both lanes)

**Files:** none (verification only).

- [ ] **Step 1: Daemon regtest smoke (server lane).**

```powershell
.\scripts\native-msvc-regtest-smoke.ps1 -BuildDir 'build-msvc-server\Release' -TimeoutSeconds 90
```
Expected: `PASS: native MSVC dinerod starts, mines one regtest block, stops, restarts, and reloads the same tip.`

- [ ] **Step 2: wallet-cli runs (proves static curl+OpenSSL 3.5.7 links and loads).**

```powershell
& 'build-msvc-server\Release\dinero-wallet-cli.exe' --version
```
Expected: prints a version line, exit 0 (no missing-DLL error for libcurl/libcrypto).

- [ ] **Step 3: Exercise a curl-backed path.** Run the miner/wallet HTTP code path that uses libcurl against a local/regtest endpoint (the smallest command that issues an HTTPS/HTTP request through curl). Confirm it completes without a TLS/init error — proving the vendored static OpenSSL 3.5.7 initializes inside curl at runtime, not just at link.
Expected: the request path returns without a curl/OpenSSL initialization error.

- [ ] **Step 4: Final green assertion (server lane) as the acceptance gate.**

```powershell
.\scripts\ci\check_openssl_version.ps1 -BuildDir 'build-msvc-server' -ExpectedVersion '3.5.7'
"EXIT=$LASTEXITCODE"
```
Expected: `openssl version assertion OK ... (daemon + bundled payload)` and `EXIT=0`.

- [ ] **Step 5: Update the CLAUDE.md build note.** In `CLAUDE.md`, update the OpenSSL/QUIC bullet to state the baseline is **3.5.7** and that curl is now vendored static (no libcurl/openssl DLLs shipped). Commit:

```bash
git add CLAUDE.md
git commit -m "docs: note vendored static libcurl + OpenSSL 3.5.7 baseline on Windows"
```

---

## Self-Review

**Spec coverage:**
- Vendoring script → Task 1. ✓
- `cmake/VendoredCurl.cmake` + `find_package` guards + `CMakeLists.txt` wiring → Task 2 (covers the extra miner/tests sites the spec's "consumers" list implied). ✓
- Packaging DLL removal, both lanes → Task 3 (server) + Task 4 (user). ✓
- Verification (assertion green, dumpbin, wallet-cli, curl path, daemon smoke, stage scan) → Task 3 Step 4–5, Task 4 Step 4–5, Task 5. ✓
- zlib/`z.dll`: resolved to `CURL_ZLIB=OFF` (Task 1) + `$vcpkgDlls=@()` drops `z.dll` (Task 3). ✓
- Static link order (curl→ssl→crypto→system): encoded in `INTERFACE_LINK_LIBRARIES` (Task 2 Step 1). ✓
- Out-of-scope (Unix parity) correctly omitted. ✓

**Placeholder scan:** The only deferred value is the curl SHA256, made an explicit fetch-and-pin action (Task 1 Steps 1–2), not a silent TODO. Task 4 Step 2 has a documented branch (explicit list vs windeployqt) resolved by Step 1's grep — a real conditional, not vagueness.

**Type/name consistency:** `CURL::libcurl`, `DINERO_CURL_TARGET`, `DINERO_VENDORED_CURL`, `DINERO_VENDORED_CURL_DIR`, `third_party/curl-<ver>/prebuilt/windows-x86_64-msvc/lib/libcurl.lib`, and `$vcpkgDlls` used consistently across tasks. Assertion output strings match the extended `check_openssl_version.ps1`.
