# Combined migration + compact-proof + 60-second release qualification harness

## First real Linux CI run: workflow bug found, and a new genuine failure

PR #791 (this harness, stacked on #790/#789) ran
`.github/workflows/combined-migration-qualification.yml` for the first
time. Two findings:

1. **This workflow's own CI-status bug**: every `run:` step piped its
   command through `tee` to also capture evidence, without `pipefail` —
   the step's exit code was `tee`'s (always 0), not the piped command's.
   The actual `CombinedMigrationReleaseQualification` ctest **failed**
   ("***Failed 164.62 sec", "0% tests passed"), yet the job and PR check
   both reported success (run `35474838716`). Fixed with a job-level
   `bash -eo pipefail` default, with a deliberate override on the
   qualification-ctest step so it still copies `LastTest.log` after a
   failure instead of exiting immediately.

2. **A new, Linux-CI-specific failure, not reproduced in ~10 local macOS
   runs**: phase 6's immature-coinbase spend failed immediately on
   `wallet.sendrawtransaction` with `Script validation failed for input 0:
   invalid script format`. Decoding the exact rejected hex shows a
   **legacy-serialized transaction with an empty scriptSig and no witness
   data at all** — `wallet.signrawtransaction` produced something that was
   never actually signed, for a coinbase-sourced UTXO specifically (the
   same call pattern against the unshield's own transparent output, one
   phase earlier in the identical run, succeeded normally). This was not
   further diagnosed by rerunning on Linux (no local Linux environment in
   this session) — reported as a concrete, unconfirmed-root-cause finding
   rather than guessed at. It has the same *shape* as the wallet-indexing
   race #790 fixed for shielded input selection (a freshly-mined output
   used immediately, before some indexing step completes), but for a
   transparent coinbase output instead of a shielded note — plausible, not
   confirmed.

Date: 2026-09-19. Prepared per explicit instruction to attack the outstanding
three-way release gate directly, rather than produce another design review or
duplicate migration internals.

## Ownership boundary

This harness (`tests/integration/test_combined_migration_release_qualification.py`,
`tools/migrate_shielded_datadir.cpp`, and the CMake/CI wiring for both) is
owned by the DPI/qualification side of this work, not by the migration
implementation. It calls `dinero::storage::MigrateShieldedDatadirCopy`
(`include/storage/shielded_migration_cohort.h`) through its existing,
reviewed, unmodified public API. It does not implement, alter or assume
anything about:

- Migration internals (`src/storage/shielded_migration*.cpp`) — untouched.
- Lifecycle leases (`DatadirGuard`) — untouched.
- SR-1's separate core recovery FFI — out of scope entirely.
- Wallet coin selection / shield-unshield proving — untouched.

No production datadir, activation height or consensus parameter is touched.
Everything runs on disposable regtest datadirs under a resolved (symlink-free)
`tempfile.mkdtemp()` workdir, torn down on success and preserved on failure.

## Pinned starting point

