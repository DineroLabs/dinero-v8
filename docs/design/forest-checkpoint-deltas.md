# Forest checkpoint deltas — design (campaign prep)

**Status:** design approved by owner 2026-07-16; campaign not yet started.
**Goal:** stop serializing the full Utreexo forest per connected block.
Expected: sync ×3–5 (phone-flash bound today), datadir ×~100 smaller,
large battery savings. Applies to full nodes, CSN/phone, and fleet alike.

## Current state (why it is like this)

Both connect paths persist the **entire forest** every block:

- `ChainstateService::ConnectTip` (~chainstate_service.cpp:11441) —
  `GetForest().serialize()` staged into the same unified RocksDB batch as
  the UTXO rows + tip marker. Deliberate June-2026 fix: a crash could
  previously separate the forest checkpoint from the ChainDB tip
  ("forest checkpoint does not match ChainDB tip" → safe mode).
- CSN validation worker (daemon_app.cpp ~4539) — `putUtreexoCheckpoint`
  per validated utxoblk, same full serialize.

Cost at mainnet size (~191k leaves): `nodes_` ≈ 380k+ optional 32-byte
entries → **~10–12 MB written per block**, i.e. ~6 MB/s sustained during
sync at 30 blk/min, plus RocksDB write amplification. Empirical evidence:
dtx-v382's 41 GB datadir; the phone syncs I/O-bound at ~30 blk/min.

## Proposal

1. **Full checkpoint every N blocks** (N configurable; start with 500).
   Same all-or-nothing serialization as today (#397 semantics).
2. **Per-block delta record otherwise**, in the SAME unified batch as the
   tip write: `{height, block_hash, adds: [leaf_hash...], dels:
   [position...]}` — typically a few hundred bytes. The `BlockUndo`
   `utreexo_delta` structures already capture this shape at connect time.
3. **Restart restore:** load latest full checkpoint ≤ tip, apply stored
   deltas (checkpoint_height, tip] in order, in memory. Pure accumulator
   ops; no block bodies needed (this is what distinguishes it from the
   old pruned-node replay hole — deltas are stored data).
4. **Disconnect/reorg:** the disconnect path restores from checkpoint +
   deltas to the fork point (same recovery choreography the CSN reorg
   path already uses with checkpoints).
5. **Retention:** keep the last K full checkpoints (K=2) + deltas since
   the oldest retained; prune older. Old per-block checkpoints from
   existing datadirs are read fine (any checkpoint ≤ tip works) and
   pruned lazily — no migration flag-day.

## Hard invariant (the one every June/July bug violated)

After a crash at ANY write boundary, the restorable forest state at the
persisted ChainDB tip must equal the canonical accumulator at that height:
`forest(tip) == checkpoint(K) + deltas(K+1..tip)`, all components living
in batch-consistent storage. Deltas ride the unified batch, so this holds
by construction — the campaign's tests must prove it, not assume it.

## Test plan (red first, per house rules)

1. **Equivalence (hermetic, gtest):** random add/remove sequences applied
   (a) continuously and (b) via checkpoint-every-N + delta replay must
   yield byte-identical `serialize()` output and identical
   `findLeafPosition`/`prove` behavior for every live leaf. Property-style
   loop over seeds; include delete-heavy and fork-boundary (canonical
   roots flag) sequences.
2. **Husk-proofing:** delta records get the #397 treatment — truncation
   at every byte offset must fail loudly, never partially apply
   (extend ForestSerializationIntegrity).
3. **Crash-point matrix:** simulated kill between batch commit and any
   auxiliary write; restart restore must satisfy the invariant. Reuse the
   #371 fault-injection env harness where possible.
4. **Live mainnet:** the proven restart-torture harness (fresh
   ios_utreexo bootstrap, mixed SIGTERM/SIGKILL cycles through spend
   blocks + both forks) on the delta build; then fleet canary.
5. **A/B benchmark:** instrumentation lands FIRST (see below) so before/
   after is measured: per-block connect ms, checkpoint bytes/block,
   blocks/min, datadir size after 10k blocks.

## Instrumentation (phase 0 — land before any behavior change)

- Per-block: `connect_ms` and `forest_checkpoint_bytes` counters,
  aggregated into a periodic sync-stats log line (once per 100 blocks,
  cheap) + exposed via getsyncstats/getsnapshotbootstrapstatus.
- This is also the baseline evidence for the PR.

## Sizing notes

- Delta size: adds ≈ 3–5 × 32B + dels ≈ 0–1 × 8B + envelope ≈ ~200–500B.
- Restart replay: ≤ N deltas × (few adds + occasional remove) — for
  N=500 well under a second on-device.
- N tradeoff: bigger N = less write volume, longer restart replay and
  more deltas retained. 500 ≈ 12MB per ~500 blocks ≈ 24 KB/block
  amortized (~500× reduction) with sub-second restarts.

## Explicit non-goals (this campaign)

- No accumulator algorithm changes (add/remove/prove untouched).
- No change to snapshot (.dat) format or AssumeUTXO flow.
- No change to the bridge's historical checkpoint serving (bridge reads
  whatever checkpoint exists at H-1; with every-N checkpoints the bridge
  restores nearest checkpoint + replays deltas — include a bridge-side
  test in the equivalence suite).

## Sequencing

0. Instrumentation + baseline capture (Mac repro + phone numbers).
1. Delta record format + writer behind a feature flag (default off).
2. Restore path (checkpoint + replay) + equivalence/husk/crash tests red→green.
3. Torture + A/B on Mac repro; flip default on.
4. PR → CI → fleet canary (one node) → fleet → xcframework → phone.
