# Native MSVC Port — Bug Log

Investigation log for porting Dinero-Coin from MinGW-w64 cross-compile to
native MSVC on Windows. **This file does not modify the existing MinGW
build path.** Build directory: `build-msvc-native/` (parallel to the
existing `build/`).

Configure command used:

```powershell
cmake -S . -B build-msvc-native `
  -G "Visual Studio 17 2022" `
  -A x64 `
  -DCMAKE_BUILD_TYPE=Release
```

Status: **investigation in progress**. No binaries from this branch ship
until smoke + regtest pass.

---

## Bug #1 — Vendored OpenSSL build script is shell-only

**Status:** RESOLVED (2026-05-11) — `scripts/build-openssl-vendored.ps1`
ships as the Windows companion to the existing `.sh` script. Same flags
(`VC-WIN64A no-shared no-tests no-apps enable-ec enable-ecdh enable-ecdsa`),
same output location (`third_party/openssl-3.3.2/libcrypto.lib` +
`libssl.lib`), same `.dinero-build-meta` cross-check that CMake reads
at configure time. Auto-locates VS Build Tools via `vswhere.exe` so the
script runs from a regular PowerShell — no need to launch a Developer
Command Prompt first. NASM is detected and used if present; falls back
to `no-asm` with a warning when missing (slower SHA/AES, still
consensus-correct).

**Severity:** blocks configure

**File:** `CMakeLists.txt:702-810` (OpenSSL discovery + metadata gate),
`scripts/build-openssl-vendored.sh` (Linux/macOS),
`scripts/build-openssl-vendored.ps1` (Windows MSVC — new).

**Original symptom:** CMake aborted with `Cannot proceed without OpenSSL`.
The build expects pre-built `third_party/openssl-3.3.2/lib*.{a,lib}` files;
the only mechanism to produce them was `scripts/build-openssl-vendored.sh`,
which is bash-only and uses Unix Makefile workflow.

**Root cause:** OpenSSL vendoring path was designed for macOS / Linux /
MinGW (where bash is available). Windows-native MSVC has no equivalent
until now. Building OpenSSL 3.3.2 on MSVC requires Perl (Strawberry
preferred) + optional NASM + `Configure VC-WIN64A` + `nmake`.

**Resolution:** `scripts/build-openssl-vendored.ps1` runs the same
sequence using `perl Configure VC-WIN64A` + `nmake build_libs`, then
writes `.dinero-build-meta` with `OS=Windows ARCH=AMD64`. Existing CMake
discovery at line ~725 already prefers `libcrypto.lib`/`libssl.lib` on
Windows and falls back to `.a` (MinGW pipeline), so no CMake changes
were required beyond surfacing the `.ps1` path in the FATAL_ERROR hint.

**Workaround for development without rebuilding OpenSSL (still works):**
`cmake -DUSE_SYSTEM_OPENSSL=ON …` falls back to `find_package(OpenSSL)`
(picks up vcpkg's OpenSSL if the vcpkg toolchain file is set).
Investigation-only: release artifacts must use the project's own
vendored copy.

---

## Bug #2 — Protobuf required, not found

**Severity:** blocks configure (when gRPC dev mode is on)

**File:** `CMakeLists.txt:1106-1108`

**Symptom:** `Could NOT find Protobuf (missing: Protobuf_LIBRARIES Protobuf_INCLUDE_DIR)`. The first probe is `find_package(Protobuf CONFIG QUIET)`; if that misses, the second probe is `find_package(Protobuf REQUIRED)` which aborts.

**Root cause:** The build is in "DEV MODE: gRPC enabled for faster iteration" by default. gRPC requires Protobuf. We don't have it.

**Workarounds for investigation:**
- A: `vcpkg install protobuf grpc` to satisfy via vcpkg.
- B: Find the flag that disables gRPC and set it (if such a flag exists). Need to grep for `ENABLE_GRPC` / similar.
- C: Use the hermetic build mode (per `scripts/hermetic-build.sh` reference) which may have different defaults.

**Decision:** workaround A — protobuf is a standard vcpkg package, fast install.

**Note:** secp256k1-zkp configured cleanly under MSVC — its CMake build is MSVC-friendly. Was a feared blocker; isn't.

---

## Bug #3 — gRPC discovery via pkg_check_modules fails with vcpkg

**Severity:** blocks configure (when ENABLE_GRPC is on)

**File:** `CMakeLists.txt:1192`

**Symptom:** `No package 'grpc++' found / No package 'grpc' found` from `pkg_check_modules(GRPC REQUIRED grpc++ grpc)`.

**Root cause:** `pkg_check_modules` reads pkg-config `.pc` files. vcpkg installs gRPC with CMake config (`find_package(gRPC CONFIG REQUIRED)`) but no `.pc` files, so the discovery path doesn't see it. On macOS/Linux Homebrew/apt installs `.pc` files, so the same code works there.

**Real fix:** replace the pkg_check_modules block with `find_package(gRPC CONFIG REQUIRED)` and use `gRPC::grpc++` / `gRPC::grpc` imported targets. Cross-platform; works with vcpkg AND Homebrew/apt (both ship CMake configs nowadays).

**Investigation workaround:** `-DENABLE_GRPC=OFF`. The entire gRPC stack is dev-mode-only (`option(ENABLE_GRPC ... ON)` at line 1013, but `set(ENABLE_GRPC OFF CACHE BOOL "" FORCE)` at line 201 in hermetic mode). Bypasses both #2 and #3 cleanly because Protobuf isn't called either when ENABLE_GRPC=OFF (the Protobuf find at line 1108 is also gated).

**Decision:** disable gRPC for the investigation. Real fix is a separate refactor.

---

## Bug #4 — nvcc + MSVC 14.44 + CUDA 12.2 toolchain mismatch

**Status:** RESOLVED (2026-05-12) — Option C executed on consolidate. The
daemon's `src/mining/gpu/cuda_backend.cpp` was rewritten to use the CUDA
Driver API + NVRTC at runtime (same pattern solo-miner's
`miner/src/gpu/cuda_backend.cpp` uses). `enable_language(CUDA)` removed
from top-level `CMakeLists.txt`; the kernel `miner/shaders/sha256d.cu` is
now read at configure time and embedded into `dinero_gpu_mining.lib` as a
C-string literal via the new `src/mining/gpu/sha256d_cuda_src.cpp.in`
template, then NVRTC-compiled against the running device's exact compute
capability on `initDevice()`. Link deps switched from `CUDA::cudart` to
`CUDA::cuda_driver + CUDA::nvrtc`. `CUDA_ARCHITECTURES` property dropped
(per-arch PTX snapshots no longer needed — NVRTC produces optimal PTX for
the running device).

Validated end-to-end on RTX 4060 Laptop GPU + CUDA 12.2 + MSVC 14.44:
- `cmake -S . -B build-msvc-native -DENABLE_GPU_MINING=ON ...` configures
  clean (no "No CUDA toolset found" error).
- `cmake --build ... --target dinero_gpu_mining` builds the library; logs
  show `cuda_backend.cpp` and `sha256d_cuda_src.cpp` compiled, no nvcc.
- `cmake --build ... --target dinero-gpu-miner` builds the binary that
  previously hit Bug #4. Runtime startup against a regtest dinerod:
  ```
  [CUDA] Found device 0: NVIDIA GeForce RTX 4060 Laptop GPU
         -- 24 SMs @ 2370 MHz, 8187 MB, Compute 8.9
  [CUDA] Initialized device 0 ..., compute 8.9
  [CUDA] Kernel compiled (NVRTC PTX for compute_89) and device buffers allocated.
  [GPU] Ready to mine on NVIDIA GeForce RTX 4060 Laptop GPU (CUDA)
  ```

Single source of truth for the kernel: `miner/shaders/sha256d.cu`. Both
the daemon's IComputeBackend (single-winner `mine()` per WorkPackage) and
solo-miner's IGpuBackend (multi-winner `dispatch()`) now NVRTC-compile the
same .cu file at runtime, eliminating kernel drift between the two paths.
The daemon's old nvcc-compiled `src/mining/gpu/kernels/sha256d_cuda.cu` is
no longer in GPU_SOURCES (kept on disk as a reference for now; can be
removed in a follow-up cleanup).

**Severity:** blocks `enable_language(CUDA)` when ENABLE_GPU_MINING=ON

**File:** `CMakeLists.txt:1499`

**Symptom:** `No CUDA toolset found` from `enable_language(CUDA)`. CUDA 12.2's nvcc has STL `static_assert`s that demand CUDA 12.4+ when compiled with MSVC 14.44.x — known issue (cf. `dinerolabs_cuda_port.md` memory note from 2026-05-09).

**Real fix options:**
- A: upgrade CUDA Toolkit to 12.4+ (changes the install footprint).
- B: install older MSVC (14.40.x) toolset alongside 14.44 and use it for nvcc only.
- C: stop using nvcc — switch the kernel build to NVRTC at runtime, like `dinero-miner` and the new `dinero-solo-miner` CUDA backend already do. Eliminates the nvcc dep entirely; one binary works on any compute capability. **← THIS IS WHAT 2026-05-12 SHIPPED.**

**Investigation workaround:** `-DENABLE_GPU_MINING=OFF`. **No longer needed** after the 2026-05-12 fix — `ENABLE_GPU_MINING=ON` now works on Windows MSVC native.

**Decision:** Option C executed. `dinero-gpu-miner` on MSVC native now mirrors the solo-miner pattern, with a shared kernel source of truth.

---

## Progress so far

Configure makes it through:
- ✅ secp256k1-zkp (CMake-based, MSVC-friendly)
- ✅ OpenSSL via vcpkg (with `-DUSE_SYSTEM_OPENSSL=ON`)
- ✅ Threads, hidapi, ZSTD vendored, blake3
- ✅ Bulletproofs FFI (pre-built Rust artifact)
- ✅ OpenCL detection (uses CUDA's `OpenCL.lib`)
- ⚠️ Lightning Network gracefully degraded (libwally not built — separate bug to address)

## Bug #5 — RocksDB ExternalProject install hardcodes Unix `librocksdb.a`

**Severity:** blocked dinerod build (RocksDB lib couldn't be installed)

**File:** `cmake/VendorRocksDB.cmake:95,97,105`

**Symptom:** `Error copying file ".../_deps/rocksdb-build/librocksdb.a" to ".../_deps/rocksdb-install/lib/librocksdb.a": No such file or directory`. RocksDB itself built fine and produced `_deps/rocksdb-build/Release/rocksdb.lib` (MSVC convention: no `lib` prefix, `.lib` suffix, multi-config generator places it in `Release/` subdir). The install step's hard-coded Unix paths can't find it.

**Root cause:** The ExternalProject install commands assume Unix static-library naming (`lib<name>.a`) and single-config generator output paths.

**Fix applied (committed as `fix(build): use platform-correct static-library naming for vendored RocksDB`):**
- Use `CMAKE_STATIC_LIBRARY_PREFIX` and `CMAKE_STATIC_LIBRARY_SUFFIX` for filenames — works on every platform.
- Use `CMAKE_CONFIGURATION_TYPES` to detect multi-config generator and add the `Release/` subdir.
- Result: `librocksdb.a` on MinGW/Linux/Mac, `rocksdb.lib` on MSVC, install rule looks in the right place either way.

**Decision:** real cross-platform fix, not a hack. Doesn't break MinGW path. Patch lands in main if MSVC build ultimately succeeds.

---

## Bug #6 — vendored hidapi `hidapi_hidpi.h` uses `NTSTATUS` without an include

**Severity:** blocks dinerod build (when ENABLE_HARDWARE_WALLETS=ON)

**File:** `third_party/hidapi/windows/hidapi_hidpi.h:68`

**Symptom:** `error C2143: syntax error: missing '{' before '__cdecl'` and follow-on errors. The file uses `NTSTATUS` (Windows NT type) in a typedef without including any header that defines it. Works on MinGW (probably gets the type via different transitive includes) but fails on MSVC.

**Real fix:** patch hidapi to either include the appropriate Windows header inside `hidapi_hidpi.h` or local-typedef `NTSTATUS` to `LONG` (its actual definition under `WIN32_LEAN_AND_MEAN`). Cleanest: typedef-locally with a comment.

**Investigation workaround:** `-DENABLE_HARDWARE_WALLETS=OFF`. Hardware wallet support isn't needed for dinerod-only daemon operation.

**Decision:** disable for the investigation. Real fix is upstreaming a patch to hidapi (or carrying it locally) — separate work item.

---

## Bug #7 — `ml_dsa_65.cpp` uses `std::to_string` without `#include <string>`

