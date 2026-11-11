# Consensus-kernel purity: shared-globals migration (sep3)

**Status:** stage-0 inventory complete (2026-11-10, measured at merge ed23b8928); design draft.
**Baseline:** dinero-main ed23b8928 (post wallet separation, post node-friend-access removal #892/#894).
**Sibling work honored, not advanced:** draft PR #877 (CV1 header-reach gate, report-only, owner hold of
2026-11-01 on CV2/CV3 review) — this program reuses its measurement approach and bucket semantics; it does
not touch #877's branch. Draft PR #893 (mempool library extraction, in flight) — stages touching root
`CMakeLists.txt`, `src/daemon/`, `src/nodecore/`, `rust/orchard_backend/` or `tests/` build files are
sequenced AFTER #893 lands (§6).

## 1. Goal

Make the consensus libraries' freedom from ambient process state **measured, enforced, and then real**:
consensus code decides rule-validity from its inputs (block, params, clock, config), not from globals it
reaches behind its callers' backs. End state of THIS program (not all of "purity", see §3):

- `dinero_consensus_core` ("the kernel") reads **zero** ambient globals: no `Params()`/`GetActiveChain()`/
  `MutableParams()`, no `DaemonContext::instance()`, no `daemon::GetConfig()`, no `getenv`, no wall clock,
  no weak-stub `ParamsImpl` dependency. Chain parameters arrive as arguments.
- `dinero_consensus` (the larger library) is free of ambient **chain-selection** reads (its ~13 `Params()`
  lines migrated to passed parameters); its remaining globals (sig/script caches, logger) are baselined and
  ratcheted, not eliminated here.
- A required, always-run, matrix-free CI gate (**P1**, modeled on F1) enforces both, with a committed
  baseline that can only shrink.

## 2. Measured scope (stage-0 inventory, ed23b8928)

Totals: ~40 distinct ambient globals; **832 `Params()` occurrences / 750 lines / 129 production files**
(+ ~718 in tests, + 372 test `MutableParams`). By directory: daemon 462 (chainstate_service.cpp alone 248),
consensus 126, rpc 56, wallet 37, rest ~70.

The mechanism (`include/consensus/chainparams.h`, `src/consensus/chainparams_impl.cpp`):
`static ChainParams g_mainnet/g_testnet/g_regtest`, `static const ChainParams* g_active`,
`static bool g_paramsSelected`; `ParamsImpl()` lazily writes `g_regtest.genesis` with no lock;
`MutableParams()` is labeled test-only but used in production `src/daemon/main.cpp` (6 sites);
`src/crypto/params_stub.cpp` defines a **weak** `ParamsImpl()` with its own static `ChainParams` — which
implementation a target gets depends on link composition. `dinero_consensus_core` calls `Params()` without
linking `chainparams_impl.cpp`, i.e. it depends on the stub-or-link-order accident today.

Kernel-relevant ambient reads beyond chainparams (consensus sources): `DaemonContext::instance()`
(block_acceptor, 11), `daemon::GetConfig()` (consensus_write_batch, 1), `getenv` switches (6: forest
serialize/trace, reindex dump, consensus paranoid, abort-after-store), wall clock in validation
(`block_validation.cpp:1362`, 2-hour future rule; clock calls in 9 more files), `g_logger` (373 uses),
plus the chain-state globals family (`g_block_index` + recursive mutex, `g_candidates`, `g_orphan_pool`,
`g_orphan_manager`, block_lifecycle maps, sig/script caches) — the last family is **out of scope** (§3).

Structural facts that shape the stages:
- The heavy validation sources (`block_validation.cpp`, `reindexer.cpp`, `header_chain.cpp`,
  `block_download_scheduler.cpp`, `block_lifecycle.cpp`, `orphan_manager.cpp`,
  `parallel_block_validator.cpp`, `assume_utxo.cpp`) compile into **`dinero_core`**, not a consensus
  library. `pow_consensus_engine.cpp` is in `dinero_chainstate`; `orchard_block_staging.cpp` is in the
  Orchard backend target.
