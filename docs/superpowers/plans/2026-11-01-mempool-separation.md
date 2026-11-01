# Mempool as a Library (`dinero_mempool`) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give the mempool one library, `dinero_mempool`, with one-way dependencies: it calls consensus, reaches chain state only through a storage-free view, and has no edge back to `dinero_core`, the wallet, RPC, P2P or RocksDB. Prove it with gates that run in CI, with no behaviour change.

**Architecture:** First add mechanical gates and record what each shows on the baseline (stage 0). Then put the mempool's two node edges behind interfaces, `IMempoolChainView` and `IMempoolScanSink`, with the ChainDB adapter on the node side (stage 1). Then create the library from the existing sources and replace every direct compile (stage 2). Then trim exports and make the gates required (stages 3–4). Each stage is one PR onto `dinero-main`.

**Tech Stack:** C++20, CMake ≥ 3.20 (File API codemodel v2, read by the gate scripts), Ninja, Python 3, GNU ld / ld64 / MSVC link, GitHub Actions.

**Spec:** `docs/superpowers/specs/2026-11-01-mempool-separation-design.md` (rev 1.2). The spec is the authority; §-numbers below refer to it.

**Execution (prompt Phase C):** Stage 0 only, after plan approval and the owner's choice of execution method. Each task: test-first RED→GREEN, then an independent review before the next task. Progress ledger in the git-ignored `.superpowers/sdd/2026-11-01-mempool-separation/` inside the worktree. Stop after Task 0.8.

**Sibling:** wallet/node separation, draft PR #860, plan `docs/superpowers/plans/2026-10-29-wallet-node-separation.md` (branch `docs/wallet-node-separation-design`). Its Global Constraints, Review Focus and Stage 0 were read before this plan was written; where they bind this project too, they are repeated below.

## Global Constraints

The owner's constraints from the task prompt (`~/src/dinero-evidence/2026-11-01-mempool-separation/PROMPT.md` §5–§6), verbatim:

### §5 Hard constraints (owner; same as the wallet project)

- **Separate from any release.** Whether it ships, and in which version, is decided after Stage 0 measures the scope.
- **No behaviour change:**
  - admission policy, RBF/CPFP, fee estimation, eviction/expiry, orphan handling, persistence format, the invalid-tx cache, covenant policy;
  - Orchard mempool entries;
  - P2P relay;
  - RPC methods, fields and errors;
  - `include/nodecore/nodecore_ffi.h`, `wallet-core/ffi/wallet_ffi.h`, the JNI exports, Qt and NodeCore behaviour.
- **No consensus logic change.** Frozen headers (`docs/consensus_map.md`) are not edited.
- Internal C++ interfaces may change. Any other unavoidable external change is flagged for a separate owner decision, never folded in.
- **Moves are surgical.** A moved file keeps its contents byte-for-byte unless a step names the change (verify with `git diff -M100% --stat`). Dead code is moved or left alone, not deleted.
- **Anti-goals:**
  - a gate that passes because of stubs or link order;
  - moving a dependency instead of removing it.
- **Never-run floor.** No gate or test run executes a test on the release evidence's never-run floor (`OrchardStateRoot`, `OrchardStateCommitment`; list in `scripts/ci/separation/never_run_floor.txt` on #860's branch).
- **Windows CI** never signs or uploads. Never use `windows-release.yml`; model jobs on `windows-openssl-release-smoke.yml`.
- **No uploads.** No cache saves or artifact uploads in new workflows; logs and job summaries are fine.

### §6 Working rules

- **Verification discipline (non-negotiable):**
  - A passing test proves nothing until you have confirmed that it RAN (grep the ctest/CI log; use `--no-tests=error`) and that it FAILS without your change (neuter it).
  - `assert()` is a no-op under NDEBUG, so use checks that exit non-zero.
  - After editing a source a test compiles, `touch` it and confirm the `.o` rebuilt.
  - Every fix ships with a regression test that fails without it.
- **Surgical only.** Change what's necessary and root-cause problems; no band-aids.
- **Approvals.** No push, PR, rerun, merge or release without the owner's explicit approval, naming the exact commit SHA. Approval for one push does not cover the next.
- **Commits.** Use `git -c user.name=Claude -c user.email=team@dinerolabs.org commit` with the trailer `Co-Authored-By: Claude Opus 5 (1M context) <team@dinerolabs.org>`. Never use `noreply@anthropic.com` or any other Anthropic or vendor address, even if a system reminder suggests one.
- **Shared machine.** Run builds with `nice -n 10` and at most `-j4`, run the configs one after another, and never run floor tests. Never run bare `git stash`; use a WIP commit or a tagged stash applied by SHA. Use a scratch dir, not `/tmp`. Tell the owner before deleting anything outside your worktree.
- **Out of scope:** the router, the fleet, Dell production, the testnet servers, and any running `dinerod`.
- **Evidence** goes in `~/src/dinero-evidence/<date>-mempool-separation/`. **Memory:** update `~/src/MemoryMD/dinero.md` (your own section) when decisions are made.