**Severity:** blocks dinerod build

**File:** `src/consensus/pq/ml_dsa_65.cpp:47,58,73,80`

**Symptom:** `error C3861: 'to_string': identifier not found`. GCC and Clang transitively include `<string>` from many other headers (specifically `<stdexcept>` on libstdc++); MSVC's `<stdexcept>` does not pull in `<string>`. So `std::to_string` is undefined under MSVC. Knock-on errors: `std::runtime_error: no appropriate default constructor` because the failed `std::to_string(...)` argument turns into `()`.

**Fix applied:** added `#include <string>` to `ml_dsa_65.cpp`. Real cross-platform fix; doesn't break MinGW.

**Decision:** committed as part of the MSVC port investigation — strict improvement, no risk to MinGW.

---

## Bug #8 — `__uint128_t` is GCC/Clang-only; MSVC has no native 128-bit integers

**Severity:** **HARD WALL** — blocks dinerod compile. Affects consensus.

**Files:** 8 files, 72 total occurrences.
- `src/zk/zkvm/secp256k1_fe_gadget.cpp` (37) — ZK secp256k1 finite-field gadget
- `src/consensus/block_filter.cpp` (8)
- `src/daemon/mempool.cpp` (6)
- `src/consensus/block_validation.cpp` (6)
- `src/consensus/reindexer.cpp` (6)
- `src/consensus/chainwork.cpp` (4)
- `src/core/consensus/chainwork.cpp` (4)
- `src/mining/payout_spec.cpp` (1)

