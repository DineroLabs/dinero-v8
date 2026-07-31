# Vendored static libcurl on OpenSSL 3.5.7 (Windows MSVC)

**Date:** 2026-07-30
**Status:** Approved design — pending implementation plan
**Related:** PR #419 (`deps/openssl-3.5.7-unified` — "one crypto baseline"), `scripts/ci/check_openssl_version.ps1` (bundled-DLL scan extension)

## Problem

PR #419 establishes OpenSSL 3.5.7 as the single crypto baseline "everywhere a
release configuration ships," enforced on Windows by
`scripts/ci/check_openssl_version.ps1`. That assertion was extended to scan
every bundled DLL/EXE and immediately exposed a violation: the Windows package
ships **two** OpenSSLs.

- `dinerod` and the other daemon/CLI binaries statically link the vendored
  OpenSSL **3.5.7** (correct baseline). Verified: `dumpbin /dependents dinerod.exe`
  shows no OpenSSL/curl DLL imports, only Windows system crypto (`bcrypt`, `CRYPT32`).
- On MSVC, `CMakeLists.txt:1113` resolves curl via `find_package(CURL REQUIRED)`
  → vcpkg's **dynamic** `libcurl.dll`, which links vcpkg's `libcrypto-3-x64.dll` /
  `libssl-3-x64.dll` — **OpenSSL 3.6.2**. Curl is consumed by `miner/`, `tools/`,
  `src/wallet/reference`, and `src/common`, so the miners, tools, and
  `dinero-wallet-cli` all drag the 3.6.2 DLLs into the shipped installer.

The `qt/CMakeLists.txt:689` "static curl" handling is MinGW-only; the MSVC lane
has no static-curl mechanism today.

## Goal

Rebuild curl as a **vendored static libcurl linked against the vendored static
OpenSSL 3.5.7**, so no `libcurl.dll` / `libcrypto-3-x64.dll` / `libssl-3-x64.dll`
ship on Windows. After the change, the only OpenSSL present anywhere in the
Windows package is the static 3.5.7 compiled into each executable, and the
extended `check_openssl_version.ps1` passes green.

**Scope:** both Windows MSVC lanes — `build-msvc-server` (headless) and
`build-msvc-native` (user/GUI). Unix (Linux/Mac) curl handling is out of scope
for this pass; a follow-up may extend `check_openssl_version.sh` for parity.

## Non-goals

- No change to the Unix curl path (`set(DINERO_CURL_TARGET curl)` for `-lcurl`).
- No change to consensus/daemon crypto (already static 3.5.7).
- No new curl features; keep the current HTTP(S) client surface the miners/wallet use.

## Design

### 1. Vendoring script — `scripts/build-curl-vendored.ps1`

Companion to `scripts/build-openssl-vendored.ps1`, same shape and conventions:

