# Combined migration + compact-proof + 60-second release qualification harness

Date: 2026-09-19. Prepared per explicit instruction to attack the outstanding
three-way release gate directly, rather than produce another design review or
duplicate migration internals.

## Ownership boundary

This harness (`tests/integration/test_combined_migration_release_qualification.sh`,
`tools/migrate_shielded_datadir.cpp`, and the CMake/CI wiring for both) is
owned by the DPI/qualification side of this work, not by the migration
implementation. It calls `dinero::storage::MigrateShieldedDatadirCopy`
(`include/storage/shielded_migration_cohort.h`) through its existing,
reviewed, unmodified public API. It does not implement, alter or assume
anything about:

- Migration internals (`src/storage/shielded_migration*.cpp`) — untouched.
- Lifecycle leases (`DatadirGuard`, the stopped-datadir companion wrapper) —
  untouched; the harness relies on their documented, verified behavior
  (`docs/design/shielded-migration-cohort.md`).
- SR-1's separate core recovery FFI — out of scope entirely; this harness
  exercises the *offline* migration path only, never a live-recovery path.

No production datadir, activation height or consensus parameter is touched.
Everything this harness runs operates on disposable regtest datadirs under a
`mktemp -d` workdir, torn down on success and preserved (with `[INFO] keeping
work dir...`) on any failure.

## Pinned starting point

Branch `claude/combined-migration-release-harness`, created from
`codex/shielded-migration-promotion-qualification` at commit
`8a80ca9ada00c9e591ecd1174b918207f7e9abd8` — verified via `git ls-remote` to
be the exact current remote tip of that branch, and via `gh api .../check-runs`
to have both Linux CI (`35448390849`) and Tests (`35448389984`) green on that
exact commit. Per `shielded-column-family-scope-2026-09-18.md`'s "Protected-
branch integration refresh" entry, this branch "combines #782–#785 with #786"
— the most complete, already-Linux-qualified integration point available at
the time this harness was built. It does not yet include #787 (PoW profile
marker preservation), which was still a separate, unmerged PR at this date;
none of this harness's four required exercises depend on that PR's scope.

`git worktree add -b claude/combined-migration-release-harness
/Users/haydarevich/src/dinero-v8-combined-migration-release-harness
8a80ca9ada00c9e591ecd1174b918207f7e9abd8` — a dedicated worktree, not a reuse
of any Codex-owned worktree/branch, per the explicit "no overlapping edits"
instruction.

## Why a new CLI driver was needed (concrete missing interface)