**Symptom:** `error C2065: '__uint128_t': undeclared identifier`. `__uint128_t` is a GCC/Clang extension for 128-bit unsigned integers. MSVC does **not** support it.

**Real fix options:**
- A: Use MSVC intrinsics for each operation (`_umul128`, `_addcarry_u64`, `_subborrow_u64`, etc.). Faithful, fast, but requires hand-porting every `__uint128_t` expression with care.
- B: Use a portable 128-bit type — e.g. `absl::uint128`, `boost::multiprecision::uint128_t`, or write a minimal one. Cleaner abstraction, slight perf overhead.
- C: Add a `dinero::uint128` shim that compiles to `__uint128_t` under GCC/Clang and to `_umul128`/struct-based ops under MSVC. Best long-term answer for cross-platform consistency.

**Why this is a hard wall and not just another fix:**

The `__uint128_t` uses are in **consensus-critical** paths (`block_validation`, `chainwork`, `block_filter`, `mempool`, ZK gadget). Any bug in the port is a chain-split-causing defect. This is not a 1-hour fix or even a 1-day fix:
- Each call site needs careful analysis (which math operation? overflow semantics? sign extension expectations?)
- Each port needs tests verifying bit-identical output vs the GCC build
- The ZK gadget alone has 37 uses, many in tight inner loops
- Consensus regressions don't fail loudly; they silently produce different blocks