- Env knobs: `CURL_VERSION` (default a pinned stable, target curl 8.21.0 as of
  2026-07 — finalize exact version + published SHA256 at implementation and add
  to a `$KnownCurlSourceSha256` map, exactly like the OpenSSL script's map),
  `CURL_SOURCE_DIR`, `CURL_OUTPUT_DIR`, `CURL_REBUILD`, `OPENSSL_VERSION`
  (default 3.5.7 — locates the vendored static OpenSSL prebuilt).
- `Ensure-CurlSource`: download `curl-<ver>.tar.gz` from the pinned URL, verify
  SHA256 against the map, extract into `third_party/curl-<ver>`.
- Locate MSVC via `vswhere` + `vcvars64.bat` env replay (reuse the OpenSSL
  script's proven bootstrap block).
- Build with CMake (not nmake — curl's CMake build is the supported static path):
  - `-DBUILD_SHARED_LIBS=OFF -DBUILD_CURL_EXE=OFF -DBUILD_STATIC_LIBS=ON`
  - `-DCURL_USE_OPENSSL=ON -DCURL_USE_SCHANNEL=OFF`
  - `-DOPENSSL_ROOT_DIR=<repo>/third_party/openssl-3.5.7/prebuilt/windows-x86_64-msvc`
    (and `OPENSSL_INCLUDE_DIR` / `OPENSSL_CRYPTO_LIBRARY` / `OPENSSL_SSL_LIBRARY`
    pointed at the vendored static `.lib`s explicitly, so CMake never falls back
    to a system/vcpkg OpenSSL).
  - Dynamic CRT (`/MD`) to match the project (daemon links `MSVCP140.dll`).
  - `-DCURL_ZLIB=OFF` (drop the `z.dll` dependency) unless build/tests show a
    real need for transfer compression; if needed, static-link zlib instead.
    zlib is **not** OpenSSL and does not affect the crypto-baseline assertion —
    treated as a minor cleanup, not a blocker.
  - Narrow protocol surface (HTTP/HTTPS only) is acceptable and reduces size, but
    keep it conservative: only trim protocols confirmed unused by the miners/wallet.
- Output layout mirrors the OpenSSL prebuilt:
  `third_party/curl-<ver>/prebuilt/windows-x86_64-msvc/`
  containing `lib/libcurl.lib` (the static archive curl's CMake emits — normalize
  the name if CMake emits `libcurl_a.lib`), `include/curl/*`, and a
  `.dinero-build-meta` file (`OS=Windows`, `ARCH=AMD64`, `CURL_VERSION=<ver>`,
  `OPENSSL_VERSION=3.5.7`, `BUILT_AT_UTC=...`).

### 2. CMake integration — `cmake/VendoredCurl.cmake`

- New cache option `DINERO_VENDORED_CURL` (default ON when MSVC).
- When ON: define an IMPORTED STATIC target `CURL::libcurl` (drop-in for the
  existing consumers, no per-target link edits):
  - `IMPORTED_LOCATION` → vendored `libcurl.lib`.
  - `INTERFACE_INCLUDE_DIRECTORIES` → vendored `include`.
  - `INTERFACE_LINK_LIBRARIES` → vendored static `libssl.lib` then `libcrypto.lib`
    (order matters: curl before ssl before crypto in final link), plus system
    libs `ws2_32 crypt32 wldap32 bcrypt normaliz advapi32`.
  - `INTERFACE_COMPILE_DEFINITIONS CURL_STATICLIB` so consumers stop dllimport-ing.
- `DINERO_VENDORED_CURL_DIR` override path, mirroring
  `DINERO_VENDORED_OPENSSL_DIR`.
- Behavior when the vendored prebuilt is absent: hard error directing the user to
  run `build-curl-vendored.ps1` (match the OpenSSL policy — no silent system
  fallback for a release configuration). A non-MSVC configure never enters this
  path.
- `CMakeLists.txt:1110-1116`: on MSVC, `include(cmake/VendoredCurl.cmake)` and
  set `DINERO_CURL_TARGET CURL::libcurl` from the vendored target instead of
  `find_package(CURL REQUIRED)`. Unix branch (`set(DINERO_CURL_TARGET curl)`)
  unchanged.

### 3. Packaging changes

- `packaging/windows/build-server-installer.ps1` (~L247): remove `libcurl.dll`,
  `libcrypto-3-x64.dll`, `libssl-3-x64.dll` from `$vcpkgDlls` (keep `z.dll` only
  if zlib stays dynamic — ideally also removed once `CURL_ZLIB=OFF`).
- User-lane `packaging/windows/build-installer.ps1` (+ any windeployqt step):
  ensure the same DLLs are not bundled. With a static `CURL::libcurl` target,
  `dinero-qt` no longer has a `libcurl.dll` runtime dependency, so Qt deploy
  should not list it — verify via the generated `QtDeployTargets-*.cmake`.

### 4. Verification

Rebuild both lanes fresh (wipe `build-msvc-server` / `build-msvc-native` to avoid
stale cache pins, as with the OpenSSL 3.5.6→3.5.7 cache trap already hit):

1. `check_openssl_version.ps1 -BuildDir build-msvc-server` → **green** (no
   non-baseline OpenSSL in any bundled DLL/EXE; only static 3.5.7).
2. `dumpbin /dependents` on `dinerod.exe`, `dinero-wallet-cli.exe`, and the miners
   → no `libcurl.dll` / `libcrypto-3-x64.dll` / `libssl-3-x64.dll` imports.
3. `dinero-wallet-cli.exe --version` succeeds; exercise a curl-backed code path
   (e.g. the miner/wallet HTTP call) to confirm static curl+OpenSSL 3.5.7 works
   at runtime, not just links.
4. `native-msvc-regtest-smoke.ps1` daemon smoke still passes.
5. Spot-check the user installer payload for the absence of the three DLLs.

## Risks

- **Static link order:** curl → libssl → libcrypto → system libs. Managed via the
  imported target's `INTERFACE_LINK_LIBRARIES` ordering. The repo already hit
  "multiple curl copies" link-order pain on MinGW, so watch for duplicate curl
  symbols if any consumer still transitively pulls vcpkg curl.
- **curl version/SHA pin:** finalize exact version + published SHA256 at
  implementation; add to the `$KnownCurlSourceSha256` map.
- **Qt deploy re-adding libcurl.dll:** confirm the static target removes the
  runtime DLL entry from `QtDeployTargets-*.cmake`.
- **Build cost:** both lanes rebuild; the user/GUI lane is heavy (Qt, `--parallel 2`).
- **CRT / zlib:** match `/MD`; drop or static-link zlib (`z.dll`) — cosmetic to the
  crypto goal but wanted for a clean single-baseline payload.

## Out of scope / follow-ups

- Linux/Mac curl parity and `check_openssl_version.sh` bundled-lib scan.
- Any curl feature changes beyond what the miners/wallet already use.
