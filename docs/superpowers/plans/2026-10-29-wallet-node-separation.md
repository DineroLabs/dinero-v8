# Storage-free Consensus Kernel and Wallet Separation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make `dinero_consensus` storage-free and independently linkable, and give `dinero_wallet` a transitive link with no storage, node, P2P or RPC code. Prove both with gates that run in CI, then remove the ubuntu-24.04 pin.

**Architecture:** First add mechanical gates and record what each shows on the baseline (stage 0). Then move impure sources to the libraries that own their dependencies (stages 1–4). Then give the wallet two small interfaces, `IWalletChainView` and `IWalletMempoolView`, with header-only node-side adapters (stages 5–6). Finally make the gates required and remove the pin (stages 7–8). Each stage is one PR onto `dinero-main`.

**Tech Stack:** C++20, CMake ≥ 3.20 (the CMake File API, codemodel v2, is used by the gate scripts), Ninja, Python 3 (gate scripts), GNU ld / ld64 / MSVC link, SQLite, GitHub Actions.

**Spec:** `docs/superpowers/specs/2026-10-29-wallet-node-separation-design.md` (rev 4.1, commit 0c39ec9b4). The spec is the authority. Section numbers below (§6.1 etc.) refer to it.

## Global Constraints

- **Baseline:** `dinero-main` 1667f8ed6. Task 0.1 refreshes it to the then-current `dinero-main`, and every later reference uses the refreshed SHA.
- **Separate from v8.2.0 and PR #855.** This approval covers planning only. It does not select v8.2.1, and it does not approve the frozen-header change. **Stop after Task 0.10 for the owner's v8.2.1 decision, and stop before Task 2.6 for "CONSENSUS CHANGE" approval.**
- **Wallet files must open unchanged, with no migration.** Do not change any schema, path, PRAGMA, migration or trigger. `open_sqlite`'s PRAGMAs move byte-for-byte (WAL, `synchronous=NORMAL`, `foreign_keys=ON`, busy 5000, mmap 64 MB). Raw `sqlite3_open` sites are not touched.
- **External interfaces stay identical:** RPC methods, fields and errors; `include/nodecore/nodecore_ffi.h`; `wallet-core/ffi/wallet_ffi.h`; the JNI exports; Qt and NodeCore behavior.
- **No consensus logic change.** The only frozen-header edit allowed is §6.5. Do not edit `methods_wallet_context.cpp` lines matched by `qt/tests/test_shielded_rpc_status.cmake:7-39`.
- **Every gate runs twice:** with `-DDINERO_BUILD_ORCHARD_BACKEND=OFF` and with `=ON`.
- **No gate runs a test on the release evidence's never-run floor.** `OrchardStateRoot` and `OrchardStateCommitment` are on it.
- **Windows CI must never sign or upload.** Do not use `windows-release.yml`; model new jobs on `windows-openssl-release-smoke.yml`.
- **Verification discipline:**
  - observe every gate failing before the change that turns it green;
  - grep logs to prove tests ran (`ctest --no-tests=error`);
  - use no `assert()`-only checks;
  - `touch` edited sources and confirm the `.o` rebuilt.