- `src/consensus_core/CMakeLists.txt` defines a second, dead `dinero_consensus_core` (never added).
- Prior docs: wallet-sep design §12 already names this follow-up ("full purity" inventory);
  `docs/ARCHITECTURE_MIGRATION_PLAN.md` is the older anti-globals plan (it produced the
  `DaemonContext::instance()` singleton — a lesson, not a template); CV1 (#877) measures header reach for
  the same two libraries and stays the header-boundary gate.

## 3. Non-goals (documented follow-ups, not silent omissions)

- **Chain-state globals ownership** (`g_block_index` family): needs an ownership-model redesign (recursive
  mutex, mutated from daemon/RPC/consensus layers). Separate program.
- **Logger injection** (373 consensus uses of `g_logger`): baselined and ratcheted by P1's report tier,
  not migrated here.
- **chainstate_service.cpp's 248 `Params()` sites** and other daemon/rpc/wallet call sites: callers of
  consensus APIs may keep calling `Params()` to obtain the argument they now pass. Shrinking caller-side
  use is explicitly out of scope.
- **Test coupling** (718 `Params()` / 372 `MutableParams` in tests): untouched except where a migrated
  API signature forces a mechanical update.
- **`MutableParams()` in production `main.cpp`**: surfaced to the owner; changing startup override
  behavior is not this program's call.
- No behavior change anywhere: every stage is signature/plumbing-only, qualified by the same build+test
  bar as sep2.

## 4. The P1 gate

`scripts/ci/separation/check_consensus_globals.py` + pytest suite + `.github/workflows/
separation-purity-gate.yml` (always-run, unfiltered, single job — same matrix-skip immunity as F1).

- **Source sets** read from the two consensus CMake source lists (parsed, not hardcoded):
  Tier K = `dinero_consensus_core` sources; Tier L = `dinero_consensus` sources.
- **Forbidden tokens** (comment-stripped scan): `Params(`, `GetActiveChain(`, `IsChainSelected(`,
  `MutableParams(`, `HrpForActiveNetworkRef`, `DaemonContext::instance`, `GetConfig(`, `getenv(`,
  `std::time(`, `time(nullptr)`, `ParamsImpl`.
- **Semantics:** ratchet against a committed baseline `consensus_globals_baseline.tsv`
  (file \t token \t count). A count above baseline ⇒ FAIL (`GATE P1 FAIL rc=1`). Below ⇒ PASS and the
  stage that lowered it also lowers the baseline in the same commit. Tier K's end state is an **empty**
  baseline, after which any hit fails outright. Tier L ratchets to chain-selection-free; its logger/cache
  rows remain as a visible report.
- **Self-tests:** positive control (a known violation in a fixture tree is found), neuter test (relaxing
  the deny list or inflating the baseline makes a planted regression pass ⇒ test fails), live-repo test
  asserting the current baseline matches a fresh scan exactly (stale baseline fails CI).

## 5. Stages

- **A — gate + doc (one PR, sep2-#892 pattern):** this document, P1 checker + tests + workflow with the
  baseline measured at the branch point. Green from day one (ratchet, no designed-red period — unlike F1,
  the migration here is too large to hold a red gate across it). Also deletes the dead
  `src/consensus_core/` duplicate target (file not referenced by any build; zero behavior risk).
- **B — kernel chainparams:** remove `Params()`/chain-selection reads from Tier K sources by threading
  `const ChainParams&` (or the narrow substruct a function actually needs) through signatures; end the
  kernel's `ParamsImpl` weak-stub dependency (`dinero_consensus_core` no longer references the symbol);
  baseline rows for Tier K chain-selection drop to zero. Touches `src/consensus/CMakeLists.txt` only
  (not in #893's file set).
- **C — kernel clock/env/config:** inject a clock (e.g. `int64_t now` or a `ValidationClock` capability)
  for time rules reachable from Tier K; replace `getenv` switches reachable from Tier K with explicit
  config arguments defaulted to current behavior. Tier K baseline now empty ⇒ P1 flips Tier K to
  zero-tolerance.
- **D — `dinero_consensus` chain-selection:** migrate the ~13 library `Params()` lines (block_reward,
  filter_commitment, script_validation, transaction_validator, tx_validation) the same way; callers in
  dinero_core/daemon pass `Params()` at the boundary.
- **E — membership (post-#893 only):** move `block_validation.cpp`, `header_chain.cpp` and feasible
  peers from `dinero_core` into `dinero_consensus`, so their ambient reads (39 + 3 + …) enter P1's
  ratchet; then migrate their chain-selection reads in follow-up slices. This is the only stage touching
  the root `CMakeLists.txt` — it waits for #893 to land and rebases on it.
- **F — enforcement:** add the P1 context to dinero-main branch protection (after observing it on a main
  push run), final report.

Each stage: ON+OFF local builds, full pytest suite for the gate scripts, neuter-proofs, and the sep2
verification discipline (a test that passes must be shown to run and to fail without the change).

## 6. Collision and hold ledger

- #893 (mempool): stages A–D avoid its 82-file set entirely (verified against the stage-0 overlap list:
  only root/daemon/nodecore/orchard/tests build files and mempool sources overlap — none are touched
  before stage E). Stage E rebases on #893's merged state.
- #877 (CV1): untouched; P1 is a separate checker and workflow. If the owner later merges #877, CV1 and
  P1 coexist (header boundary vs. symbol-use boundary).
- F-C3 security thread: no overlap by construction (no validity-affecting edits anywhere in sep3).
- Orchard activation operations (Codex's fleet lane): sep3 is repo-only; nothing deploys.