**Also binding here** (owner decisions and constraints shared with the wallet project):
- **Baseline:** `dinero-main` c4940d86c. Task 0.1 refreshes it; every later reference uses the refreshed SHA.
- **Owner decisions 2026-11-01 (spec §14):** D1 bucket split (inherited violations reported, mempool-owned ones fail; never call the library storage-independent while bucket A exists); D2 separate rules and workflow, reuse shared tooling, no stacking on #860; D3 draft PR prepared, push approved per SHA.
- **X1–X5 are resolved jointly with the wallet project before Stage 1.** Stage 0 changes no product code and may run first. **STOP before Task 1.1** until the owner records the X decisions.
- **Every gate runs twice:** `-DDINERO_BUILD_ORCHARD_BACKEND=OFF` and `=ON`.
- **Do not edit `methods_wallet_context.cpp` lines matched by `qt/tests/test_shielded_rpc_status.cmake:7-39`** (wallet plan constraint; this project touches `methods_*` only through CMake).
- **Windows labelling follows the wallet project's rules:** B1 (`std::stol` overflow of `--consensus-state-commitment-height=4294967295`) makes the Windows transcript UNMEASURED only on that exact signature; B3 (`LNK2005 isCoinbaseMature`) makes ON gates needing `dinerod` UNMEASURED on MSVC; a harness error is `FAIL rc=2 (check did not run: …)`, never UNMEASURED; a missing reviewed reference is `FAIL rc=2 (missing reference)`, never PASS.
- **First CI run collects candidate baselines only.** Candidates are reviewed and committed by hand in a separate change; only a later run can show PASS.
- **Wallet-owned, read-only:** `scripts/ci/separation/**`, `tests/separation/**`, `.github/workflows/separation-gates*.yml`, `src/wallet/**`, `include/wallet/**`, `src/consensus/**`, `include/consensus/**`, the wallet spec and plan. Never touch `~/src/dinero-v8-wallet-sep`, `~/src/dinero-v8-measure-leg1a`, `~/src/dinero-sep-builds/{off,on,measure-*}`, PR #860 or its branches, or Codex's worktrees.
- **Own space:** worktree `~/src/dinero-v8-mempool-sep`, branch `docs/mempool-separation-design` (docs) and per-stage branches; builds `~/src/dinero-sep-builds/mempool-{off,on}` (`$CFG` below means each in turn, never in parallel). Configure flags: `-G Ninja -DCMAKE_BUILD_TYPE=Release -DDINERO_USE_VENDORED_DEPS=ON -DENABLE_GRPC=OFF -DBUILD_NODECORE=ON -DENABLE_TESTS=ON -DDINERO_MEMPOOL_SEPARATION_GATES=ON -DDINERO_BUILD_ORCHARD_BACKEND=<OFF|ON>` (+ the local `-DDINERO_VENDORED_OPENSSL_DIR=…/prebuilt/macos-arm64`).
- **Stop and report** (don't work around it) if:
  - `mempool.cpp` needs a ChainDB operation beyond spec §7.1, or a core symbol beyond §6.1;
  - M1–M3, `MempoolEntry` or `MempoolTransaction` would need a change;
  - any consensus file or frozen header would need an edit;
  - MP4 differs anywhere;
  - a `KEY_TIP` writer does not hold the block-ingress activation lock (Task 0.2);
  - the ON closure of `dinero_orchard_mempool_body` reaches `dinero_core` or chainstate;
  - any gate would need a floor test.

## Review Focus

These failure modes are the most likely to hurt a user and are not otherwise covered. Each is pinned by a test in the named task.

1. **`testmempoolaccept` changes because a read moved under (or out of) the guard.** The view must not add or remove locking on any path, guarded or not (spec §7.4). Pinned by `MempoolChainReadSitesUnchanged` (Task 0.2 records, Task 1.3 checks): per-function multiset of chain reads and guard positions identical to baseline.
2. **Orchard admission starts rejecting with "Configured scanner cannot consume Orchard body".** A null or wrong sink must not flip the reachable path. Pinned by `MempoolScanSinkOrchardAdmission` (Task 1.2): with the production sink an Orchard body is admitted exactly as today; with a fake sink returning null the UNAVAILABLE rejection text and code are byte-identical.
3. **After the source moves (ON), Orchard entries vanish from `mempool.dat` on restart.** `relay_transaction_reader.cpp` needs `DINERO_HAS_ORCHARD_RELAY_READER=1` (a SOURCE property set from `rust/orchard_backend/CMakeLists.txt:183-184`); if it is lost, load decodes historical bodies only. Pinned by MP4(b)(iii) and `MempoolRelayReaderOrchardDefine` (Task 2.1): a compile-time check in an ON build that the define is set for the object in `dinero_mempool`.
4. **A stub silently wins over the real archive.** A target that links core and still compiles `mempool_test_stubs.cpp` gets no-op `isOutputSpentInMempool` etc. without a link error on GNU/Apple. Pinned by MP2's stub rule over all codemodel targets (Task 0.4) and `MempoolStubRuleAllTargets` (Task 2.3).
5. **A cache creeps into the adapter.** Coherence depends on the adapter holding nothing. Pinned by `ChainDbMempoolViewIsStateless` (Task 1.1): `static_assert(sizeof(ChainDbMempoolView) == 2 * sizeof(void*))` plus the no-mutex check in the call-site script.
6. **The relocated commitment scan changes.** Same predicate, same early exit, still `ChainDB::forEachUTXO` (with its `/tmp` diagnostic side effect). Pinned by `ChainDbMempoolViewCommitmentScan` (Task 1.1) against a regtest ChainDB with two confidential outputs (hit first, hit second, miss, empty commitment).

---

## File structure

| Path | Owner | Purpose |
|---|---|---|
| `scripts/ci/mempool-separation/mp_closure.py` | this project | MP2 wrapper importing `check_link_closure` (X5); bucket A/B split |
| `scripts/ci/mempool-separation/mp_symbols.py` | this project | MP3 wrapper importing `check_symbols` (X5) |
| `scripts/ci/mempool-separation/mempool_closure_rules.json` | this project | MP2 rules + path allowlist (spec §6.4) |
| `scripts/ci/mempool-separation/chain_read_sites.py` | this project | per-function chain-read and guard-site inventory (spec §7.4) |
| `scripts/ci/mempool-separation/design_anchors.tsv` | this project | file:line anchors this plan acts on |
| `scripts/ci/mempool-separation/tests/` | this project | pytest for the above, incl. the X5 API pin |
| `tests/mempool_separation/CMakeLists.txt` | this project | `sep_mempool_probe`, `sep_mempool_forced_link`, `sep_mempool_standalone`, ctests |
| `tests/mempool_separation/{forced_link,standalone}.cpp`, `fake_chain_view.h` | this project | MP1, MP5 |
| `tests/mempool_separation/mempool_transcript.py`, `transcripts/{off,on}.json` | this project | MP4 |
| `.github/workflows/mempool-gates{,-windows}.yml` | this project | non-required gate workflows |
| root `CMakeLists.txt` | **shared hot file** | one `option(DINERO_MEMPOOL_SEPARATION_GATES …)` + `add_subdirectory` block (stage 0); `dinero_mempool` block and core link line (stage 2) |
| `include/daemon/mempool_chain_view.h`, `mempool_scan_sink.h`, `include/storage/chain_db_mempool_view.h`, `include/storage/coin.h` | this project (stage 1; `coin.h` per X4) | interfaces and adapter |

`check_design_anchors.py` is reused from #860 unmodified (it is gate-name-free). Until #860's Stage 0 is on `dinero-main`, local runs set `SEP_TOOLS=~/src/dinero-v8-wallet-sep/scripts/ci/separation` and **only read** from it.

---

## Stage 0 — refresh, gates, measurement (one PR, gates non-required)

### Task 0.1: Baseline refresh and design anchors

**Files:**
- Create: `scripts/ci/mempool-separation/design_anchors.tsv`

**Interfaces:**
- Consumes: `check_design_anchors.py <repo> <tsv>` (exit 0 all match; prints `MOVED path old->new` / `MISSING path:line`).

- [ ] **Step 1:** Fill `design_anchors.tsv`, one row per file:line the spec's §6.1–§7.4 tables cite (e.g. `src/daemon/mempool.cpp	10	storage/chain_db.h`, `src/daemon/mempool.cpp	793	acquireEmptyRegistry`, `include/storage/chain_db.h	37	struct Coin`, `src/daemon/services/mempool_service.cpp	125	std::make_unique<Mempool>`).
- [ ] **Step 2:** Run the checker at c4940d86c. Expected: exit 0 (anchors were taken there).
- [ ] **Step 3: Neuter check.** Change one anchor's line by +1 in a scratch copy. Expected: exit 1 with exactly one `MOVED` line. Revert.
- [ ] **Step 4:** Rebase onto current `origin/dinero-main`; run again. Expected: exit 0 or a MOVED/MISSING list. Update MOVED rows. **Stop and report** any MISSING row.
- [ ] **Step 5:** Re-check the `v8.2.0` tag (`git ls-remote --tags https://github.com/DineroLabs/dinero-v8.git 'v8.2.0*'`) and whether #860 re-anchored; record both in the commit body.
- [ ] **Step 6:** Commit `ci(mempool-sep): design anchors`, refreshed SHA in the body.

### Task 0.2: Chain-read inventory and the tip-writer trace (spec §7.4)

**Files:**
- Create: `scripts/ci/mempool-separation/chain_read_sites.py`, `scripts/ci/mempool-separation/chain_read_sites_baseline.json`, `scripts/ci/mempool-separation/tests/test_chain_read_sites.py`

**Interfaces:**
- Produces: `chain_read_sites.py record|check <mempool.cpp> [--baseline json]`. Output per enclosing function: the multiset of chain reads (`getTip`, `getCoinWithConfidentialFallback`, `getTxLocation`, `forEachUTXO`/`HaveConfidentialCommitment`, fallback `getCoin`/`hasCoin`, and their view-method equivalents mapped to the same names), the lines of `chainstate_read_guard_factory_()` calls, and whether each precedes the function's first `m_mutex` lock. `check` exits 1 on any difference, printing `DIFF <function> <before> -> <after>`. It also fails if `ChainDbMempoolView`'s method bodies contain `mutex`, `lock_guard`, `unique_lock` or `shared_lock`.

- [ ] **Step 1: Write failing tests:** `test_records_reads_per_function` (fixture C++ with two functions, one guarded), `test_view_call_maps_to_same_read`, `test_moved_guard_is_diff`, `test_adapter_mutex_rejected`.
- [ ] **Step 2:** `python3 -m pytest scripts/ci/mempool-separation/tests/test_chain_read_sites.py -v`. Expected: 4 FAIL (script missing).
- [ ] **Step 3:** Implement.
- [ ] **Step 4:** Run again. Expected: 4 PASS.
- [ ] **Step 5:** `record` at the baseline. Expected: the 23 chain-read sites and 5 guarded functions of spec §7.4 (`submitTransactionInternal`, `submitOrchardBody`, `getConfirmedWalletCoins`, `CaptureEntriesWithInputCoins`, `CaptureBlockSelectionImpl`). Any difference from the spec → update the spec's table first, then the baseline.
- [ ] **Step 6: Tip-writer trace (read-only).** List every caller of `ChainDB::setTip` (the only `KEY_TIP` writer, `chain_db.cpp:1089`, `:1096`) and of any write batch carrying it, and for each name the lock held (expected: the block-ingress activation lock, `AcquireBlockIngressActivationLock`). Save `tip-writers.md` in the evidence dir with file:line per caller. Expected: every writer holds it. **Stop and report** if one doesn't (pre-existing defect; not fixed here).
- [ ] **Step 7:** Commit `ci(mempool-sep): chain-read site inventory (spec §7.4)`.

### Task 0.3: Probe archive and forced link (MP1) and symbols (MP3)

**Files:**
- Create: `tests/mempool_separation/CMakeLists.txt`, `tests/mempool_separation/forced_link.cpp`, `tests/mempool_separation/fake_chain_view.h`, `scripts/ci/mempool-separation/mp_symbols.py`, `scripts/ci/mempool-separation/tests/test_mp_symbols.py`
- Modify: root `CMakeLists.txt` — one contiguous block next to the existing `DINERO_SEPARATION_GATES` area (or, if #860 isn't merged, after the guardrail targets at `:63-67`): `option(DINERO_MEMPOOL_SEPARATION_GATES "Build mempool separation gate probes" OFF)` and `if(DINERO_MEMPOOL_SEPARATION_GATES) add_subdirectory(tests/mempool_separation) endif()`. Record the exact lines for the merge-order list.

**Interfaces:**
- Produces: `sep_mempool_probe` (STATIC, `EXCLUDE_FROM_ALL`), the planned `dinero_mempool` source list of spec §6.7 with the planned PUBLIC deps; `sep_mempool_forced_link` (whole-archive `sep_mempool_probe` + declared deps only; GNU `--whole-archive … --no-undefined`, Apple `-force_load`, MSVC `/WHOLEARCHIVE`); ctest `SepMempoolForcedLink` (label `mempool_separation`, build-then-run). `main` constructs `dinero::Mempool` with null view/sink (baseline ctor takes `ChainDB* = nullptr`) and checks `size()==0`, returning nonzero otherwise (no `assert`).
- Produces: `mp_symbols.py --binary <path>` → `FORBIDDEN <pattern> <symbol>` lines, exit 1 on any. Patterns (spec §8 MP3): `rocksdb::`, `sqlite3_`, `ChainDB`, `ChainstateService`, `din::sp::`, `PeerManager`, `P2PService`, `RPCServer`, `WalletManager`, `grpc::`. Bucket A patterns reached only via consensus/pq objects are labelled `INHERITED`.
- X5 pin: `test_x5_symbols_api` imports `check_symbols` from `$SEP_TOOLS` and asserts `parse`, `symbols`, `nm_text` exist with their current signatures.

- [ ] **Step 1: Write failing tests:** `test_mp_symbols_flags_chaindb`, `test_mp_symbols_clean`, `test_inherited_label`, `test_x5_symbols_api`.
- [ ] **Step 2:** Run pytest. Expected: FAIL.
- [ ] **Step 3:** Implement the wrapper, the CMake block and `forced_link.cpp`.
- [ ] **Step 4:** Run pytest. Expected: PASS. Then `nice -n 10 cmake --build $CFG --target sep_mempool_forced_link -j4 -k 0`. Expected **RED** (link errors) listing exactly the spec §8 set: `ChainDB::{getTip,getCoinWithConfidentialFallback,getTxLocation,forEachUTXO}`, `din::sp::ScannerManager::{ScannerManager,~ScannerManager,scanMempoolTransaction}` (+ `acquireEmptyRegistry` if out-of-line), plus bucket A. Save logs to `~/src/dinero-evidence/2026-11-01-mempool-separation/stage0/`. **Stop and report** any other undefined symbol.
- [ ] **Step 5:** MP3 on the probe's objects (`nm -C` of the archive, since the executable didn't link). Expected: RED with `ChainDB`, `din::sp::` references.
- [ ] **Step 6:** Commit `ci(mempool-sep): probe archive, forced link and symbol checks (MP1/MP3)`.

### Task 0.4: Closure check (MP2) with the stub rule

**Files:**
- Create: `scripts/ci/mempool-separation/mp_closure.py`, `scripts/ci/mempool-separation/mempool_closure_rules.json`, `scripts/ci/mempool-separation/tests/test_mp_closure.py` (+ fixture codemodels)

**Interfaces:**
- Produces: `mp_closure.py --build <dir> --target <name> [--all-targets-stub-rule]`. Imports `Model`, `check`, `load_json` from `check_link_closure` (X5). Prints the wallet checker's line types plus `BUCKET A|B` per violation (A = every path from the root to the violation passes through `dinero_consensus` or `dinero_pq`, or is the undeclared logger), and `STUB_IN_MEMPOOL_CLOSURE <target>`. Exit 1 iff any bucket-B violation; bucket A is printed and summarised, never hidden. Reverse check: exit 1 if any target in `dinero_consensus`'s closure has `dinero_mempool`/`sep_mempool_probe` in its closure.
- Rules: spec §8 MP2 allowed/forbidden sets; allowlist of spec §6.4 with reasons.

- [ ] **Step 1: Write failing tests:** `test_bucket_a_via_consensus`, `test_bucket_b_direct_chainstate`, `test_stub_rule_flags_core_plus_stub`, `test_stub_rule_allows_stub_without_core`, `test_reverse_edge_from_consensus`, `test_x5_closure_api`.
- [ ] **Step 2:** Run pytest. Expected: 6 FAIL.
- [ ] **Step 3:** Implement.
- [ ] **Step 4:** Run pytest. Expected: 6 PASS.
- [ ] **Step 5:** On `$CFG`: `--target sep_mempool_probe`. Expected **RED**: bucket B `dinero_chainstate`/RocksDB (ChainDB) and `dinero_core` (scanner, if the probe declares what the sources need) — or, with only the planned deps, the gaps show in MP1 instead; record which. Bucket A listed. `--all-targets-stub-rule` at baseline: expected PASS (no `dinero_mempool` exists, the probe has no stub users). Save outputs.
- [ ] **Step 6: Neuter check.** Add `tests/stubs/mempool_test_stubs.cpp` to a scratch target linking the probe. Expected: `STUB_IN_MEMPOOL_CLOSURE`. Revert.
- [ ] **Step 7:** Commit `ci(mempool-sep): closure check with bucket split and stub rule (MP2)`.

### Task 0.5: Behaviour freeze (MP4)

**Files:**
- Create: `tests/mempool_separation/mempool_transcript.py`, `tests/mempool_separation/transcripts/{off,on}.json`, `tests/mempool_separation/transcript_mask.txt`

**Interfaces:**
- Produces: `mempool_transcript.py record|check --dinerod <path> --out <json> --cfg <off|on>`. Starts regtest in a temp datadir (scratch dir, not `/tmp`), mines maturity, then runs spec §8 MP4(a) in order (incl. the reorg case), then (b): `stop`, decode `mempool.dat` (magic, version, entries sorted by txid), restart, compare `getrawmempool true`. Normalisation as the wallet's W5 (`<H#n>`, `<A#n>`, `<T>`), plus the reviewed `transcript_mask.txt`. Refuses (exit 2) if any invoked ctest name is on `never_run_floor.txt` (read from `$SEP_TOOLS`).
- ON adds: an Orchard mempool entry (accepted) and its presence after reload.

- [ ] **Step 1:** `record` at the baseline, OFF then ON.
- [ ] **Step 2:** `check` at the baseline. Expected: PASS (twice in a row, to prove the masks remove all run-varying fields).
- [ ] **Step 3: Neuter checks** (scratch, reverted): (a) change one `getmempoolinfo` field name → FAIL; (b) make `saveToDisk` skip the last entry → FAIL in (b); (c) ON only: drop `DINERO_HAS_ORCHARD_RELAY_READER` → FAIL in (b)(iii).
- [ ] **Step 4:** Commit `test(mempool-sep): behaviour freeze transcript and persistence check (MP4)`.

### Task 0.6: Standalone consumer (MP5)

**Files:**
- Create: `tests/mempool_separation/standalone.cpp`; modify `tests/mempool_separation/CMakeLists.txt`

**Interfaces:**
- Produces: `sep_mempool_standalone`, linked only with `target_link_libraries(... sep_mempool_probe)` (later `dinero_mempool`), no core, no stubs, no hand-listed archives; ctest `SepMempoolStandalone`. On the fake chain view it submits a valid tx (accepted), an invalid one (rejected, frozen reason text), saves, clears, loads, compares; returns nonzero on any mismatch.

- [ ] **Step 1:** Write it against the baseline API (`ChainDB* = nullptr`; a fake chain view can't be injected yet — record that).
- [ ] **Step 2:** Build. Expected **RED**: link fails with the Task 0.3 set. Save log.
- [ ] **Step 3:** `mp_closure.py --target sep_mempool_standalone`. Expected: as Task 0.4.
- [ ] **Step 4:** Commit `test(mempool-sep): standalone consumer (MP5)`.

### Task 0.7: Non-required CI workflows (prepared, not pushed)

**Files:**
- Create: `.github/workflows/mempool-gates.yml`, `.github/workflows/mempool-gates-windows.yml`

**Interfaces:**
- Matrix {`ubuntu-latest` GCC 15, `ubuntu-24.04` GCC 13, `macos-15` Xcode 16.4} × {OFF, ON}; Windows modelled on `windows-openssl-release-smoke.yml` (no signing, no `secrets.*`, no uploads, no release). Cache steps restore-only. Each gate step `continue-on-error: true`, echoes `GATE MP<n> <PASS|FAIL|UNMEASURED> why=<reason>`; summary to `$GITHUB_STEP_SUMMARY`; bucket A shown on every MP1–MP3 row. Triggers: `pull_request` with paths on the files of this plan.
- `mempool-separation-script-tests`: pytest on ubuntu-24.04 with pinned pytest, no native build.
- If `scripts/ci/separation/` is absent on the base branch, MP2/MP5-closure print `GATE MP2 UNMEASURED why=shared tooling not on base (#860 Stage 0 not landed)` (owner decision on that dependency, spec §14).

- [ ] **Step 1:** Write the workflows. Grep for `sign`, `secrets.`, `upload`, `release`, `save`. Expected: no matches apart from comments.
- [ ] **Step 2:** Run every step's command locally on macOS OFF and ON. Expected: the Stage 0 expectations above.
- [ ] **Step 3:** Commit `ci(mempool-sep): non-required gate workflows`.
- [ ] **Step 4: STOP — D3.** Prepare the draft PR text (base `dinero-main`, head this branch, title `[draft] mempool separation stage 0: gates (non-required)`), the exact run scope (the two workflows and the lanes above; no other workflow triggered by these paths, verified by grepping every workflow's `paths:`), and the expected per-lane results. Send the owner the exact SHA. **No push without approval of that SHA.** The first run only collects candidate baselines.

### Task 0.8: Scope measurement → owner release decision (STOP)

**Files:**
- Create: throwaway branch `measure/mempool-separation` (never merged), worktree `~/src/dinero-v8-mempool-measure`, builds `~/src/dinero-sep-builds/mempool-measure-{off,on}`; report `~/src/dinero-evidence/2026-11-01-mempool-separation/stage0/measurement.md`

- [ ] **Step 1:** On the throwaway branch apply Stages 1–2 mechanically (interfaces, adapter, ctor switch, library, consumers declare it, remove direct compiles, apply the X3 rule). Skip consumer fixes beyond that.
- [ ] **Step 2:** Build every target with `-k 0` in both configs locally; Linux/Windows via the workflow only after owner approval to push the throwaway branch (separate approval).
- [ ] **Step 3:** Report: failing targets per platform/config grouped by cause (missing scanner sink, missing view, duplicate definitions, stub rule, ODR/LNK2005, export reliance on core), count of consumer fixes, MP1–MP5 status as measured with bucket A shown, the ~46 test construction sites, estimate per stage. Nothing UNMEASURED or missing-reference is reported as a pass.
- [ ] **Step 4: STOP.** Owner decides release fit and permission for Stage 1 (which also needs the X decisions). Delete the throwaway branch only when told.

---

## Stage 1 — interfaces and the adapter (one PR; needs X1–X5 resolved)

### Task 1.1: `IMempoolChainView`, `Coin` header, `ChainDbMempoolView`

**Files:**
- Create: `include/daemon/mempool_chain_view.h`, `include/storage/chain_db_mempool_view.h`, `include/storage/coin.h` (per X4), `tests/mempool_separation/test_chain_db_mempool_view.cpp`
- Modify: `include/storage/chain_db.h:37-44` → `#include "storage/coin.h"` (one contiguous block; `git diff` shows only that)

**Interfaces:** spec §7.1, return types per X4.

- [ ] **Step 1: Write failing tests:** `ChainDbMempoolViewIsStateless` (static_assert size), `ChainDbMempoolViewForwardsEachCall` (regtest ChainDB: tip, coin hit/miss, tx present/absent equal direct ChainDB results), `ChainDbMempoolViewCommitmentScan` (Review Focus 6).
- [ ] **Step 2:** Build. Expected: compile FAIL (headers missing).
- [ ] **Step 3:** Implement. Move `struct Coin` byte-for-byte.
- [ ] **Step 4:** Run ctest `--no-tests=error -R 'ChainDbMempoolView'`; grep the log for all three names. Expected: PASS. **Neuter:** make the adapter cache the tip → static_assert fails; make the scan return true on empty commitment → FAIL.
- [ ] **Step 5:** Full build OFF/ON. Expected: no other change (Coin users compile unchanged).
- [ ] **Step 6:** Commit `refactor(mempool): storage-free chain view and ChainDB adapter`.

### Task 1.2: `IMempoolScanSink`

**Files:**
- Create: `include/daemon/mempool_scan_sink.h`; core implementation beside `mempool_service.cpp`; test `tests/mempool_separation/test_mempool_scan_sink.cpp`
- Modify: `include/daemon/mempool.h:29,773`, `src/daemon/mempool.cpp:14,468,793-794,1104-1106` (named lines only)

- [ ] **Step 1: Write failing tests:** `MempoolScanSinkOrchardAdmission` (Review Focus 2; ON only for the Orchard half), `MempoolScanSinkScansAfterAdmission` (fake sink records the hex after an ordinary admission).
- [ ] **Step 2:** Build. Expected: FAIL.
- [ ] **Step 3:** Implement; `MempoolService` passes the `ScannerManager`-backed sink.
- [ ] **Step 4:** Run, grep log. Expected: PASS. Neuter: sink returns null in production wiring → MP4 ON FAIL.
- [ ] **Step 5:** Commit `refactor(mempool): inject the silent-payment scan sink`.

### Task 1.3: Switch `Mempool` to the view; migrate construction sites

**Files:**
- Modify: `include/daemon/mempool.h` (ctor, `chain_db_` member), `src/daemon/mempool.cpp` (three adapters, 23 read sites; `storage/chain_db.h` include removed), `src/daemon/services/mempool_service.{h,cpp}` (view member declared before `mempool_`), the ~46 test files

- [ ] **Step 1:** `chain_read_sites.py check` against the Task 0.2 baseline. Expected now: FAIL is impossible to observe before the edit — so first neuter-check the script on a scratch edit that moves one guard below `m_mutex`: Expected FAIL. Revert.
- [ ] **Step 2:** Edit. Then `chain_read_sites.py check`. Expected: PASS (`MempoolChainReadSitesUnchanged`).
- [ ] **Step 3:** `touch src/daemon/mempool.cpp`; rebuild; confirm the `.o` rebuilt; `nm -u -C` it. Expected: no `ChainDB::` and no `din::sp::` undefined symbols.
- [ ] **Step 4:** MP4 OFF/ON. Expected: PASS against the committed reference. Full ctest (non-floor).
- [ ] **Step 5:** Commit `refactor(mempool): read chain state through IMempoolChainView`.

## Stage 2 — the library (one PR)

### Task 2.1: `add_library(dinero_mempool)`

**Files:** root `CMakeLists.txt` (new block before `add_library(dinero_core` at `:1063`; remove the moved sources from `:991-995`, `:1043-1049`, `:1120`, `:1127`, `:1208`; add `target_link_libraries(dinero_core PUBLIC dinero_mempool)`); `rust/orchard_backend/CMakeLists.txt:181-184` (`if(TARGET dinero_mempool)` body link); `tests/mempool_separation/CMakeLists.txt` (probe → `dinero_mempool`).

- [ ] **Step 1:** MP1/MP3/MP5 on `dinero_mempool`. Expected: RED only where Stage 2.2 has not yet removed duplicates; bucket B otherwise zero. `MempoolRelayReaderOrchardDefine` (ON): FAIL before the property check is added, PASS after.
- [ ] **Step 2:** Implement. `git diff --stat` shows CMake files only.
- [ ] **Step 3:** MP1 PASS (bucket B), MP3 PASS (bucket B), MP5 links and runs. Commit `build(mempool): dinero_mempool library`.

### Task 2.2: Remove direct compiles

**Files:** `src/daemon/CMakeLists.txt:58,133`, `src/nodecore/CMakeLists.txt:50`, `tests/CMakeLists.txt` (50 live entries; `dinero_attach_real_mempool_fixture_runtime` keeps stripping stubs but stops adding `dinero_test_mempool_runtime`, which is deleted), `tests/mining/CMakeLists.txt:147,195,374,461`; `dinero_rpc_handlers` declares `dinero_mempool`; NodeCore xcframework script bundles `libdinero_mempool.a`.

- [ ] **Step 1:** Before: `grep -c 'daemon/mempool.cpp'` in compile_commands. Expected 57 targets. After: Expected exactly 1 (`dinero_mempool`).
- [ ] **Step 2:** Full build both configs, `-k 0`. Expected: green, or the Task 0.8 list.
- [ ] **Step 3:** MP4 PASS; full non-floor ctest. Commit `build(mempool): link dinero_mempool instead of compiling mempool.cpp`.

### Task 2.3: Apply the stub rule (X3)

- [ ] **Step 1:** `mp_closure.py --all-targets-stub-rule`. Expected: FAIL listing the stub users that link core.
- [ ] **Step 2:** Drop the stub from each (CMake only). Expected: PASS (`MempoolStubRuleAllTargets`). W4's stub untouched (wallet-owned; X1/X3).
- [ ] **Step 3:** Commit `test(mempool): stub only where no core or dinero_mempool is linked`.

## Stage 3 — exports (one PR)

### Task 3.1: Trim `dinero_mempool`'s PUBLIC set; consumers declare their own

- [ ] **Step 1:** Remove each PUBLIC dep not proven necessary by MP1; build `-k 0`; record what breaks; each consumer declares what it uses.
- [ ] **Step 2:** MP2 bucket B = 0 on all platforms, both configs. Commit `build(mempool): declare only what the library uses`.

## Stage 4 — required gates (one PR)

### Task 4.1: Make MP1–MP5 required (bucket B)

- [ ] **Step 1:** Owner chooses the required-check setting. Remove `continue-on-error` from MP steps.
- [ ] **Step 2:** After wallet Stages 1–3 land and bucket A is empty, a follow-up change turns bucket A into a failure and the docs may then call the library storage-independent (D1).