- **Pushing needs owner approval.** Commit as `git -c user.name=Claude -c user.email=team@dinerolabs.org commit`, with the trailer `Co-Authored-By: Claude Opus 5 (1M context) <team@dinerolabs.org>`.
- **Surgical moves:** a file that moves keeps its contents byte-for-byte unless a step names the change. Verify with `git diff -M100% --stat`.
- **Stop and report** (don't work around it) if any of these happens:
  - a consensus source beyond §6.1 needs storage or node code;
  - a frozen header needs a change beyond §6.5;
  - K4 digests differ;
  - an external change beyond E1–E3 appears;
  - an ON-build Orchard consumer needs wallet internals beyond the §7.2 friend list.

## Review Focus

These five failure modes are the most likely to bite a user and are not otherwise covered. Each is pinned by a test in the task named.

1. **A rescan from `wallet_worker` silently starts reading flat files.** A view built without BlockStorage must read ChainDB only. Pinned by `WalletChainViewNoBlockStorageReadsChainDbOnly` (Task 5.2).
2. **An Orchard mempool entry reaches RBF/CPFP detection.** It must throw the same `std::logic_error("Orchard transaction has no historical body")` at the same point as today. Pinned by `WalletMempoolEntryOrchardVinThrowsSameError` (Task 5.3).
3. **`listunspent` with no mempool view must not filter, exactly as today** (NodeCore and FFI paths). Pinned by `ListUnspentNullMempoolViewNoFilter` (Task 5.3).
4. **`g_logger` used during static initialization** after it moves into its own archive. If any static initializer logs, order across archives could change. Pinned by `LoggerStaticInitOrder` (Task 1.1), which runs only if the audit in that task finds a static-init use.
5. **A prepared-history rescan from ChainstateService with a null view.** It must still run, and a null view with no prepared history must still return false. Pinned by `RescanNullViewPreparedHistoryRuns` (Task 5.2).

---

## File structure

| Path | Responsibility |
|---|---|
| `scripts/ci/separation/check_link_closure.py` | **K2/W2.** Walks one target's transitive closure using CMake File API codemodel v2, including PRIVATE links of static libraries. Checks forbidden targets, include directories, cycles and the source-path allowlist |
| `scripts/ci/separation/closure_rules.json` | Per-gate rules: forbidden targets and include-dir patterns, allowed third-party targets, and the source allowlist with reasons |
| `scripts/ci/separation/check_symbols.py` | **K3/W3.** Lists defined and undefined symbols in a linked binary (`nm -C`; `dumpbin /SYMBOLS` on MSVC) and fails on forbidden patterns |
| `scripts/ci/separation/consensus_freeze.py` | **K5.** `record` / `check` SHA-256 of `src/consensus/**` and `include/consensus/**` against an allowed-change list |
| `scripts/ci/separation/consensus_baseline.sha256` | K5 baseline, recorded at the refreshed baseline SHA |
| `scripts/ci/separation/consensus_allowed_changes.txt` | Files K5 lets change, with the stage and spec section for each |
| `scripts/ci/separation/run_consensus_vectors.py` | **K4.** Runs the named tests with `--no-tests=error`, checks each one ran, and extracts and compares 64-hex digests |
| `scripts/ci/separation/consensus_vectors.txt` | K4 test list (spec §8), minus anything on `never_run_floor.txt` |
| `scripts/ci/separation/never_run_floor.txt` | Copy of the release evidence's never-run floor, with its source path and SHA recorded in the header |
| `scripts/ci/separation/design_anchors.tsv` | `path<TAB>line<TAB>expected substring` for each spec citation that a task depends on |
| `scripts/ci/separation/check_design_anchors.py` | Verifies the anchors and prints a moved/missing report |
| `scripts/ci/separation/tests/` | Unit tests for the scripts, with fixture codemodel JSON and fixture `nm` output |
| `tests/separation/CMakeLists.txt` | Gate targets, built only with `-DDINERO_SEPARATION_GATES=ON` (default OFF) |
| `tests/separation/consensus_forced_link.cpp` | **K1.** `main`: `Params()`, one script vector, one sighash vector |
| `tests/separation/wallet_forced_link.cpp` | **W1.** `main`: construct `WalletManager` on a temp dir and open it |
| `tests/separation/make_wallet_fixture.cpp` | **W4 generator.** Built only at the baseline, never against split code |
| `tests/separation/wallet_fixture_compat.cpp` + `fixtures/{off,on}/` | **W4 checker** and its checked-in fixtures |
| `tests/separation/rpc_transcript.py` + `transcripts/{off,on}.json` | **W5 regtest transcript** and its normalizer |
| `tests/separation/interface_freeze.py` + `interface_baseline/` | **W5.** Header diff, JNI export list and exported dynamic symbols |
| `tests/separation/tx_validation_binds.cpp` | **K6.** Compile-time binding test |
| `.github/workflows/separation-gates.yml` | Non-required gate jobs: Linux GCC 15 and 13, macOS arm64, each × OFF/ON |
| `.github/workflows/separation-gates-windows.yml` | CI-only MSVC gate job: no signing, no uploads |
| `include/wallet/node_interfaces.h` | `IWalletChainView`, `IWalletMempoolView`, `WalletChainTip`, `WalletMempoolEntry`, `WalletMempoolStats` |
| `include/storage/chain_db_wallet_view.h` | Header-only `ChainDbWalletView` |
| `include/daemon/mempool_wallet_view.h` | Header-only `MempoolWalletView` |
| `include/wallet/snapshot_wallet_types.h` | `SnapshotWalletSourceData` and `SnapshotWalletCoin`, moved unchanged |
| `include/common/sync_phase.h` | `SyncPhase`, moved unchanged |
| `src/wallet/p2mr_signing.cpp` + `include/wallet/p2mr_signing.h` | `SignP2MRParams` and `SignP2MR`, moved verbatim |
| `tests/wallet/fake_node_views.h` | `FakeChainView` and `FakeMempoolView` for wallet tests |

Library edits: `src/consensus/CMakeLists.txt`, `src/consensus/pq/CMakeLists.txt`, `src/storage/CMakeLists.txt`, `src/wallet/CMakeLists.txt`, root `CMakeLists.txt` (logging, sqlite, crypto, core lists), `tests/CMakeLists.txt`, `rust/orchard_backend/CMakeLists.txt`, `mobile-tauri/src-tauri/build.rs` (only if E1 is approved separately).

**Build directories** used by every command below:
- `build-sep-off`: `-DDINERO_BUILD_ORCHARD_BACKEND=OFF`.
- `build-sep-on`: `=ON`.
- Both are configured with `-G Ninja -DCMAKE_BUILD_TYPE=Release -DDINERO_USE_VENDORED_DEPS=ON -DENABLE_GRPC=OFF -DBUILD_NODECORE=ON -DDINERO_SEPARATION_GATES=ON`, and each has a CMake File API query file, `.cmake/api/v1/query/codemodel-v2`, created before configuring.

`$CFG` below means "run in both directories".

---

## Stage 0 — refresh, gates, measurement (one PR, gates non-required)

### Task 0.1: Baseline refresh and design anchors

**Files:**
- Create: `scripts/ci/separation/design_anchors.tsv`, `scripts/ci/separation/check_design_anchors.py`, `scripts/ci/separation/tests/test_check_design_anchors.py`

**Interfaces:**
- Produces: `check_design_anchors.py <repo> <tsv>`. It exits 0 when every anchor matches, 1 otherwise, and prints `MOVED path old->new` or `MISSING path:line` for each anchor that doesn't match.

- [ ] **Step 1: Write a failing test.** `test_reports_moved_and_missing`: a temp repo with `a.cpp` holding `foo` at line 3, and anchors for `a.cpp 2 foo` and `a.cpp 9 bar`. Expect exit 1 and output containing exactly `MOVED a.cpp 2->3` and `MISSING a.cpp:9`.
- [ ] **Step 2:** Run `python3 -m pytest scripts/ci/separation/tests/test_check_design_anchors.py -v`. Expected: FAIL (script missing).
- [ ] **Step 3:** Implement it. A moved anchor is found by searching ±200 lines for the substring, nearest first.
- [ ] **Step 4:** Run the test again. Expected: PASS.
- [ ] **Step 5:** Fill in `design_anchors.tsv`. Use one row per file:line the plan's tasks act on, taken from the spec §6.1–§7.2 tables (for example `src/consensus/tx_validation.cpp	2	coins_db.h`, `src/wallet/wallet_manager.cpp	7507	if (!chain_db && !prepared)`).
- [ ] **Step 6:** Rebase this branch onto current `origin/dinero-main`, then run `python3 scripts/ci/separation/check_design_anchors.py . scripts/ci/separation/design_anchors.tsv`.
  - Expected: exit 0, or a MOVED/MISSING list.
  - Update the TSV for MOVED rows.
  - **Stop and report** any MISSING row: it means the spec's facts changed.
- [ ] **Step 7:** Commit `ci(separation): design anchors and checker`. Record the refreshed baseline SHA in the commit body.

### Task 0.2: Closure checker (K2, W2)

**Files:**
- Create: `scripts/ci/separation/check_link_closure.py`, `scripts/ci/separation/closure_rules.json`, `scripts/ci/separation/tests/test_check_link_closure.py`, `scripts/ci/separation/tests/fixtures/codemodel_*`

**Interfaces:**
- Produces: `check_link_closure.py --build <dir> --target <name> --rules closure_rules.json --gate {K2,W2}`. It exits 0 when clean and 1 on any violation, and prints one line per violation: `FORBIDDEN_TARGET`, `FORBIDDEN_INCLUDE`, `CYCLE a->b->a`, `NODE_SOURCE <path>`.
- Rules: K2 forbids `RocksDB::rocksdb|rocksdb`, `sqlite3|SQLite::SQLite3`, `dinero_chainstate`, `dinero_core`, `dinero_wallet`, `dinero_rpc_handlers`, any target whose sources include `src/daemon/mempool.cpp`, and include dirs matching `rocksdb` or `/src/daemon$`. W2 uses the same rules minus `sqlite3` and `dinero_wallet`, plus `dinero_core`, `dinero_rpc_handlers` and any target with `src/p2p/` sources. W2 allows `lightning_core_static`. The source allowlist is `src/daemon/bech32_decode.cpp`, `src/daemon/bech32_encoder.cpp` and `include/daemon/bech32_decode.h`, with the reason "pure codec (spec §6.3)". The cycle check covers only the walked closure.

- [ ] **Step 1: Write failing tests** against fixture codemodels:
  - `test_private_static_link_propagates`: A static PRIVATE→sqlite3 is reported for a root that links A.
  - `test_cycle_in_closure_reported`: A→B→A gives `CYCLE`.
  - `test_cycle_outside_closure_ignored`.
  - `test_allowlisted_node_source_passes`.
  - `test_unlisted_src_rpc_source_fails`.
- [ ] **Step 2:** Run `python3 -m pytest scripts/ci/separation/tests/test_check_link_closure.py -v`. Expected: 5 FAIL.
- [ ] **Step 3:** Implement it by reading `reply/index-*.json` → codemodel → target JSONs (`dependencies`, `link.commandFragments`, `compileGroups[].includes`, `sources`). Static-library PRIVATE deps are followed. External link fragments (for example `librocksdb.a`) are matched by basename.
- [ ] **Step 4:** Run the tests again. Expected: 5 PASS.
- [ ] **Step 5:** On a real `$CFG` build, run `--target dinero_consensus --gate K2` and `--target dinero_wallet --gate W2`. Expected **RED**, with violations including:
  - K2: RocksDB, sqlite3 (also via `dinero_pq`), `src/daemon` include, `NODE_SOURCE src/rpc/v7_pq_handlers.cpp`, the pq wallet sources, and a consensus↔pq cycle.
  - W2: RocksDB via consensus, and `src/rpc/v7_pq_handlers.cpp`.

  Save both outputs to `~/src/dinero-evidence/<date>-separation/stage0/`.
- [ ] **Step 6:** Commit `ci(separation): transitive closure checker (K2/W2)`.

### Task 0.3: Forced-link probes and symbol checker (K1, K3, W1, W3)

**Files:**
- Create: `tests/separation/CMakeLists.txt`, `tests/separation/consensus_forced_link.cpp`, `tests/separation/wallet_forced_link.cpp`, `scripts/ci/separation/check_symbols.py`, `scripts/ci/separation/tests/test_check_symbols.py`
- Modify: root `CMakeLists.txt`: add `option(DINERO_SEPARATION_GATES "Build wallet/consensus separation gate probes" OFF)` and `if(DINERO_SEPARATION_GATES) add_subdirectory(tests/separation) endif()`.

**Interfaces:**
- Produces: executable targets `sep_consensus_forced_link` and `sep_wallet_forced_link`, and ctest names `SepConsensusForcedLink` and `SepWalletForcedLink` (label `separation`).
- Each executable links only the root library, whole-archive, plus that library's declared link dependencies:
  - GNU: `-Wl,--whole-archive $<TARGET_FILE:lib> -Wl,--no-whole-archive -Wl,--no-undefined`;
  - Apple: `-Wl,-force_load,$<TARGET_FILE:lib>`;
  - MSVC: `/WHOLEARCHIVE:$<TARGET_FILE_NAME:lib>`.
- Produces: `check_symbols.py --binary <path> --gate {K3,W3}`. It exits 1 on any forbidden symbol and prints `FORBIDDEN <pattern> <symbol>`.
  - K3 patterns: `rocksdb::`, `sqlite3_`, `dinero::ChainDB`, `ChainstateService`, `dinero::Address::`, `dinero::Mempool`.
  - W3 patterns: the same minus `sqlite3_` and `Address::`, plus `dinero::BlockStorage`, `HeaderSyncManager`, `PeerManager`, `TxInclusionAnalyzer`.

- [ ] **Step 1: Write a failing test.** `test_check_symbols_flags_rocksdb`: fixture `nm -C` text containing `T rocksdb::DB::Open` gives exit 1 and one FORBIDDEN line. `test_check_symbols_clean` gives exit 0.
- [ ] **Step 2:** Run `python3 -m pytest scripts/ci/separation/tests/test_check_symbols.py -v`. Expected: FAIL.
- [ ] **Step 3:** Implement `check_symbols.py`. Write `consensus_forced_link.cpp`: `main` calls `dinero::Params()`, runs the first case of `tests/data/script_tests.json` through the script interpreter and one BIP341 sighash vector, and returns nonzero on mismatch (no `assert`). Write `wallet_forced_link.cpp`: `main` creates a temp dir, constructs `dinero::WalletManager(dir)`, opens it, and returns nonzero on failure.
- [ ] **Step 4:** Run the pytest. Expected: PASS. Then run `cmake --build $CFG --target sep_consensus_forced_link sep_wallet_forced_link -k 0`. Expected **RED** (link errors):
  - consensus: 16 undefined node symbols (ChainstateService, ChainDB, `Address::*`, Logger);
  - wallet: at least the 10 `wallet_manager.o` references plus the D1–D3 symbols.

  Save the logs as evidence.
- [ ] **Step 5:** Commit `ci(separation): forced-link probes and symbol checker (K1/K3/W1/W3)`.

### Task 0.4: Consensus vectors (K4) and the never-run floor

**Files:**
- Create: `scripts/ci/separation/run_consensus_vectors.py`, `scripts/ci/separation/consensus_vectors.txt`, `scripts/ci/separation/never_run_floor.txt`, `scripts/ci/separation/tests/test_run_consensus_vectors.py`

**Interfaces:**
- Produces: `run_consensus_vectors.py --build <dir> --list consensus_vectors.txt --floor never_run_floor.txt --record <out.json>` or `--compare <ref.json>`. It exits 1 if:
  - any listed test is also on the floor (it refuses to run);
  - any test did not run;
  - any test failed;
  - any test's set of 64-hex digests in its output differs from the reference.

- [ ] **Step 1: Obtain the never-run floor.** Ask the owner for the release-evidence path, then copy the list into `never_run_floor.txt`, with that path and its SHA-256 in the header comment. **Do not proceed without it.**
- [ ] **Step 2:** Write `consensus_vectors.txt` from spec §8 (K4 list). Exclude anything on the floor, so it contains neither `OrchardStateRoot` nor `OrchardStateCommitment`. Mark ON-only tests with the prefix `on:`.
- [ ] **Step 3: Write failing tests:**
  - `test_refuses_floor_test`: the list contains a floor entry → exit 1 and `ON_FLOOR`.
  - `test_missing_test_fails`: ctest JSON lacks a listed name → exit 1 and `NOT_RUN`.
  - `test_digest_mismatch_fails`.
- [ ] **Step 4:** Run `python3 -m pytest scripts/ci/separation/tests/test_run_consensus_vectors.py -v`. Expected: FAIL.
- [ ] **Step 5:** Implement it. Use `ctest --test-dir <dir> -R '^(...)$' --no-tests=error --output-junit`, then parse the per-test output.
- [ ] **Step 6:** Run the tests again. Expected: PASS. Run `--record` on `$CFG` at the baseline. Expected: all listed tests ran and passed. Save the reference JSON as evidence and commit it as `scripts/ci/separation/consensus_vectors_ref_{off,on}.json`.
- [ ] **Step 7:** Commit `ci(separation): consensus vector gate (K4) with never-run floor`.

### Task 0.5: Consensus source freeze (K5)

**Files:**
- Create: `scripts/ci/separation/consensus_freeze.py`, `scripts/ci/separation/consensus_baseline.sha256`, `scripts/ci/separation/consensus_allowed_changes.txt`, `scripts/ci/separation/tests/test_consensus_freeze.py`

**Interfaces:**
- Produces: `consensus_freeze.py record|check --repo . --baseline consensus_baseline.sha256 --allowed consensus_allowed_changes.txt --stage N`.
- `check` fails on:
  - any changed, added or removed file under `src/consensus/**` or `include/consensus/**` that isn't allowed at or before stage N;
  - for `tx_validation.h` and `tx_validation.cpp`, any hunk beyond the §6.5 lines.
- Allowed list:

| Stage | Files |
|---|---|
| 1 | `src/consensus/CMakeLists.txt` |
| 2 | `src/consensus/CMakeLists.txt`, `src/consensus/pq/CMakeLists.txt`, removal of the moved files from `src/consensus/` (`coins_db.cpp`, `coins_view_cache.cpp`, `global_utxo_set_impl.cpp`), `include/consensus/tx_validation.h` (§6.5 lines), `src/consensus/tx_validation.cpp` (include line), `include/consensus/freeze.h:136` (manifest entry only) |
| 3 | `src/consensus/CMakeLists.txt` |

- [ ] **Step 1: Write failing tests:**
  - `test_unlisted_change_fails`;
  - `test_tx_validation_extra_hunk_fails`: a body change in `tx_validation.cpp` fails even at stage 2;
  - `test_allowed_stage_passes`.
- [ ] **Step 2:** Run the pytest. Expected: FAIL.
- [ ] **Step 3:** Implement it. Record the baseline at the refreshed SHA.
- [ ] **Step 4:** Run the pytest. Expected: PASS. Run `check --stage 0` at the baseline. Expected: PASS.
- [ ] **Step 5:** Commit `ci(separation): consensus source freeze (K5)`.

### Task 0.6: Wallet-file fixtures (W4)

**Files:**
- Create: `tests/separation/make_wallet_fixture.cpp`, `tests/separation/wallet_fixture_compat.cpp`, `tests/separation/fixtures/off/`, `tests/separation/fixtures/on/`

**Interfaces:**
- Produces: `sep_make_wallet_fixture <outdir>`, built only at the baseline. It writes:
  - `wallet_registry.db`;
  - `wallets/wallet_sepfixture.db`: an HD wallet created from the fixed regtest mnemonic `abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon about`, with 3 receive addresses, 1 watch script and a descriptor store entry;
  - `blockchain/utxo` (UTXOIndex at `user_version=2`, holding 2 UTXOs for those addresses);
  - `expected.json` (addresses, balance, UTXOs, `user_version` per DB);
  - `schema.txt` (each DB's `SELECT type,name,tbl_name,sql FROM sqlite_master ORDER BY 1,2`).
- Produces: ctest `SepWalletFixtureCompat`. It copies the fixture to a temp dir, opens it read-write with the current build, re-derives `expected.json` fields, and compares them and `schema.txt` before and after the open. It exits 1 on any difference.

- [ ] **Step 1:** At the baseline, build `sep_make_wallet_fixture` in `build-sep-off` and `build-sep-on`. Generate `fixtures/off` and `fixtures/on`. ON includes the Orchard tables and triggers.
- [ ] **Step 2:** Write `wallet_fixture_compat.cpp`. Run `ctest --test-dir $CFG -R '^SepWalletFixtureCompat$' --no-tests=error`. Expected: PASS at the baseline.
- [ ] **Step 3: Neuter check.** In a scratch copy, change `open_sqlite`'s `mmap_size` PRAGMA, rebuild, and rerun. Expected: FAIL (schema or PRAGMA dump differs). Revert the change.
- [ ] **Step 4:** Commit `test(separation): wallet-file compatibility fixtures (W4)`.

### Task 0.7: External interface freeze and RPC transcript (W5)

**Files:**
- Create: `tests/separation/interface_freeze.py`, `tests/separation/interface_baseline/`, `tests/separation/rpc_transcript.py`, `tests/separation/transcripts/{off,on}.json`

**Interfaces:**
- Produces: `interface_freeze.py record|check --build <dir>`.
  - It diffs `nodecore_ffi.h` and `wallet_ffi.h` against the baseline copies.
  - It compares the JNI export list (`Java_org_dinerolabs_*` in `nodecore_jni_android.cpp`).
  - It compares exported dynamic symbols of `nodecore_ffi` and `dinero_wallet_ffi` (`nm -D --defined-only`; `-gU` on Apple).
- Produces: `rpc_transcript.py record|check --dinerod <path> --out <json>`. It starts regtest in a temp datadir, mines, and runs the spec's W5 list in order:
  - getbalance, getnewaddress, listunspent (including an output spent by a mempool tx), sendtoaddress;
  - rescan, and a snapshot-recovery rescan where the fixture supports it;
  - fee bump/RBF, wallet sync status;
  - three error cases: invalid address, insufficient funds, unknown txid;
  - **ON builds only:** an Orchard mempool entry seen by RBF detection.

  Normalization: a 64-hex value becomes `<H#n>`, a `din1…`/`bcrt1…` address becomes `<A#n>`, and times become `<T>`, numbered by first appearance. Field names, types, error codes and messages are compared exactly.

- [ ] **Step 1:** Run `record` at the baseline for both configs.
- [ ] **Step 2:** Run `check` at the baseline. Expected: PASS.
- [ ] **Step 3: Neuter check.** Rename one JSON field in a scratch copy of a listunspent handler. Expected: FAIL. Revert.
- [ ] **Step 4:** Commit `test(separation): external interface freeze and RPC transcript (W5)`.

### Task 0.8: Standalone consumer baseline (W6)

**Files:**
- Modify: `tests/separation/CMakeLists.txt`. Add target `sep_wallet_ffi_standalone`, which links `dinero_wallet_ffi` whole-archive plus the Rust bridge's static set from `mobile-tauri/src-tauri/build.rs:29-38` (`dinero_wallet_ffi`, `dinero_wallet`, `dinero_crypto`, `dinero_consensus`) and nothing else. Add ctest `SepWalletFfiStandalone`.

- [ ] **Step 1:** Build it in `$CFG` at the baseline and record the result.
  - **If GREEN:** label the ctest `separation;required` and note "W6 required (baseline green)" in the evidence README.
  - **If RED:** label it `separation;recorded` and save the undefined-symbol list. The repair is E1, handled separately.
- [ ] **Step 2:** Commit `test(separation): standalone FFI consumer baseline (W6)`.

### Task 0.9: Non-required CI workflows

**Files:**
- Create: `.github/workflows/separation-gates.yml`, `.github/workflows/separation-gates-windows.yml`

**Interfaces:**
- Produces two workflows. Neither is a required check.
  - `separation-gates.yml` runs a matrix of {`ubuntu-latest` (GCC 15), `ubuntu-24.04` (GCC 13), `macos-14` (arm64)} × {OFF, ON}, each with steps for K1–K6 and W1–W6.
  - `separation-gates-windows.yml` follows `windows-openssl-release-smoke.yml`'s toolchain steps. It has no signing steps, no `secrets.*`, no artifact uploads and no release creation.
- In stage 0 every gate step is `continue-on-error: true`. Each step echoes `GATE <id> <PASS|FAIL>`, and a final summary step prints the table.

- [ ] **Step 1:** Write the workflows. Grep each file for `sign`, `secrets.`, `upload` and `release`. Expected: no matches apart from comments.
- [ ] **Step 2:** **Ask the owner before pushing.** After approval, push the stage 0 branch and record the run URL. Expected outcomes:
  - K1, K2, W1 and W2 FAIL on every platform and configuration.
  - K3 and W3 FAIL where they are measured.
    - On Windows they report **UNMEASURED** (neither PASS nor a measured FAIL) until the COFF archive scan is verified: the probe `.lib` link inputs, a verified `llvm-nm` and a positive control.
  - K5 and W4 PASS (K5 runs on POSIX runners only).
  - K4 and W5 PASS on macOS, where the reviewed reference baselines are committed.
    - **On Linux and Windows, K4 and W5 are expected red** (`FAIL rc=2 (missing reference -- not a vector/interface mismatch)`) until reviewed reference baselines for that platform are committed.
    - A missing reference is missing evidence, not a pass and not a mismatch.
  - W6 as recorded in Task 0.8.
  - **The first PR run only collects candidate baselines,** which the record step prints to the job log. It is not described as green.
    - The candidates are reviewed, then committed by hand in a separate change.
    - Only a later run against the committed baselines can show K4/W5 PASS on Linux and Windows.
- [ ] **Step 3:** Commit `ci(separation): non-required gate workflows`.

### Task 0.10: Scope measurement → owner decision (STOP)

**Files:**
- Create: throwaway branch `measure/separation-leg1a`, never merged; report `~/src/dinero-evidence/<date>-separation/stage0/measurement.md`

- [ ] **Step 1:** On the throwaway branch, apply all of leg 1a mechanically:
  - Tasks 1.1 and 1.2;
  - Tasks 2.1–2.5;
  - Task 2.6's header change;
  - Task 3.1's export drop.

  Skip the consumer fixes.
- [ ] **Step 2:** Build every target with `-k 0` in `$CFG` on Linux GCC 15, Linux GCC 13, macOS and Windows (via the Windows workflow, on the throwaway branch, after owner approval to push it).
- [ ] **Step 3:** Write the report:
  - failing targets per platform and configuration, grouped by cause (missing RocksDB, missing sqlite3, missing daemon include, missing moved object, cycle, logger);
  - the count of consumer fixes needed;
  - W6 status;
  - an estimate per stage;
  - gate status per platform, stated as measured.
    - Linux and Windows K4/W5 count as red (missing reference) until reviewed baselines are committed.
    - Windows K3/W3 count as UNMEASURED until the COFF scan is verified.
    - Neither is reported as a pass.
- [ ] **Step 4: STOP.** Send the report to the owner. The v8.2.1 decision and permission to start stage 1 come from the owner. Delete the throwaway branch only after the owner says so.

---

## Stage 1 — declared logger, pbkdf2 into crypto (one PR)

### Task 1.1: `dinero_logging`

**Files:**
- Modify: root `CMakeLists.txt` (new `add_library(dinero_logging STATIC src/common/logger.cpp)`; remove `logger.cpp` from `dinero_core`, root `:1163`), `src/wallet/CMakeLists.txt:15`, `src/consensus/CMakeLists.txt`, `src/storage/CMakeLists.txt`, `src/pool/CMakeLists.txt`, and the `dinero_bridge` and `dinero_crypto` definitions.
- Test: `tests/separation/logger_static_init.cpp`, only if the Step 1 audit finds a static-init use.

**Interfaces:**
- Produces: target `dinero_logging` (PUBLIC include `${CMAKE_SOURCE_DIR}/include`). It is linked PUBLIC by `dinero_crypto`, `dinero_consensus`, `dinero_consensus_core`, `dinero_chainstate`, `dinero_pool`, `dinero_bridge`, `dinero_wallet` and `dinero_core`.

- [ ] **Step 1: Audit.** `git grep -nE 'g_logger|Logger::' -- src include` and filter for uses at namespace scope or in static initializers. Write the result in the commit body. If there are any, write `LoggerStaticInitOrder`: a test linking the library that holds that initializer plus `dinero_logging`, asserting (by return code) that the log call does not crash and lands in the logger. Run it before the change and expect PASS (current behavior). Run it after and expect PASS.
- [ ] **Step 2:** Run the K1 probe build. Expected: still RED, but the `Logger` undefined references in its list are gone (check the log diff).
- [ ] **Step 3:** Make the CMake edits.
- [ ] **Step 4:** Full build plus ctest in `$CFG`. Then `run_consensus_vectors.py --compare` and `consensus_freeze.py check --stage 1`. Expected: all PASS, and K1's log no longer lists any `Logger` symbol.
- [ ] **Step 5:** Commit `build: dinero_logging, declared by every user`.

### Task 1.2: `pbkdf2.cpp` → `dinero_crypto`

**Files:**
- Modify: root `CMakeLists.txt` (the `dinero_crypto` source list near `:542`), `src/wallet/CMakeLists.txt:105` (remove), `src/wallet/reference/CMakeLists.txt:14` and the test entries `tests/CMakeLists.txt:4197,10796,10830,10993`. Remove a test's direct compile only where the test now links `dinero_crypto`.

- [ ] **Step 1:** Show the RED. A target linking `dinero_crypto` alone with whole-archive (add `sep_crypto_forced_link` to `tests/separation`) fails with undefined `PBKDF2_HMAC_SHA512`.
- [ ] **Step 2:** Move the source. Verify `git diff -M100%` shows it unchanged.
- [ ] **Step 3:** Run `sep_crypto_forced_link` and the BIP39 tests (`ctest -R 'BIP39|Bip39' --no-tests=error`). Expected: PASS.
- [ ] **Step 4:** Commit `build: pbkdf2 into dinero_crypto (crypto→wallet edge removed)`.

## Stage 2 — consensus moves (one PR; Task 2.6 gated on approval)

### Task 2.1: `coins_db.cpp`, `coins_view_cache.cpp` → `dinero_chainstate`

**Files:**
- Modify: `src/consensus/CMakeLists.txt` (remove both), `src/storage/CMakeLists.txt` (add both). The sources stay at their paths; only list membership changes.

- [ ] **Step 1:** Make the edit. Build `$CFG`. Expected: no consumer breaks, since none of them is compiled anywhere (spec §6.1). If any target fails, **stop and report**.
- [ ] **Step 2:** Run K5 at stage 2 and K4 compare. Expected: PASS.
- [ ] **Step 3:** Commit `build: coins_db/coins_view_cache into chainstate`.

### Task 2.2: `utxo_position_index.cpp` → `dinero_chainstate`

- [ ] **Step 1:** Move the list entry from consensus to chainstate. Build `$CFG`.
- [ ] **Step 2:** Run `ctest -R '^(UTXOPositionIndexRebuild|CSNIntegration)$' --no-tests=error` and K4. Expected: PASS.
- [ ] **Step 3:** Commit `build: utxo_position_index into chainstate`.

### Task 2.3: `global_utxo_set_impl.cpp` → `dinero_core`

- [ ] **Step 1:** Move the list entry (to root `CMakeLists.txt`, core sources). Build `$CFG`. Run K4 and K5. Expected: PASS.
- [ ] **Step 2:** Commit `build: global_utxo_set_impl into core`.

### Task 2.4: `addr_codec.cpp` out of consensus

- [ ] **Step 1:** Remove `src/address/addr_codec.cpp` from the `dinero_consensus` list. Keep `addr_types.cpp`. Build `$CFG`. Any target that now lacks addr_codec symbols links `dinero_wallet` or `dinero_core` already, or else fails; record any failure and fix it by declaring the right library.
- [ ] **Step 2:** Run K3 on the K1 binary. Expected: no `Address::` symbols. Run K4. Expected: PASS.
- [ ] **Step 3:** Commit `build: addr_codec out of consensus (consensus↔wallet cycle removed)`.

### Task 2.5: Split `dinero_pq`; move `SignP2MR` into the wallet

**Files:**
- Modify: `src/consensus/pq/CMakeLists.txt` (keep `scheme_registry.cpp`, `ml_dsa_65.cpp`, `ml_dsa_65_keygen.c`, `p2mr_consensus.cpp`; drop `sqlite3` from links), `src/wallet/CMakeLists.txt` (add `pq_derivation.cpp`, `secure_keypair.cpp`, `aead_seed.cpp`, `v7_p2mr_store.cpp`, `wallet_key_provider.cpp`, `p2mr_signing.cpp`), root `CMakeLists.txt` (`dinero_rpc_handlers` adds `src/rpc/v7_pq_handlers.cpp`).
- Create: `src/wallet/p2mr_signing.cpp` and `include/wallet/p2mr_signing.h`, holding `SignP2MRParams` and `SignP2MR` moved verbatim from `v7_pq_handlers.cpp:203` and its header.
- Modify: `src/wallet/wallet_key_provider.cpp:18` (include `wallet/p2mr_signing.h` instead of `rpc/v7_pq_handlers.h`) and `src/rpc/v7_pq_handlers.cpp` (include `wallet/p2mr_signing.h`; drop the moved definitions).

**Interfaces:**
- Produces: `dinero::wallet::SignP2MR(const SignP2MRParams&)`, with the exact signature and namespace of the current declaration in `include/rpc/v7_pq_handlers.h`. Only the declaring header changes. `v7_pq_handlers.h` keeps a forwarding `#include "wallet/p2mr_signing.h"` so existing includers compile unchanged.

- [ ] **Step 1:** Show the RED: K2 lists `NODE_SOURCE src/rpc/v7_pq_handlers.cpp`, the pq wallet sources, sqlite3 via pq and a consensus↔pq cycle.
- [ ] **Step 2:** Make the moves. Confirm `p2mr_signing.cpp`'s function bodies are byte-identical to the removed lines (`git diff -M -C --color-moved`).
- [ ] **Step 3:** Declare the pq kernel's primitive dependencies (`dinero_tx_primitives` or the lowest library that defines `uint256::IsNull` and `Transaction::GetBaseSize`).
  - If that library is only `dinero_consensus`, fold the four kernel sources into `dinero_consensus`'s list instead and remove `dinero_pq` from consensus's links. Record the choice in the commit body (spec §13, left to the builder).
- [ ] **Step 4:** Build `$CFG`. Run:
  - `ctest -R '^(WalletP2MRKeyRead|WalletP2MRInventory)$' --no-tests=error`;
  - every ctest whose name matches `P2MR|Pq|PQ`;
  - K4;
  - W5 `check` (P2MR RPCs are in the transcript).

  Expected: PASS. K2 no longer lists pq sources or the pq cycle.
- [ ] **Step 5:** Commit `build: split dinero_pq; SignP2MR moves into the wallet`.

### Task 2.6: `tx_validation` → `const ChainStateView&` (STOP for "CONSENSUS CHANGE" approval first)

**Files:**
- Modify: `include/consensus/tx_validation.h:33,263-268,339-344,361-365`, `src/consensus/tx_validation.cpp:2` and the three definitions (`:72`, `:187`, `:449`; parameter type only), `include/consensus/freeze.h:136` (hash only).
- Create: `tests/separation/tx_validation_binds.cpp` (ctest `SepTxValidationBinds`).

**Interfaces:**
- Produces:
  - `TxValidationOutput validateTransaction(const Transaction&, const ChainStateView& view, …unchanged…)`;
  - `TxValidationResult validateInputs(…, const ChainStateView& view, …)`;
  - `bool checkSequenceLocks(…, const ChainStateView& view, …)`.

  All other parameters are unchanged.

- [ ] **Step 1: Ask for "CONSENSUS CHANGE" approval** with the planned diff, generated in a scratch copy. Do not commit before approval.
- [ ] **Step 2:** Write `SepTxValidationBinds`. It holds static asserts that a `CoinsViewCache&` and a `const ChainStateView&` both bind to each of the three functions (`std::is_invocable_v`), and a run that validates one coinbase-spending transaction through a test `ChainStateView`. Build it against the unchanged header. Expected: compile FAIL on the `const ChainStateView&` asserts.
- [ ] **Step 3:** Make the edit. Run a grep gate: `git grep -nE '\b(validateTransaction|validateInputs|checkSequenceLocks)\s*\(' -- src tests tools` filtered to CMake-compiled files. Expected: only `tx_validation.cpp`.
- [ ] **Step 4:** Update the `freeze.h:136` hash to `sha256sum include/consensus/tx_validation.h`. Run `consensus_freeze.py check --stage 2`. Expected: PASS (the diff limited to §6.5 lines). Run `SepTxValidationBinds`, the tx-validation tests (`ctest -R 'TxValidation|CovenantActivation' --no-tests=error`) and K4 compare. Expected: PASS.
- [ ] **Step 5:** Commit `consensus(CONSENSUS CHANGE): tx_validation takes const ChainStateView&`. The commit body cites the approval and records that the change is source-compatible but not ABI-compatible, and that there are no external users (spec §6.5).

### Task 2.7: `dinero_address_encoding` declares consensus

- [ ] **Step 1:** Add `target_link_libraries(dinero_address_encoding PUBLIC dinero_consensus)`. Run K2 for both gates. Expected: no `CYCLE`.
- [ ] **Step 2:** Run K1 and K3 on all four platforms via the workflows (push after owner approval). Expected: K1 GREEN, K3 GREEN, K6 GREEN, K4 and K5 PASS.
- [ ] **Step 3:** Commit `build: dinero_address_encoding declares its consensus dependency`.

## Stage 3 — drop consensus exports (one PR)

### Task 3.1: Remove RocksDB, sqlite3 and the daemon include from `dinero_consensus`

**Files:**
- Modify: `src/consensus/CMakeLists.txt:195-197,201,219-220,232-233`, plus each consumer the stage 0 report listed.

- [ ] **Step 1:** Remove the three exports.
- [ ] **Step 2:** For each failing consumer in the stage 0 report, add exactly the library or include dir it uses, to that consumer's own `target_link_libraries` / `target_include_directories`. One consumer per commit is fine.
- [ ] **Step 3:** Run K2 `--gate K2` on `$CFG`. Expected: exit 0. Then run a full build plus ctest in `$CFG` on all four platforms, and K4. Expected: PASS.
- [ ] **Step 4:** Commit `build: dinero_consensus is storage-free (exports dropped; consumers declare their own)`.

## Stage 4 — wallet-owned storage helpers (one PR)

### Task 4.1: `dinero_sqlite`

- [ ] **Step 1:** Add `add_library(dinero_sqlite STATIC src/sqlite_open.cpp)` (PUBLIC sqlite3). Remove `sqlite_open.cpp` from `dinero_chainstate` (`src/storage/CMakeLists.txt`). Link `dinero_sqlite` from `dinero_chainstate` and `dinero_wallet`.
- [ ] **Step 2:** Run W4 (`SepWalletFixtureCompat`) in `$CFG`. Expected: PASS. Commit `build: dinero_sqlite shared by wallet and chainstate`.

### Task 4.2: `address_script_builder.cpp` → `dinero_wallet`

- [ ] **Step 1:** Move the list entry from core (root `:1166`) to `src/wallet/CMakeLists.txt`. Build. Run the descriptor/PSBT tests (`ctest -R 'Descriptor|Psbt|PSBT' --no-tests=error`). Expected: PASS.
- [ ] **Step 2:** Commit `build: address_script_builder into the wallet`.

### Task 4.3: `utxo_index.cpp` → `dinero_wallet`; remove direct compiles

**Files:**
- Modify: `src/storage/CMakeLists.txt:19` (remove the entry and the wrong RocksDB comment), `src/wallet/CMakeLists.txt` (add the entry), `tests/CMakeLists.txt` (~18 direct `utxo_index.cpp` entries, e.g. `:905`, `:4438`; the `wallet_test_deps` sources), `rust/orchard_backend/CMakeLists.txt:663,675`.

- [ ] **Step 1:** Show the RED. After moving only the list entry, the ON build fails on `test_orchard_account_delivery` and `test_orchard_index_delivery` with duplicate definitions (or ODR warnings). Record which.
- [ ] **Step 2:** Remove every direct `utxo_index.cpp` compile from targets that link `dinero_wallet`. If a target doesn't link the wallet, link it instead.
  - If `dinero_chainstate`'s own objects reference `UTXOIndex`, add `dinero_wallet` to chainstate's links and record that in the commit body (spec §13).
- [ ] **Step 3:** Run W4 in both configs, `ctest -R 'UTXOIndex|UtxoIndex|WalletRescan|OrchardAccountDelivery|OrchardIndexDelivery' --no-tests=error` (floor-checked), and W5 `check`. Expected: PASS.
- [ ] **Step 4:** Commit `build: UTXOIndex is wallet code; direct compiles removed`.

## Stage 5 — interfaces and adapters (one PR)

### Task 5.1: `node_interfaces.h` and test fakes

**Files:**
- Create: `include/wallet/node_interfaces.h`, `tests/wallet/fake_node_views.h`, `tests/wallet/test_node_interfaces.cpp` (ctest `WalletNodeInterfaces`).

**Interfaces:**
- Produces, in namespace `dinero::wallet`:
  - `WalletChainTip { int32_t height; uint256 hash; }`.
  - `IWalletChainView`: `GetTip() const -> std::optional<WalletChainTip>`, `GetBlockHashAtHeight(int32_t) const -> std::optional<uint256>`, `ReadBlock(const uint256&) const -> std::optional<CBlock>`.
  - `WalletMempoolEntry`: `uint64_t fee`, `size_t tx_size`, `std::vector<uint256> depends` (the current `MempoolEntry` types, `mempool.h:83,87,88`); private `std::optional<std::vector<TxInput>> historical_vin_`; public `const std::vector<TxInput>& vin() const`, which throws `std::logic_error("Orchard transaction has no historical body")` when empty.
  - `WalletMempoolStats { total_size; tx_count; avg_fee_rate; }`, with the current `Mempool::getStats` field types.
  - `IWalletMempoolView`: `IsOutputSpent(const OutPoint&) const -> bool`, `GetEntry(const uint256&) const -> std::optional<WalletMempoolEntry>`, `GetStats() const -> WalletMempoolStats`.
- Produces: `FakeChainView` and `FakeMempoolView`. They are backed by maps and count calls per method (`reads_block_storage` stays false in `FakeChainView`).

- [ ] **Step 1: Write a failing test.** `WalletMempoolEntryOrchardVinThrowsSameError`: an entry with no historical vin; `vin()` throws `std::logic_error` whose `what()` equals that message exactly. A historical entry returns its inputs.
- [ ] **Step 2:** Run `ctest -R '^WalletNodeInterfaces$' --no-tests=error`. Expected: FAIL (header missing).
- [ ] **Step 3:** Write the header and the fakes. The header may include only `primitives/*`, `consensus/outpoint.h`, `<optional>`, `<vector>` and `<stdexcept>`.
- [ ] **Step 4:** Run the test again. Expected: PASS. Commit `wallet: node-facing interfaces`.

### Task 5.2: Chain view, header-only adapter, nullable-view rescan

**Files:**
- Create: `include/storage/chain_db_wallet_view.h`.
- Modify: `include/wallet/wallet_manager.h:796-797,1150-1151` and `src/wallet/wallet_manager.cpp:7479,7505-7507,7557,7638-7640`; the callers `wallet_service.cpp:668-672`, `methods_wallet_context.cpp:6593-6597`, `nodecore_ffi.cpp:1348-1353`, `wallet_worker.cpp:217` and `chainstate_service.cpp:14538,14667`; `reorg_detector.cpp:104`, `hd_wallet` (`ChainHeightProvider` adapter).
- Test: `tests/wallet/test_wallet_chain_view.cpp` (ctest `WalletChainView`).

**Interfaces:**
- Produces: `bool WalletManager::rescanBlockchain(int start_height = 0, int gap_limit = 20, const wallet::IWalletChainView* view = nullptr)`. The private `RescanBlockchainImpl(int, int, const wallet::IWalletChainView*, const SelectedWalletHistory*, uint64_t)` has its guard `if (!view && !prepared) return false;`.
- Produces: `class ChainDbWalletView final : public wallet::IWalletChainView, public ChainHeightProvider`, with constructor `ChainDbWalletView(ChainDB& db, BlockStorage* block_storage /* nullable */)`. `ReadBlock` calls `storage::ReadArchivalBlock(db, block_storage, hash)`, exactly as `wallet_manager.cpp:7640` does now.

- [ ] **Step 1: Write failing tests:**
  - `WalletChainViewNoBlockStorageReadsChainDbOnly`: a `ChainDbWalletView` over a temp ChainDB with a null BlockStorage reads a stored block, and no `BlockStorage` method is called (link a counting BlockStorage double that is never passed).
  - `RescanNullViewPreparedHistoryRuns`: `rescanBlockchain` with a null view and a prepared history reaches the prepared path. With a null view and no prepared history it returns false.
  - `RescanWithFakeViewFindsFundedAddress`: a `FakeChainView` with 3 blocks funding a fixture address; the rescan registers the UTXO.
- [ ] **Step 2:** Run `ctest -R '^WalletChainView$' --no-tests=error`. Expected: FAIL (compile).
- [ ] **Step 3:** Implement the change. Every caller passes `ChainDbWalletView view(*chain_db, block_storage)`, except `wallet_worker` (`block_storage = nullptr`) and ChainstateService (null view). The wallet `.cpp` files drop `storage/chain_db.h` and `storage/archival_block_reader.h`.
- [ ] **Step 4:** Run `WalletChainView`, W5 `check` and W4. Expected: PASS. Then `touch src/wallet/wallet_manager.cpp`, rebuild, and confirm the `.o` rebuilt. Commit `wallet: rescan through IWalletChainView`.

### Task 5.3: Mempool view and migrating M1–M3

**Files:**
- Create: `include/daemon/mempool_wallet_view.h`.
- Modify: `wallet_manager.h:602-604` and `wallet_manager.cpp:6190,6317,6342`; `rbf_cpfp_detector.cpp:85,100,141,182,261`, `fee_bump_engine.cpp:190,260,371`, and their headers; the callers `methods_wallet_context.cpp:1905-1979`, `fee_bump_rpc_handlers.cpp`, `wallet_sync_rpc_handlers.cpp`.
- Test: `tests/wallet/test_wallet_mempool_view.cpp` (ctest `WalletMempoolView`).

**Interfaces:**
- Produces: `std::vector<WalletUTXO> WalletManager::listUnspentUTXOs(int min_confirmations = 1, int max_confirmations = 9999999, const wallet::IWalletMempoolView* mempool = nullptr) const`.
- Produces: `class MempoolWalletView final : public wallet::IWalletMempoolView`, with constructor `explicit MempoolWalletView(const Mempool& m)`. `GetEntry` copies `fee`, `tx_size` and `depends`, and sets the historical vin only when `!entry.tx.IsOrchard()`.
- `RbfCpfpDetector` and `FeeBumpEngine` take `const wallet::IWalletMempoolView*` where they took `const Mempool*`.

- [ ] **Step 1: Write failing tests:**
  - `ListUnspentNullMempoolViewNoFilter`: with a null view, all 2 fixture UTXOs are returned.
  - `ListUnspentFiltersMempoolSpent`: with a `FakeMempoolView` marking one spent, 1 is returned.
  - `RbfDetectorOrchardEntryThrowsSameError`: a `FakeMempoolView` with an Orchard-shaped entry makes the detector throw `std::logic_error` with the exact message.
- [ ] **Step 2:** Run `ctest -R '^WalletMempoolView$' --no-tests=error`. Expected: FAIL.
- [ ] **Step 3:** Implement it. The wallet sources drop `daemon/mempool.h`.
- [ ] **Step 4:** Run `WalletMempoolView`; `ctest -R 'FeeBump|RbfCpfp|test_wallet_fee_bump' --no-tests=error`; and W5 `check` in both configs (the ON transcript includes the Orchard RBF case). Expected: PASS. Commit `wallet: mempool observations through IWalletMempoolView`.

### Task 5.4: Public-header leaks (T1 and the forward declarations)

**Files:**
- Create: `include/wallet/snapshot_wallet_types.h`.
- Modify: `include/daemon/services/snapshot_wallet_source.h` (include the new header; drop the moved definitions), `include/wallet/selected_history.h:3`, `include/wallet/wallet_manager.h:45-58,174-175`, `include/wallet/wallet_worker.h:18,169`.

- [ ] **Step 1:** Show the RED. Add the ctest `SepWalletHeadersSelfContained`, which compiles each `include/wallet/*.h` that `dinero_wallet` sources include, in a TU whose include path excludes `src/daemon`, the storage RocksDB headers and `include/daemon`. Expected: FAIL on `selected_history.h` and `wallet_manager.h`.
- [ ] **Step 2:** Move the types verbatim. Remove the node forward declarations that are no longer used. Keep the friend declarations (spec §7.2).
- [ ] **Step 3:** Run `SepWalletHeadersSelfContained` and the full build in both configs. Expected: PASS. Then W1/W3: still RED only on the D1–D3 symbols. Commit `wallet: public headers free of daemon and storage types`.

## Stage 6 — diagnostics to core, test cleanup (one PR)

### Task 6.1: Diagnostics → `dinero_core`; `sync_phase.h`

**Files:**
- Modify: `src/wallet/CMakeLists.txt` (remove `wallet_sync_status.cpp`, `slow_reason_analyzer.cpp`, `wallet_fee_bump_hooks.cpp`), root `CMakeLists.txt` (add them to core), `include/p2p/block_download_scheduler.h` (include `common/sync_phase.h`), `include/wallet/wallet_sync_status.h:3`.
- Create: `include/common/sync_phase.h` (enum moved verbatim).

- [ ] **Step 1:** Make the moves. Build. Then run W1, W2 and W3 in `$CFG`. Expected: **GREEN** (the first time all three pass).
- [ ] **Step 2:** Run `ctest -R 'WalletSync|SlowReason|FeeBumpHooks|ReorgDetector|SyncProgress' --no-tests=error` and W5. Expected: PASS. Commit `build: node diagnostics move to core; SyncPhase header`.

### Task 6.2: Test link cleanup

- [ ] **Step 1:** For each pure-wallet test that links `wallet_test_deps` or compiles `tests/stubs/mempool_test_stubs.cpp`, switch it to link `dinero_wallet` (plus `fake_node_views.h` where it used a mempool). Each test must keep passing; check with `ctest -R <name> --no-tests=error` per test.
- [ ] **Step 2:** Run the full ctest in both configs on all four platforms, then every gate. Expected: K1–K6 and W1–W5 GREEN; W6 as stage 0 determined. Commit `test: wallet tests link the wallet alone`.

## Stage 7 — required gates and unpinned trials (one PR)

### Task 7.1: Make the gates required and add GCC 15 trials

- [ ] **Step 1:** In both separation workflows, remove `continue-on-error`. Add trial copies of `linux-full-build-ctest`, `linux-quic-full-build-ctest` and `linux-core-heavy` on `ubuntu-latest`, with `continue-on-error: true` at job level. Keep the pinned originals unchanged.
- [ ] **Step 2:** Ask the owner to mark the separation gate jobs as required checks (a repository setting, done by the owner). Push after approval.
- [ ] **Step 3:** Expected: all gates GREEN, and the three trial jobs GREEN on GCC 15. Grep each trial log for `100% tests passed` and for `Linking CXX executable test_header_metadata_recovery`. Commit `ci: separation gates required; GCC 15 trial lanes`.

## Stage 8 — remove the pin (one PR)

### Task 8.1: Remove the ubuntu-24.04 pin

- [ ] **Step 1:** Only after Task 7.1's trials have passed, change `.github/workflows/tests.yml:42,780` and `core-heavy.yml:49` back to `ubuntu-latest`, delete the 5-line pin comments above each, and delete the trial copies.
- [ ] **Step 2:** Push after owner approval. Expected: the three jobs GREEN on ubuntu-latest, with logs showing GCC 15 (`gcc --version`), `100% tests passed`, and the `test_header_metadata_recovery` link line. Commit `ci: remove ubuntu-24.04 pin (GCC 15 links without it)`.

---

## Stop points (owner)

1. **Before pushing anything** (Tasks 0.9, 0.10, 2.7, 7.1, 8.1, and every stage PR).
2. **After Task 0.10:** the v8.2.1 decision and the go-ahead for stage 1.
3. **Task 2.6 Step 1:** "CONSENSUS CHANGE" approval.
4. **Task 0.4 Step 1:** the never-run floor's location.
5. **Task 7.1 Step 2:** the required-check setting.

E1 (W6 repair, if RED) and E3 (manifest repair) are separate decisions outside this plan.