`MigrateShieldedDatadirCopy` has no daemon, NodeCore, RPC or operator CLI
entry point — both of its existing PR docs say so explicitly
(`shielded-state-migration-engine.md`, `shielded-migration-cohort.md`). The
only existing callers are two C++ unit-test executables
(`test_shielded_state_migration`, `test_shielded_migration_cohort`), both
built exclusively against synthetic, internally-fabricated fixtures
(`tests/storage/shielded_migration_fixture.h`) — explicitly not proof of
relocation against real daemon-produced state (their own docs: "Synthetic
READY fixtures do not prove relocation").

Since this harness's whole point is to migrate a *real*, regtest-daemon-
produced datadir (real shield/unshield transactions, real mining, real
Utreexo forests), it needed a way to call the real engine against real
directories from a shell script. `tools/migrate_shielded_datadir.cpp` is a
~120-line, deliberately thin CLI wrapper: parse two paths and an `--apply`
flag, call `MigrateShieldedDatadirCopy`, print the typed result, exit
nonzero on `!ok`. It adds no new logic to the engine itself and is not
proposed as a shipped operator tool — seeing the remaining gates
(rollback enforcement, binary/datadir pairing, disk-headroom policy) listed
in the engine's own docs makes clear why not.

**Reported as a concrete dependency, not bypassed:** if a future core-owned
CLI/RPC for this engine lands, this harness's `MIGRATE_TOOL` environment
variable should be repointed at it instead of this driver, and
`tools/migrate_shielded_datadir.cpp` retired.

## What the harness proves — mapped to the four required exercises

1. **Nonempty shielded state before migration; identical state and Utreexo
   proofs afterward.** Phase 1 shields and partially unshields real funds
   on a real regtest chain (mined past coinbase maturity first). Phase 3
   asserts byte-identical `daemon.shieldedstatehash`,
   `blockchain.getutreexoroots`, `wallet.listshielded` notes, wallet
   balance, chain tip, and a captured Utreexo membership proof
   (`blockchain.getutxoproofs_batch`/`verifyutxoproofs_batch`) for an
   already-spent coinbase output, before vs. after
   `MigrateShieldedDatadirCopy --apply`.
2. **Compact shield/unshield across the 60-second activation boundary.**
   Phase 4 mines the migrated candidate to one block before a caller-
   configurable `BOUNDARY_HEIGHT` (default 140), shields, mines across the
   boundary, then unshields — mirroring this same `CMakeLists.txt`'s own
   `CompactTimingLifecycle123/124/125` flanking-height convention, applied
   to a *migrated* store specifically (that combination — migration plus
   compact plus 60-second, on the same candidate — is exactly what
   `shielded-migration-eligibility.md` still lists as a remaining gate).
3. **Restart and reorg, transparent spend, duplicate-spend rejection.**
   Phase 5a stops/starts the migrated candidate and re-checks height/tip/
   state-hash identity. Phase 5b forces a real reorg via
   `invalidateblock`/`reconsiderblock` (the same technique
   `tests/integration/reorg_harness.sh`'s `force_reorg` already uses and
   documents in detail — applied inline here since this script needs the
   surrounding daemon to already be running with migration-specific extra
   CLI flags that `reorg_harness.sh`'s own `start_node` does not expose).
   Phase 5c spends a matured transparent coinbase output via
   `wallet.createrawtransaction`/`signrawtransaction`/`sendrawtransaction`,
   confirms it, then asserts a rebroadcast of the *exact same* raw
   transaction is rejected (`rpc_failure`, not silently re-accepted).
4. **ASERT targets, rewards, coinbase maturity.** Phase 6 compares
   `getblocktemplate`'s `bits` and `coinbasevalue` between the migrated
   candidate and a **fresh, unmigrated control chain** mined to the same
   height with identical consensus flags — a differential check that
   migration introduces no divergence, not a re-validation of the ASERT
   formula itself. **This distinction is deliberate and documented, not an
   evasion:** `tests/integration/test_sixty_second_activation.py`'s own
   header states "Regtest bypasses ASERT: this qualifies activation/state
   transitions, not cadence" — real difficulty-adjustment correctness is
   covered elsewhere (e.g. `DAAGoldenVectors`), out of scope for a
   migration-consistency harness. Coinbase maturity is checked via
   `wallet.listunspent`'s exact `[minconf, maxconf]` semantics, unaffected
   by migration.

## Negative controls

Run standalone with `bash test_combined_migration_release_qualification.sh
--self-test` (also invoked unconditionally at the end of a full run):

- A byte-flipped block file in the candidate's companion inventory must be
  refused by the migration tool (`ok=false`) — proves the byte-identical
  companion check in `shielded-migration-cohort.md` is actually load-bearing
  through this harness's own call path, not silently skipped.
- A candidate directory that was never copied from the original (freshly
  empty) must be refused — proves the harness cannot accidentally "succeed"
  by migrating into a directory with no real companion data at all.
- Rebroadcasting an already-confirmed transaction's exact raw hex must be
  rejected by the daemon (exercise 3's duplicate-spend check, listed here
  too since it is a negative control on the daemon's own mempool/UTXO
  logic, not the migration tool).

## Pinning migration evidence

Each run writes `${WORK}/evidence/`: `pre-shieldedstatehash.txt`,
`pre-utreexoroots.json`, `pre-utreexoproof.json`, `migrate-result.txt`
(the full typed `ShieldedMigrationResult` line, including `source_digest`),
and `migrate-stderr.log` (the engine's phase checkpoints). On CI, these are
uploaded as the `migration-qualification-<sha>` artifact only on failure,
matching this repo's existing `checkpoint-retention-<sha>` convention
(`.github/workflows/tests.yml`).

Binary hashes: this harness deliberately does not pin a specific `dinerod`
binary hash of its own — it always builds and runs against whatever
`dinerod`/`migrate_shielded_datadir` the *same* CI/local build produces from
the pinned source commit above, so its evidence is always traceable to that
commit via the workflow run's own commit SHA, not a separately-recorded hash
that could drift from what was actually built.

## CI wiring

Registered as CTest `CombinedMigrationReleaseQualification`
(`tests/integration/CMakeLists.txt`), guarded by the same
`if(UNIX AND NOT IOS)` condition as `migrate_shielded_datadir` itself (a
`TARGET_FILE` generator expression on a nonexistent target is a configure-
time error, not a graceful skip). Labeled `integration;...;mandatory`,
`TIMEOUT 2400`, `RUN_SERIAL TRUE` — the same label pattern this file's other
daemon-spawning tests use, which the project's own main ctest sweep
excludes via `--label-exclude 'integration|...'`
(`.github/workflows/tests.yml`). Given its own dedicated CI step instead,
mirroring the existing `CheckpointRetentionDaemon` step exactly: a single
`ctest -R '^CombinedMigrationReleaseQualification$'` invocation plus
failure-evidence collection/upload, inserted right after that step in
`.github/workflows/tests.yml`.

**Flagged, not silently absorbed:** the job's overall `timeout-minutes: 120`
was not changed. This harness's own CTest `TIMEOUT 2400` (40 minutes) is a
substantial addition to that budget alongside everything else already
scheduled in the same job; if the job starts timing out in practice, the fix
is to either raise `timeout-minutes` or move this step to run in parallel
with (not after) the existing serial e2e lane, both of which are shared-
CI-infrastructure decisions outside this harness's own ownership boundary.

## A concrete, unresolved local-build dependency (reported, not bypassed)

Full local execution of the new CTest was **not achieved on this development
machine** (macOS, Apple Silicon). Root-caused, not just observed:

- `CMakeLists.txt`'s `if(APPLE)` branch (lines ~347-379) unconditionally
  wraps the vendored `bulletproofs_ffi` Rust crate's `cargo build --release`
  with `RUSTFLAGS`/`CFLAGS`/`LDFLAGS` all appending
  `-mmacosx-version-min=${CMAKE_OSX_DEPLOYMENT_TARGET}`.
- On this machine's current Xcode/rustc (1.91.1) combination, that wrapped
  invocation intermittently — not deterministically — fails to compile
  proc-macro dependencies (`zeroize_derive`, `thiserror-impl`, `serde_derive`)
  with `error[E0463]: can't find crate for 'zeroize_derive'`, traced to a
  malformed Mach-O ("mis-aligned LINKEDIT string pool") in the freshly-built
  proc-macro dylib that rustc's own loader then rejects.
- Confirmed non-deterministic, not a fixed environmental toggle: an
  *unwrapped* `cargo build --release` in the same directory failed once and
  succeeded twice across three consecutive attempts; the *wrapped* (CMake)
  invocation failed 3/3 times attempted. Ruled out as the cause: stale
  build-cache corruption (failed identically after `rm -rf target`),
  `CARGO_BUILD_JOBS` parallelism (failed identically pinned to 1), and
  `RUSTFLAGS` specifically (failed identically with `RUSTFLAGS` forcibly
  unset via a `RUSTC_WRAPPER`/`CARGO_EXECUTABLE` substitution, while
  `CFLAGS`/`LDFLAGS`/`MACOSX_DEPLOYMENT_TARGET` remained set).
- **This does not block CI.** `.github/workflows/tests.yml` runs
  `ubuntu-latest` exclusively; `if(APPLE)` never evaluates true there, so
  this specific interaction cannot occur on the runners that actually gate
  merges. It is reported here as a genuine local-development-environment
  gap on macOS, not worked around by skipping, mocking, or hand-waving the
  daemon dependency this harness genuinely needs.

**What WAS verified locally, and constitutes real evidence this harness is
sound**, independent of that blocker:

- `tools/migrate_shielded_datadir.cpp` compiles cleanly against the real
  project headers (a real, nonzero-size `.o` produced by the project's own
  build) and links/runs correctly (`./migrate_shielded_datadir` with no args
  prints the documented usage and exits 2; the full argument-validation path
  was exercised).
- The existing, unmodified `test_shielded_migration_cohort` CTest — which
  links the exact same `dinero_chainstate`/`dinero_shielded`/`dinero_crypto`
  libraries this harness's CLI driver depends on, and does **not** need
  `bulletproofs_ffi` — built and ran successfully in this same worktree:
  **111/111 cases passed**, confirming the migration engine this harness
  calls is fully functional in this exact checkout, independent of the
  unrelated `dinerod`-only Rust/wallet dependency chain.
- `bash -n` confirms the new integration script's syntax is valid; `cmake`
  reconfigure and `ctest -N` / `ctest --show-only=json-v1` confirm the new
  CTest registers correctly with the intended `ENVIRONMENT`, `LABELS`,
  `TIMEOUT` and `RUN_SERIAL` properties; the modified `tests.yml` parses as
  valid YAML.

Full end-to-end daemon-spawning execution of
`CombinedMigrationReleaseQualification` itself has **not** been observed to
pass (or fail) yet on any machine, pending either a fix to the local macOS
Rust build interaction above or the first real CI run of this branch.
Reporting this precisely rather than claiming a pass this harness has not
actually produced.