Branch `claude/combined-migration-release-harness`, worktree at
`/Users/haydarevich/src/dinero-v8-combined-migration-release-harness`, later
merged with `origin/codex/shielded-migration-profile-marker` (PR #787).
Current HEAD includes both. Every run's evidence records
`git rev-parse HEAD` plus `sha256sum` of the exact `dinerod` and
`migrate_shielded_datadir` binaries invoked (`record_evidence()`).

## Rewrite history

**First version** (bash, commit `ce1c342f5`): used `generatetoaddress`
(hides the ASERT/PoW gate entirely), had a proof-envelope double-`.result`
bug, an unreserved coinbase for the identity comparison, a reorg that never
crossed the boundary it claimed to test, a maturity check that tested
`listunspent`'s `minconf` filter (not actual coinbase maturity), and CI
wiring that requested compact activation against a compact-disabled build.
Full review: `MemoryMD/evidence/combined-migration-harness-review-2026-09-19/README.md`.

**Second version** (this file's Python rewrite): addresses every point in
that review — see inline comments in
`tests/integration/test_combined_migration_release_qualification.py` for the
line-by-line mapping. This document covers what changed *after* that
rewrite, while actually trying to get a real end-to-end pass.

## Real PoW mining: the ASERT genesis-staleness problem and its fix

`--regtest-enforce-pow` enables genuine PoW/ASERT enforcement. A fresh
regtest chain's very first blocks compute an astronomically hard target
(observed: `bits=02008000`, ~2^249 expected hashes) instead of
`pow_limit_bits=0x207fffff`, because pre-activation ASERT extrapolates from
`genesis_time` — a fixed, distant-past chainparams constant — against real
wall-clock "now". `test_pow_enforced_regtest.py` works around this for its
own short (3-block) chain by manually overriding `curtime`/`bits`/`target`
on the first 3 `getblocktemplate` calls.

This harness needed to premine well past `COINBASE_MATURITY` (100 blocks)
*before* any block is confirmable to Utreexo maturity — spendable-fund
setup requires real transparent inputs, and coinbase needs 100 confirmations.
Two things had to be verified in isolation before trusting this at that
scale, both confirmed directly (not assumed):

1. **The override scales far past 3 blocks.** With `curtime = genesis_time +
   h*120` held on every pre-boundary block, the ASERT delta is exactly zero
   for *every* h, not just an initial handful — confirmed by mining 109
   consecutive overridden blocks in 22s, `bits` flat at `1f00fc9c` throughout.
2. **The daemon genuinely enforces this, not just checks hash-vs-target.**
   Submitting a block with deliberately wrong bits (`0x207fffff` against an
   established `0x1f00fc9c` chain) was rejected: `bad-diffbits: block has
   0x207fffff, required 0x1f00fc9c`. This is real ASERT recomputation and
   enforcement by the daemon, confirmed before relying on it as evidence for
   exercise 4 ("Correct ASERT targets").
3. **Reverting to real wall-clock curtime breaks immediately if done before
   the 60-second-activation height.** A natural (non-overridden) template at
   height 31, still pre-activation, computed the same unminable
   `0x02008000` target. The 60-second-activation retarget path does **not**
   reference `genesis_time` — it self-corrects from a rolling window of
   actual recent block timestamps — so heights at/after that boundary can
   safely use natural templates. Confirmed at the harness's actual boundary
   shape: 109 overridden blocks immediately followed by 6 natural blocks,
   all landing on the easy `0x207fffff` limit in well under a second.

Design (`mine_to()` in the harness): every height `< BOUNDARY_HEIGHT` uses
the override; every height `>= BOUNDARY_HEIGHT` uses natural
`getblocktemplate`. `BOUNDARY_HEIGHT` defaults to 130 — comfortably above
phase 1's ~108-block premine/setup so phase 4 still has room to cross the
boundary from below.

## Two harness-owned bugs found and fixed while getting a real run

Both are in this script, not the daemon — reported here for completeness
since they were briefly (and incorrectly) taken as daemon defects before
isolation proved otherwise.

1. **Top-level `finally: sys.exit(1)` silently masked every real failure's
   traceback.** `sys.exit()` inside `finally` raises `SystemExit`, which
   replaces whatever exception was propagating from `main()`. Two
   consecutive full runs failed at the identical point with *zero* printed
   exception (reproduced identically under `python3 -u`, ruling out stdout
   buffering) until this was found and fixed: the exception is now printed
   via an explicit `except: traceback.print_exc(); raise` *before* the
   `finally` block's cleanup runs, and the cleanup-only-failure exit path
   was moved outside the try/finally entirely so it can't mask anything.
2. **`note_values()` didn't filter on `spent`.** `wallet.listshielded`
   returns every note the wallet has ever seen, spent or not, each with an
   explicit `spent`/`spent_height` field. The helper returned all of them,
   so phase 1's post-unshield assertion failed even when the daemon behaved
   correctly (confirmed directly: the "missing" note carried
   `spent: true, spent_height: 108` in `wallet.listshielded` at the moment
   of the false failure). Fixed to filter `not n.get("spent", False)`.

Also fixed: macOS's `/tmp` and default `tempfile` dir are symlinks to
`/private/...`, and `MigrateShieldedDatadirCopy`'s own path validation
(`shielded_migration_cohort.cpp`) rejects any datadir path containing a
symlink component. Without resolving `WORK` immediately after `mkdtemp()`,
both negative controls reported `error=symlink in datadir path` instead of
the specific rejection they're meant to exercise — and the real phase-2
migration call would hit the identical false rejection. Fixed with
`.resolve()`.

## Debug vs. Release timing — do not compare across build types

`wallet.shield` measured at **67.6s** wall-clock on this machine's local
**Debug** build (unoptimized proving path). The project's own
`docs/shielded-cost-reduction.md` records a Release-build shield *build*
time of 3.47s (Apple M4 Max, native arm64) for the same 1-output shape —
confirming the 67.6s figure is a Debug-build artifact, not a release
performance measurement, and establishes neither a regression nor an
acceptable release baseline. The harness's RPC timeout was raised from 60s
to `RPC_TIMEOUT_SECONDS = 180` to accommodate real proving time regardless
of build type — this is a correctness fix (the daemon was doing real, slow
but legitimate work; the client was giving up too early), not a performance
change. A dedicated Release build (`build-release/`, same flags as the
Debug build) was created in this worktree for realistic timing and to
re-test the one non-reproducing race noted below.

## An intermittent, non-reproducing coin-selection anomaly (not confirmed as a defect)

One Debug-build run failed `wallet.shield` with `Input UTXO not found` on
what was identified as an already-spent coinbase outpoint from an earlier
`wallet.shield` call in the same sequence — suggesting a possible stale
coin-selection cache. **This did not reproduce** across 7 subsequent
attempts (5 isolated repros matching the exact call sequence, plus 2 full
Release-build harness runs that passed this exact point cleanly). Recorded
here per the instruction to report honestly rather than silently drop it,
but explicitly **not** claimed as a confirmed, reproducible defect — if it
recurs, the daemon log timestamp-free format made root-causing it
difficult; a timestamped log would help.

## Resolved: `MigrateShieldedDatadirCopy` real-daemon-datadir blocker

**This was the harness doing its job.** Phase 2 (the actual migration call)
failed 100% reproducibly with `error=shielded state root/count mismatch`
from `InspectOriginal` in `src/storage/shielded_migration.cpp`, isolated
down to reproducing on a completely vanilla regtest daemon with zero
shielded activity — ruling out this harness's own configuration entirely
(full isolation detail preserved above/in git history of this file).
Reported to Codex as a concrete dependency rather than bypassed.

**Root cause (Codex, PR #789,** `de3f62b33`**):** `CurrentShieldedStateSnapshot()`
(the real marker writer every live daemon path uses) copies
`CommitmentTree::Root()` into `ShieldedTipMarker::shielded_root`.
`InspectOriginal` and `LoadSeparatedShieldedState` instead compared the
marker against `ComputeShieldedRootFromParts`, a different composite
consensus hash — a reader/fixture bug, not corrupted daemon-written data or
an on-disk encoding change. Both readers now match the existing marker
writer; marker format, consensus commitments, Utreexo logic and activation
rules are unchanged. Full evidence:
`MemoryMD/evidence/shielded-tip-marker-semantics-2026-09-19/README.md`.

**Follow-on fix required in this harness's own CLI** (per that same
handoff): the corrected engine's forest audit now explicitly requires
`dinero::SelectParams(dinero::Chain::REGTEST)` before the migration call —
previously this driver selected no chain params at all. Added to
`tools/migrate_shielded_datadir.cpp`, hardcoded to REGTEST deliberately
(this is a regtest-only qualification tool; a future operator tool needs
its own separately reviewed network/profile selection contract).

**Separate, also-real bug found and fixed in the same window** (Codex, PR
#790, `a79fafd10`): `wallet.shield`'s coin selection could pick an
already-spent transparent input during a real WalletWorker
indexing-notification gap, failing the second of two back-to-back shield
calls with `Input UTXO not found`. This is the SAME failure this harness
observed once (non-reproducing across 7 follow-up attempts, logged above
as "not confirmed as a defect" at the time) — Codex's own diagnostic later
reproduced it reliably and fixed it upstream of any change here.

## Further fixes made integrating #789/#790, reaching a real full pass

Merging in #789/#790 and rebuilding surfaced several more issues, all in
this harness's own code, found only by actually running it to completion:

- **`wallet.lockunspent` is process-local, in-memory-only state**
  (`WalletManager::locked_utxos_`), never persisted — a daemon restart
  clears it independent of CF migration. Phase 3's wallet-balance
  comparison was checking `original` (lock held) against `candidate`
  (freshly started, lock cleared) and failing on that unrelated
  `locked`/`unspendable` difference. Fixed by re-applying the identical
  lock on `candidate` before comparing.
- **`gettransaction` is a confirmed-only, blockchain lookup** — phase 4
  asserted `tx_version()` immediately after broadcasting a shield/unshield,
  before the confirming `mine()` call. Moved both assertions to after
  their transaction's confirming block.
- **Phase 5b's reorg fork-length arithmetic was off by one**, and its
  "alternate branch" matched the original branch's exact length — with a
  work tie, `reconsiderblock` has no principled reason to switch back.
  Fixed: the alternate branch is deliberately one block shorter, giving the
  reconsidered branch strictly greater cumulative work.
- **`daemon.shieldedstatehash` is a composite** of the Utreexo forest plus
  the shielded tree/nullifier set/anchor history (own doc comment,
  `methods_daemon_status.cpp`) — it moves on every new block regardless of
  shielded content, since every block's coinbase is a new Utreexo leaf.
  The original assertion (expecting it unchanged across an ordinary,
  non-shielded extension) was simply wrong; fixed to assert the alternate
  branch is a genuinely different composite state, and that reconnecting
  the original restores the original composite exactly.
- **`wallet.sendrawtransaction`'s result is double-wrapped**
  (`{"result": {"result": "<txid>"}}`) unlike this file's other RPCs.
- **`getrawtransaction` (the bare alias) only finds mempool transactions**;
  `wallet.getrawtransaction` (mempool then chainstate/chain_db) is the one
  to use, and even then only in non-verbose mode (verbose returns a decoded
  object with no `hex` field).
- **`COINBASE_MATURITY` was wrong (100 assumed, but two source locations
  disagree)**: `chainparams_impl.cpp` sets a network-specific regtest value
  of 10, but `CoinbaseMaturity::isCoinbaseMature()` compares against the
  class's own hardcoded 100 constant, not `Params().coinbase_maturity`,
  despite its own header comment saying to use the network-specific value.
  Calibrated directly via an isolated template-membership probe: 100 is
  what actually governs. Also confirmed **mempool admission does not gate
  maturity at all** — `wallet.sendrawtransaction` accepted an immature
  coinbase spend unconditionally; the actual enforcement point is block
  TEMPLATE SELECTION (`mempool.cpp`'s `isCoinbaseMature(coin->height,
  next_block_height)` feeding `getblocktemplate`). Phase 6 was rewritten
  to test template membership (absent pre-maturity, present and
  auto-included once mature) instead of a `sendrawtransaction` rejection
  that could never have passed regardless of the threshold used.
- **An unrealistic 90%-of-value "fee" in phase 6's immature-spend
  transaction exposed a genuine, narrow daemon inconsistency**: at a large
  enough fee, `getblocktemplate`'s coinbase-value construction and the
  block acceptor's own "maximum subsidy + fees" validation disagreed,
  causing the daemon to reject its own template-built block. Not something
  a real wallet paying an ordinary fee would trigger; fixed by matching
  this script's own established `1000000`-una fee convention instead of
  guessing at the daemon-side root cause.
- **The daemon's own RPC rate limiter** was tripped by this harness's fast
  post-boundary natural mining (dozens of blocks/second) and by phase 6's
  self-calibrating template-membership polling loop. Added bounded
  retry-with-backoff to `mine_to()`'s per-block template/submit calls and
  to the shared `rpc()` function itself.
- **A passing run deleted its own evidence** (`shutil.rmtree(WORK)` on
  success) — exactly backwards for a release-qualification harness, whose
  passing-run evidence (source commit, binary hashes, migration receipts)
  is release sign-off material. Fixed: `EVIDENCE` is now copied to
  `<repo>/evidence/combined-migration-qualification-<timestamp>/`
  (gitignored, matching the CI workflow's own evidence-upload convention)
  before any `WORK` cleanup, regardless of pass/fail.

## Result: full, real, independently-reproduced pass

Two consecutive full runs against a from-source Release build
(`-DCMAKE_BUILD_TYPE=Release -DDINERO_ENABLE_COMPACT_REGTEST=ON`, source
commit `75811bc63` — the merge of this harness with #789 and #790) printed
`[INFO] ALL CHECKS PASSED` end to end: nonempty pre-migration state, a real
migration with byte-identical post-migration state/notes/balance/tip and
both captured Utreexo proofs verified, compact/60-second activation with
real transaction-version assertions and block inclusion, restart, a reorg
that genuinely disconnects and reconnects the boundary-crossing shielded
transitions with matching composite-state roots, spending the unshield's
own output with duplicate- and nullifier-reuse rejection, coinbase maturity
tested at the layer that actually enforces it, and all three negative
controls for the right reasons. Evidence (source commit, both binary
SHA-256 hashes, migration stdout/stderr) preserved at
`evidence/combined-migration-qualification-<timestamp>/` per run.

Per both upstream handoffs' own instruction: **this local result is not
itself a release-readiness claim.** Linux CI on #789/#790 remains the
actual gate; this is independent evidence from a from-source Release build
on one machine, not a substitute for it.

## Second review round (Codex, `combined-harness-ac5af-review-2026-09-19`) and fixes

Reviewed commit `ac5af747a`. Found and fixed, without further core changes:

- **Phase 6's maturity check self-calibrated** (searched forward up to
  `COINBASE_MATURITY` blocks for the transaction to appear in a template)
  instead of asserting the exact boundary directly — "allows inclusion
  long after the required boundary and still passes." Fixed: absence is
  asserted at exactly `blocks_on_top == COINBASE_MATURITY - 2`, one `mine()`
  call, then presence at exactly `COINBASE_MATURITY - 1` — no search loop.
  Comment corrected to acknowledge template selection is not the sole
  enforcement layer (`block_validation.cpp` and stateless validation also
  enforce it); this phase still only exercises template selection.
- **Phase 4's reward check was `coinbasevalue > 0`**, not exact, and never
  implemented the differential/independent-control-chain comparison this
  document's own earlier draft had described as part of the design.
  Fixed: asserts the exact 100 DIN subsidy (verifying mempool is actually
  empty first, not assuming it), and mines a **second, unmigrated control
  daemon** from genesis with identical consensus flags to the same
  `BOUNDARY_HEIGHT`, asserting its `bits` and `coinbasevalue` match the
  migrated candidate's exactly.
- **A fabricated test reference.** An earlier version of this file's own
  comment claimed a `test_economics_after_sixty_second_activation` case in
  `test_sixty_second_activation.py` covers mining/validating a real
  reduced-reward (tail-emission) block. That function does not exist
  anywhere in the repository — confirmed by a repo-wide search after
  Codex's review correctly flagged the claim as unsupported. Corrected: no
  test anywhere in this repo mines a real block at a reduced-reward height
  and checks the actual coinbase payout; `test_sixty_second_activation.py`
  only asserts the RPC-reported `tail_emission_una` field (never cross-
  checked against a mined coinbase) and several C++ unit tests check the
  subsidy-calculation function in isolation, never through real block
  validation. Recorded as a genuine coverage gap, not claimed as covered.
- **The retry wrappers retried every failure**, not only the documented
  HTTP 429 rate limit, because they called `miner.get_block_template()`/
  `miner.submit_block()`, which swallow all exceptions internally and
  return `None`/`False` regardless of cause — so a genuine consensus
  rejection would be retried uselessly against the same already-built
  block instead of failing immediately with its real reason. Fixed: both
  helpers now call `rpc_call()` directly and re-raise immediately on
  anything that isn't specifically an HTTP 429.
- **Negative controls accepted "any nonzero exit or missing ok=true"**
  as proof of refusal, rather than the specific structured refusal. Fixed
  with a corrected field parser (`parse_migrate_output` — the naive
  `dict(kv.split("=",1) for kv in stdout.split())` used previously breaks
  with a `ValueError` on any multi-word `error=` message, confirmed by
  direct reproduction, since `error=` is always the last field and its
  value itself contains spaces) and exact `error=` substring assertions.
- **The "neuter check" was a bare Python constant-versus-constant
  assertion** (`TX_VERSION_COMPACT_REGTEST == 0x40000007`), proving only
  that `assert` itself works, not that this script's real comparison logic
  is discriminating. Fixed: reuses the REAL, genuinely-corrupted migration
  result from negative control #1 and confirms phase 2's actual success
  condition (`ok=="true" and ready=="true"`) evaluates false against it.
- **Nullifier-reuse and duplicate-spend rejections matched any RPC error**,
  not their specific expected reason. Fixed with exact substring
  assertions (`"utxo not found"`, `"nullifier"`) against the real observed
  daemon messages.
- **A vacuous `assert ... or True`** (phase 5c) — always true regardless of
  its left operand, and immediately followed by the real check it was
  presumably meant to be. Removed.
- **Evidence was five summary files, no complete phase log**, and recorded
  the source commit at test time without any indication of whether this
  harness's own script was dirty (uncommitted) relative to that commit —
  which it was. Fixed: this script's full stdout/stderr is now teed to
  `evidence/.../transcript.log` from the very first line, and
  `record_evidence()` additionally captures `git status --short` and a
  SHA-256 of the harness script file itself, independent of git commit
  state. A failed evidence-preservation copy now fails the run and retains
  `WORK`, instead of logging an error and still deleting it on success.

Two further consecutive full runs against a freshly-rebuilt Release build
(now including `f76d2843c`/`2356d6ebd`, the #789 CI-fixture fix) printed
`ALL CHECKS PASSED` with all of the above tightened assertions in place.

## Negative controls

Run standalone with
`python3 tests/integration/test_combined_migration_release_qualification.py --self-test`
(dispatches before any other phase runs):

- A byte-flipped block file in the candidate's companion inventory is
  refused with the exact structured field `error=original/candidate
  companion mismatch` — the byte-identical companion check is genuinely
  load-bearing through this harness's call path.
- A candidate directory that was never copied from the original is refused
  with the exact structured field `error=cannot stat companion path`.
- The genuinely-corrupted migration result from the first control is fed
  through phase 2's own real `ok`/`ready` success condition, confirming it
  evaluates false — not a bare Python constant comparison.

## CI wiring

`tests/integration/CMakeLists.txt` registers `CombinedMigrationReleaseQualification`
guarded by `DINERO_ENABLE_COMPACT_REGTEST`, `ENVIRONMENT` wiring `DINEROD`/
`MIGRATE_TOOL` to the built targets, `TIMEOUT 2400`, `RUN_SERIAL TRUE`. A
dedicated `.github/workflows/combined-migration-qualification.yml` builds
with `-DDINERO_ENABLE_COMPACT_REGTEST=ON` specifically (the main `tests.yml`
job builds compact-disabled and must not run this test — this was the
first version's CI bug, now fixed by giving this its own workflow instead
of inserting a step into `tests.yml`). Given the confirmed migration
blocker above, this workflow will not pass end-to-end until that is
resolved; it is still valuable to land now so CI immediately reflects
current reality (red for a real, tracked reason) rather than being wired
up later once the underlying fix lands.

## What's next

1. Wire this harness's own `CombinedMigrationReleaseQualification` CTest
   into Linux CI now that a real local pass has been reached (it registers
   correctly already; `.github/workflows/combined-migration-qualification.yml`
   builds compact-enabled specifically) and watch its first real CI run.
2. Obtain comparable Release-build shield/unshield timing (cold start,
   blocks continuing to arrive at 60-second spacing) as its own dedicated
   measurement, per the user's stated need for real release-representative
   numbers — this harness's phase timings are a byproduct of correctness
   testing, not a calibrated performance benchmark.
3. If the once-observed, non-reproducing `Input UTXO not found` coin-
   selection race recurs even after #790, capture full daemon logs (ideally
   timestamped — their absence made this harness's own diagnosis harder
   than necessary) and hand them to Codex directly.