This is multi-day to multi-week refactoring work that should be done in a focused effort with full review, not improvised in a Windows-port session.

**Decision:** **stop the investigation here.** Document honestly. The MSVC port for dinerod is deferred pending a dedicated `__uint128_t`-portability refactor.

---

# Investigation Summary

**dinero-cli native MSVC: ✅ BUILDS** (`build-msvc-native/Release/dinero-cli.exe`, 194 KB). Configure flags: `-DUSE_SYSTEM_OPENSSL=ON -DENABLE_GRPC=OFF -DENABLE_GPU_MINING=OFF -DENABLE_HARDWARE_WALLETS=OFF -DCMAKE_TOOLCHAIN_FILE=vcpkg.cmake`. Smoke test passes (`--help` works). **Could be shipped if a clean MSVC dinero-cli is wanted.** Note: it links against vcpkg's OpenSSL rather than vendored, so technically not the "release" artifact yet.

**dinerod native MSVC: ❌ BLOCKED** by Bug #8.

Configure went all the way through. 8+ libraries built cleanly:
- jsoncpp, sqlite3, zstd, bulletproofs_ffi (Rust), pqclean_ml_dsa_65
- dinero_rpc_client, dinero_lightning_keys, dinero_tx_primitives
- dinero_pq, dinero_gpu_mining (stub mode), RocksDB
- secp256k1-zkp configured (didn't reach build of dinero_zk)

Build wall: dinero_zk → secp256k1_fe_gadget.cpp → `__uint128_t` undeclared. 72 uses across 8 files in consensus + ZK code.

**Bugs fixed in-tree (real cross-platform fixes, no MinGW regression):**
- Bug #5: `cmake/VendorRocksDB.cmake` — platform-correct static-library naming.
- Bug #7: `src/consensus/pq/ml_dsa_65.cpp` — added `#include <string>`.

**Bugs deferred (workaround flags, not real fixes):**
- Bug #1 (vendored OpenSSL build script shell-only)
- Bug #2/#3 (Protobuf/gRPC discovery via pkg_check_modules)
- Bug #4 (CUDA 12.2 + MSVC 14.44 nvcc mismatch)
- Bug #6 (hidapi NTSTATUS without include)

**Honest release note:**

> Windows daemon and CLI remain MinGW-w64 for v2.2.6-rc1. Native MSVC port investigation in `NATIVE-MSVC-PORT-BUGS.md` documents 8 bugs found and 2 fixed; the dinerod build is blocked on a `__uint128_t` portability refactor (~72 uses across 8 consensus-critical files) which should be done as a dedicated effort with bit-identical-output testing, not as part of a release.

---

# Update 2026-05-09 / 2026-05-10 — Bug #8 fixed, ctest widening

Bug #8 (`__uint128_t`) is **resolved**. The portability shim
(`include/dinero/compat/int128.hpp`) plus the bit-identical-output gate
(`tests/consensus/test_int128_compat.cpp`, run twice — native and
`DINERO_INT128_FORCE_STRUCT=1`) is in place. All 72 call-sites ported
across 8 files. Dinerod.exe builds + mines regtest blocks on native
MSVC. Three additional pre-existing test bugs surfaced under MSVC
were unblocked in commit `1be3c549` (uniform_int_distribution<uint8_t>,
mempool stub TU, signature_rejection setup-bug). Non-aborting
libsecp256k1 illegal_callback added in `7c19d485`.

After widening to a 12-test ctest set, two additional pre-existing
issues found that are NOT MSVC-port bugs:

## Test rot #1 — `test_stateless_verification` Test 1 (`StatelessVerification` ctest)

**Severity:** test rot, not a port bug.

**File:** `tests/consensus/test_stateless_verification.cpp:74-78`

**Symptom:** `[FAIL] Height 1 should be premine`. Test asserts
`GetBlockSubsidy(1) == 2'627'900 * UNA_PER_DIN`.

**Root cause:** the consensus rule was changed to "no premine, 100 DIN
at height 1" (see `src/consensus/genesis_canonical.cpp:15` "No
premine"; `include/consensus/supply_validator.h:65` asserts
`GetBlockSubsidy(1) == INITIAL_SUBSIDY`). The block-subsidy comment in
`src/consensus/block_validation.cpp:2520` likewise says "Height 1: 100
DIN (first PoW block)". The test expectation is from the removed
premine schedule and would fail on Linux too if the test were run
post-rule-change. The pass on Linux is silent (presumably this ctest
isn't in the default Linux suite) and the failure on MSVC is just
because we widened the gate.

**Fix:** update the test to match the canonical schedule —
`GetBlockSubsidy(1) == 100 * UNA_PER_DIN`, `GetBlockSubsidy(2) == 100
* UNA_PER_DIN`, halvings at 1314002, 2628002 unchanged. Out of scope
for the MSVC-port effort; filed as separate test-quality task.

## Test rot #2 candidate — `test_block_download_scheduler` (`BlockDownloadScheduler` ctest)

**Severity:** unknown (pure scheduling logic, unlikely to be
arithmetic / int128-port related).

**File:** `tests/consensus/test_block_download_scheduler.cpp:355-363`

**Symptom:** `stale height-index must NOT suppress fork block X at
height 3 (got missing=1, expected >=2)`. Test simulates a stale
height-index callback that returns the wrong hash for height 3 and
asserts the scheduler queues two missing blocks anyway. Got 1.

**Root cause:** unverified. The scheduler-fork-detection logic this
test guards against is not arithmetic-portable and the int128 shim
cannot affect it. Most likely candidates are (a) the fix the test
guards never landed, or (b) `unordered_map` iteration-order
sensitivity inside the scheduler. Deferred — needs Linux comparison
to confirm whether this is MSVC-specific or pre-existing.

## Build-only failure — `test_consensus_core_standalone`

**Severity:** unresolved-external link error in the test target.

**File:** `tests/consensus/test_consensus_core_standalone.vcxproj`

**Symptom:**
```
dinero-consensus.lib(utreexo_accumulator.obj) : error LNK2019:
  unresolved external symbol "enum dinero::Chain __cdecl
  dinero::GetActiveChain(void)"
  referenced in function "bool __cdecl
  dinero::consensus::IsUtreexoCanonicalRootsActive(unsigned int)"
dinero-consensus.lib(consensus_utxo_set.obj) : error LNK2001
```

**Root cause:** same shape as the daemon-Mempool stub problem fixed
in `1be3c549` — the .obj files inside `dinero-consensus.lib`
reference `dinero::GetActiveChain()` whose definition lives in a
runtime TU that this standalone test target doesn't link. Needs a
test-side stub TU (parallel to `tests/stubs/mempool_test_stubs.cpp`)
or a build-system fix to extract the chain-state symbol from the
runtime TU. Deferred — straightforward fix when this test is
prioritized.

## Current MSVC ctest gate

Of 12 tests now wired in:
- ✅ 9 pass: DAA golden vectors, genesis, mainnet checkpoints,
  subsidy, hash domains, block height, amount, both int128
  portability gates, signature_rejection, compact-block
  serialization, headers wire v1, CsnProofRefresh, ABI_Stability,
  ConsensusInvariants.
- ❌ 2 fail: `StatelessVerification` (test rot — premine removed),
  `BlockDownloadScheduler` (pre-existing? unverified).
- ⚠️ 1 build-only failure: `test_consensus_core_standalone` (link
  error needs stub TU).

Several other ctest registrations (`FormalInvariants`,
`BlockDownloadSchedulerRestartBootstrap`, `BlockDownloadSchedulerG6BVerification`,
`Mining_WitnessExtranonce_Invariants`) point at exes that need
separate `cmake --build` targets and were not yet wired into the
batch builds — addressed in the next iteration.

## Update 2026-05-10 — second batch (4 more exes built)

Targets `test_formal_invariants`, `test_block_download_scheduler_restart_bootstrap`,
`test_block_download_scheduler_g6b`, `test_witness_extranonce_invariants`,
`test_descriptor_psbt_simple` all built clean under MSVC Release.

ctest results:

- ✅ `BlockDownloadSchedulerG6BVerification` (#194) — PASS (5.08s)
- ✅ `Mining_WitnessExtranonce_Invariants` (#327) — PASS
- ❌ `FormalInvariants` (#343) — `0xC0000409` mid-I4 ("Snapshot/Restore Identity"). I1, I2, I3 all passed; crash hits inside `ConsensusUTXOSet::BulkLoad` → utreexo forest population, before the invariant check runs.
- ❌ `BlockDownloadSchedulerRestartBootstrap` (#153) — `0xC0000409` immediately on launch (no test output).

`0xC0000409` on MSVC is `STATUS_STACK_BUFFER_OVERRUN` / `__fastfail()`. Three known sources in this codebase:
1. `/GS` stack-canary failure → real stack corruption.
2. libsecp256k1's default illegal_callback `abort()` → patched in `7c19d485` for the three consensus-side context creations, but a context anywhere else not yet covered will still abort.
3. CRT `abort()` from anywhere else (e.g. an `_ASSERTE` in a Release build, though Release strips most).

For these two failures, libsecp256k1 is unlikely to be involved (the I4 test exercises utreexo forest BulkLoad, no signatures); more likely candidates are stack-buffer overrun or a `__fastfail` from the utreexo C++ code under MSVC. **Filed for follow-up triage** — needs WinDbg attached to capture the actual fail-fast code, or a `/GS-` rebuild of those two TUs to localize.

## Build-only failure update — `test_consensus_core_standalone`

Same architectural shape as the Mempool stub fixed in `1be3c549`. The .obj files in `dinero-consensus.lib` reference `dinero::GetActiveChain()` whose definition lives in a runtime TU that this standalone test target doesn't link. Fix: add a test-only stub TU (parallel to `tests/stubs/mempool_test_stubs.cpp`) returning a sensible default `dinero::Chain::Mainnet`. Out of scope for this notes update; tracked separately.

## Current MSVC ctest gate (after second batch)

15 tests now wired in:

- ✅ 11 pass: DAA golden vectors, genesis, mainnet checkpoints, subsidy, hash domains, block height, amount, both int128 portability gates, signature_rejection, compact-block serialization, headers wire v1, CsnProofRefresh, ABI_Stability, ConsensusInvariants, BlockDownloadSchedulerG6BVerification, Mining_WitnessExtranonce_Invariants.
- ❌ 4 fail: `StatelessVerification` (test rot — premine removed), `BlockDownloadScheduler` (pre-existing? unverified), `FormalInvariants` (new — 0xC0000409 mid-I4), `BlockDownloadSchedulerRestartBootstrap` (new — 0xC0000409 on launch).
- ⚠️ 1 build-only failure: `test_consensus_core_standalone` (link error needs stub TU for GetActiveChain).
