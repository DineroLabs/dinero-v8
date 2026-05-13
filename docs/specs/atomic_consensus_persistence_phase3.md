# Atomic Consensus Persistence — Phase 3 Design

**Status:** design-first draft, 2026-04-28. Owner: project. Tracks
phase 3 of `shielded_reorg_invertibility_audit.md` per operator
greenlight: "design-first, not a sweeping rewrite. First land a
small document or interface sketch that defines the transaction
boundary and lists the exact containers in scope. Then implement
the narrowest atomic staging layer behind the existing stateful
path, with the copied-mainnet reindex regression as the gate."

This document is the contract. No code changes; nothing else in
the audit moves until this is reviewed and a tighter version
agreed on.

---

## 1. The rule we are encoding

> **A consensus container may not persist independently of the
> block transaction it belongs to.**

That is the law. Every clause below is a corollary. If a future
change disagrees with that sentence, the change is wrong, not the
sentence.

### 1.1 Atomic unit

The atomic unit is **exactly one block connect or disconnect**.
`ChainstateService::ConnectTip(block N)` and
`ChainstateService::DisconnectTip(block N)` are each one
transaction. Multi-block reorgs are a sequence of single-block
transactions, never a single fused transaction. (One block at
a time keeps the failure radius bounded and the recovery rule
simple.)

### 1.2 Commit rule

> No canonical pointer advances until every staged container
> write for that block has succeeded.

Specifically, the *tip pointer*, the *height-index entry*, and
the *shielded tip marker* are all canonical pointers. None of
them moves until every other container's staged write — UTXO
delta, utreexo checkpoint, position-index delta, block-undo
record, block-index entry, shielded tree frontier, nullifier
inserts, anchor record — has been durably persisted. The
canonical pointers are the LAST writes in the batch, and they
are written together as part of the same atomic commit (§3 step
4 + 5).

A successful commit means: all staged container writes are on
disk, AND the canonical pointers point at the new tip.
Anything less is a failed commit and §1.3 applies.

### 1.3 Rollback rule

> A crash anywhere before commit must leave the old canonical
> state readable.

Concretely: if the daemon dies between `ConnectTip` start and
the commit step, the next clean read must return the pre-block-N
state. The live in-memory containers cannot be mutated until the
batch commits (`ConsensusWriteBatch` enforces this — staged ops
go to working copies, not live state). The on-disk write is one
atomic operation; before it, the canonical pointers still
address block N-1; after it, they address block N.

There is no "block N partially applied" state on disk and no
"block N partially applied" state in memory. The destructor of
`ConsensusWriteBatch` aborts the daemon if it runs without an
explicit Commit() or Abort() — see §6 Q4. That guarantees we
never accidentally leak a half-committed in-memory state into
shutdown.

### 1.4 Recovery rule

> On startup, the daemon must choose either the old complete
> state or the new complete state, never a mix.

The single durable commit pointer is the *journal row* in §3.4 —
keyed by the new tip's block hash, written inside the same
atomic batch as the canonical pointers. Its presence on disk is
the only thing that distinguishes "block N applied" from "block
N not applied" after a crash.

Startup procedure:

  1. Read the canonical tip pointer. Call its block hash `H`.
  2. Read `consensus_journal:<H>`. If present, verify its
     fingerprint matches the post-apply state of the in-memory
     containers loaded from their respective columns. If yes,
     state is "block H applied" — accept it.
  3. If the journal row is absent OR its fingerprint disagrees,
     the last commit was incomplete. Roll back the canonical
     pointer to `H`'s parent (which IS guaranteed to have a
     complete journal row because step 1's `H` was the previous
     tip on a successful commit) and proceed with that as the
     active tip.

This is **one branch**, not five. The current
`RecoverShieldedStateFromTipMarker` 5-branch logic exists
because today's persistence is per-container; once §1.2's commit
rule binds, four of those branches become unreachable and the
fifth collapses into step 3 above.

### 1.5 Two-mode separation

The stateful daemon path and the StatelessNode / mobile path are
**separate write modes with separate property tests**. They share
the law in §1 but their containers, their commit batches, and
their property tests are independent. Phase 3 is stateful-mode
only; the stateless path's `RewindToCheckpoint`/`ReplayBlock`
gets its own atomic-batch contract in a future phase, mirroring
this document but for that mode's narrower container set
(forest only).

This separation prevents a stateful change from accidentally
breaking iOS, and prevents a stateless change from accidentally
breaking the fleet. They are co-designed; they are not unified
into one batch.

### 1.6 What this prevents

This is the structural fix that prevents shielded (or any future
container — covenants, governance, vault) from re-introducing
the rings-class drift. It is **not** an optimization; it is the
contract. The 2026-04-28 LA failure happened because UTXO
position index, utreexo forest serialization, and anchor
history were all persisted independently of the block they
belonged to. §1 makes that class of failure structurally
impossible going forward. Existing on-disk drift is gap #10 and
is not retroactively healed by phase 3.

---

## 2. Containers in scope

> Every container below is bound by §1 ("a consensus container
> may not persist independently of the block transaction it
> belongs to"). The list is exhaustive — adding a 12th container
> requires updating this section AND §3 staging interface AND
> §7 crash injections.

The transaction boundary covers exactly these containers, and
only these:

| # | Container | Live owner | Persistence today | Role under §1 |
|---|-----------|-----------|-------------------|---------------|
| 1 | UTXO map | `ConsensusUTXOSet::utxos_` | ChainDB UTXO column family, per-write | content |
| 2 | Utreexo forest | `ConsensusUTXOSet::forest_` | ChainDB utreexo checkpoint column, per-block | content |
| 3 | UTXO position index | `ChainstateService::utxo_position_index_` | not persisted (rebuilt at startup) | content (in-memory only, but must move with the block) |
| 4 | Block undo record | constructed in `BlockValidator` | ChainDB block-undo column, per-block | content |
| 5 | Tip pointer | ChainDB metadata | ChainDB tip column | **canonical pointer** (§1.2: last to move) |
| 6 | Block index entries | `block_index_` (in-memory graph) | ChainDB header-metadata column | content |
| 7 | Height index | ChainDB | ChainDB height-index column | **canonical pointer** (§1.2: last to move) |
| 8 | Shielded commitment tree | `shielded_tree_` frontier | ChainDB shielded-frontier column + per-block undo frontier | content |
| 9 | Shielded nullifier set | `shielded_nullifiers_` | sqlite-backed (separate database) | content (sqlite WAL discipline in §3 step 6) |
| 10 | Shielded anchor history | `shielded_anchor_history_` | single-file `Save`/`Load` at shutdown / startup | content |
| 11 | Shielded tip marker | ChainDB shielded-tip-marker column | ChainDB | **canonical pointer** (§1.2: last to move) |

The three rows tagged "canonical pointer" are the ones the
commit rule §1.2 says do not advance until every "content" row
has been durably persisted. Phase 3a's hidden flag enforces this
ordering for rows 1–7; phase 3b extends to rows 8–11.

Containers explicitly **not in scope** for phase 3:

- Mempool — not consensus state, evicted on reorg from a
  separate path.
- Wallet UTXO index — wallet-side, not consensus.
- StatelessNode forest path — separate state machine; phase 5
  per the audit handles "single Disconnect per mode" separately.
- P2P-layer block-relay state — not consensus.

Out-of-scope containers are documented here so the next session
does not silently expand phase 3.

---

## 3. The transaction boundary

Define one type, owned by `ChainstateService`:

```cpp
// Brief sketch — full header lands in the staging-layer commit.
class ConsensusWriteBatch {
public:
    explicit ConsensusWriteBatch(ChainstateService& chainstate);
    ~ConsensusWriteBatch();  // aborts if not committed

    ConsensusWriteBatch(const ConsensusWriteBatch&) = delete;
    ConsensusWriteBatch& operator=(const ConsensusWriteBatch&) = delete;

    // Stage mutations for each container. Each call is in-memory
    // only; nothing reaches disk or the live container yet.
    void StageUTXOAddition(const OutPoint&, const UTXOEntry&);
    void StageUTXOSpend(const OutPoint&);
    void StageForestDelta(const consensus::UtreexoDelta&);
    void StagePositionIndexDelta(const PositionIndexDelta&);
    void StageBlockUndo(const BlockUndo&);
    void StageTipMove(const uint256& new_tip, uint32_t height,
                      const arith_uint256& chainwork);
    void StageBlockIndexEntry(const BlockHeader&, uint32_t height,
                              const arith_uint256& chainwork);
    void StageHeightIndex(uint32_t height, const uint256& hash);
    void StageShieldedTreeAppends(const std::vector<Hash>& commitments);
    void StageShieldedFrontierBefore(std::vector<uint8_t>);
    void StageNullifierInsertions(const std::vector<NullifierInsertion>&);
    void StageAnchorRecord(uint32_t height, const Hash& root);
    void StageShieldedTipMarker(const ChainDB::ShieldedTipMarker&);

    // Commit or abort. Exactly one MUST be called before the
    // batch is destroyed. Commit is atomic across every staged
    // mutation; if any single backend write fails, the live
    // containers are left untouched and the destructor aborts
    // cleanly (no partial commit).
    Status Commit();
    void Abort() noexcept;
};
```

Construction acquires a single `ChainWriteToken`. Commit serializes
the staged mutations into one `rocksdb::WriteBatch` (after phase
3b: every consensus container, including nullifiers, lives there)
plus the anchor history serialization, and writes them in this
order:

### 3.1 Phase 3a commit ordering (nullifiers still on legacy sqlite path)

  1. Apply staged ops to a working copy of every container in
     §4-phase-3a (UTXO map, forest, position index, block undo,
     block index entry). No live container has been touched yet.
  2. Build the unified `rocksdb::WriteBatch` for those staged
     ops plus the canonical pointers (tip, height index).
  3. Add a single journal row to the WriteBatch keyed
     `consensus_journal:<height_be>:<block_hash>` (D1) whose
     value is the byte fingerprint of all the other writes (a
     hash, like `daemon.shieldedstatehash` v1 but over the
     *staged* state post-apply). This row is the single durable
     "commit pointer" — its presence on disk after restart is
     the only thing that distinguishes "applied" from "not
     applied" within the phase-3a container subset.
  4. Commit the WriteBatch.
  5. Move the working-copy containers into the live containers
     (in-memory swap).

If step 4 fails, working copies are dropped, live state is
unchanged. §1.3 holds.

If step 5 throws, the daemon enters safe mode (release) or
aborts (debug) per D4 — this would mean the in-memory swap of
a container we just persisted failed, which is an internal bug,
not a recoverable state.

Phase 3a does NOT touch nullifiers, anchor history file, shielded
tree frontier, or shielded tip marker through the batch. They
continue to be written through their existing per-call paths.
§1.2's commit rule does not yet bind those containers in 3a;
that's why phase 3a is gated by both property tests passing and
why phase 3b is a separate operator-greenlit step.

### 3.2 Phase 3b commit ordering (after nullifier ChainDB fold-in per D2)

The goal of 3b is to **eliminate cross-backend recovery, not
polish it**. Once nullifiers live in a ChainDB column family
(D2), there is exactly one backend, exactly one commit, and the
"sqlite committed but rocksdb didn't" / "rocksdb committed but
sqlite didn't" two-phase window is structurally absent — not
mitigated.

Phase 3b commit ordering:

  1. Apply staged ops to a working copy of every container in
     §2 (all 11). No live container has been touched yet.
  2. Build the unified `rocksdb::WriteBatch` for every staged
     op, including nullifier inserts as column-family writes
     and the anchor history delta as a column-family write
     (anchor history's single-file `Save` becomes a
     ChainDB-rooted column too — same logic as nullifiers).
  3. Add the journal row keyed
     `consensus_journal:<height_be>:<block_hash>` whose value
     is the byte fingerprint of every staged write.
  4. Commit the WriteBatch.
  5. Move the working-copy containers into the live containers.

§1.4's recovery rule collapses from the phase-3a "rocksdb
committed but legacy nullifier path didn't" branch to just "the
rocksdb commit failed" — and the journal row tells us whether
it did. One branch.

The migration commit (D2 ¶2) is a separate, pre-3b deliverable:
drain the legacy sqlite into the new column family, write a
sentinel, preserve the sqlite file. Phase 3b assumes that
migration has already run on first daemon start with the 3b
binary.

### 3.3 Anchor history file fate

Anchor history's `Save`/`Load` to a flat file is the last
non-ChainDB persistence in the audit. Phase 3b folds it into a
ChainDB column too, by the same logic as nullifiers (D2). The
flat file is preserved (read-only, not deleted) for one release
to ease forensic comparison, then deletable. Tracked under §7.5
phase-3 done condition #6.

---

## 4. The minimal staging layer (phase 3a)

Phase 3 lands in two commits, gated by the
`ReindexCopiedDatadir` regression test plus
`ShieldedReorgInvertibility`:

### Phase 3a — narrow staging scaffold (landed)

> **Honesty note (2026-04-28).** Phase 3a as shipped (commits
> `9379909ac` skeleton, `21f938dfb` UTXO routing, `<this commit>`
> D4 leak + flag-toggle + remaining-Stage* declarations) delivers
> the staging surface and the activation flag. It does NOT yet
> deliver §1.2's commit ordering on disk — the existing
> per-container persist code in `ChainstateService::ConnectTip`
> still runs unchanged. Phase 3a's atomicity guarantee is
> therefore **in-memory only**: with the flag on, UTXO mutations
> are recorded into a single ordered log and replayed at
> `ConsensusWriteBatch::Commit()`. The on-disk journal row + the
> "no canonical pointer advances until every staged container
> write succeeds" rule from §1.2 lands in phase 3b alongside the
> nullifier ChainDB fold-in (D2).
>
> What this means for review: phase 3a proves the **scaffold and
> the contract are testable**. It does not retroactively heal the
> existing on-disk drift on the live fleet (gap #10) and it does
> not yet enforce all-or-nothing on disk. The four phase-3a
> property tests landed
> (`ShieldedReorgInvertibility_AtomicPersistOn`,
> `ShieldedReorgInvertibility_AtomicPersistToggle`,
> `ReindexCopiedDatadir`, `ConsensusWriteBatchLeak`) plus the
> baseline flag-off variants are the gate phase 3b cannot regress.



**Reality, not aspiration.** The original phase 3a plan listed
seven containers (UTXO map, utreexo forest, position index,
block undo, block index entry, tip, height index). What actually
shipped under `consensus.atomic_persist=1` is narrower:

- **Wired through the batch (one container):** UTXO map, on the
  ConnectTip path only, in `BlockValidator::ConnectBlockInternal`
  at the deferred-apply commit branch (post-activation, STATEFUL
  path).
- **Still on legacy per-call paths:** utreexo forest, position
  index, block undo, block index entry, tip pointer, height
  index. Their on-disk persist code in `ConnectTip` is unchanged
  with the flag on or off. Their staging-layer wiring is deferred
  to phase 3b.
- **DisconnectTip is NOT routed through the batch.** Phase 1's
  law says Connect AND Disconnect are both transaction boundaries.
  DisconnectTip's batch routing is open work — staying on the
  phase 3a scaffold list, deliberately untouched in this round
  because it shares the same forest / undo / canonical-pointer
  surface that phase 3b is rewriting.
- **Forest still advances before `ConsensusWriteBatch::Commit()`.**
  The forest commit at `block_validation.cpp:~1898` (snapshot
  move-assigned into the live forest) runs INSIDE
  `ConnectBlockInternal`, before ConnectTip calls
  `batch.Commit()` on the UTXO ops. The ordering is reversed
  from §1.2's "canonical pointers move last" rule — phase 3a's
  in-memory replay records mutations after the forest already
  moved. This is acceptable for a scaffold (the legacy path runs
  end-to-end either way) but it is not §1.2's contract. Phase 3b
  fixes the ordering by routing forest mutations through the
  same batch.
- **`ConsensusWriteBatch::Commit()` swallows AddCoin/SpendCoin
  failures.** It logs the failure to stderr but returns
  `Status::Ok`. That is fine for a scaffold whose job is the
  call-site routing scaffold, not §1.3's rollback rule. Phase 3b
  promotes Commit() to fail-loud once the working-copy pattern
  is in place.

Activation contract that DOES hold today:

- The flag is opt-in via `-consensus.atomic_persist=1`, default
  false. With the flag off, every code path runs unchanged.
- D4 destructor contract is wired and CI-enforced via
  `ConsensusWriteBatchLeak`: hard abort in debug, enter safe
  mode (`reason="consensus_write_batch_dropped"`) in release.
- No bypass-path deletion. `RecoverShieldedStateFromTipMarker`'s
  five branches stay. That is phase 5.

**Operator-facing rule:** `consensus.atomic_persist=1` is
**dev/regtest only**. Do not enable on the live fleet. The flag
is a scaffold gate, not a soak switch — that comes after phase
3b lands the working-copy pattern + the journal row.

### Phase 3b — make atomic persistence the law (greenlit 2026-04-28)

Phase 3a proved the route and the flag. Phase 3b makes it law.
**The goal is to eliminate cross-backend recovery, not polish
it** — D2 folds nullifiers (and anchor history, see §3.3) into
ChainDB column families so §3.2's commit ordering has exactly
one backend and exactly one commit. There is no "sqlite
committed but rocksdb didn't" window because there is no sqlite
in the consensus path after 3b.

#### Plain-English rule

When a block connects or disconnects, every consensus container
must move together or none move at all.

No more:
- UTXO persisted but shielded not
- forest checkpoint advanced but nullifiers not
- anchor history written later at shutdown
- startup recovery guessing between five possible mixed states

#### Six-step implementation order (operator's call)

The shortest safe route is **#9 first**, because closing nullifier
content coverage strengthens the test oracle BEFORE we touch
commit ordering. Then the atomic batch. Then retire the recovery
maze.

  1. **Nullifier ChainDB migration + DSRH v2.**
     - New ChainDB column / table: `shielded_nullifiers`
       keyed `nullifier → height`.
     - One-time startup migration: drain `shielded_nullifiers.db`
       (sqlite) → ChainDB → write migration sentinel →
       preserve sqlite read-only backup.
     - Promote `daemon.shieldedstatehash` to v2: hash sorted
       nullifier contents, not just count. Closes audit gap #9
       (count-only coverage) and gives the property tests a
       real content-level oracle for everything that follows.

  2. **Anchor history ChainDB migration.**
     - New ChainDB column / table: `shielded_anchor_history`
       keyed `height → root`.
     - Same migration shape: drain
       `shielded_anchor_history.bin` (flat file) → ChainDB →
       sentinel → keep flat-file read-only for one release.
     - Anchor history's existing `RecordRoot` / `RollbackAbove`
       interface stays; storage moves under the unified
       backend.

  3. **Real `ConsensusWriteBatch` for ConnectTip.**
     - Stage every consensus row:
       UTXO map, utreexo forest checkpoint, position index,
       block undo, block index entry, height index, tip
       pointer, shielded tree frontier, nullifier
       inserts/removals, anchor history records/rollbacks,
       shielded tip marker, journal row.
     - Working copies before live mutation. ConnectTip mutates
       working copies first; live state changes only after
       the rocksdb commit succeeds.
     - Journal row written inside the same WriteBatch:
       `consensus_journal:<height_be>:<block_hash>`.
     - Phase 3a's `consensus.atomic_persist` flag stays
       opt-in for one soak window before becoming default-on.

  4. **Real `ConsensusWriteBatch` for DisconnectTip.**
     - Same staging surface as #3 but inverted. The legacy
       DisconnectBlock path (already snapshot-restore-on-
       failure as of 5bfdd014b) becomes the working-copy
       reference.
     - §1's law applies symmetrically: Connect AND Disconnect
       are both transaction boundaries.

  5. **Crash-injection / property tests.**
     - `AtomicCommitCrashPreCommitAllContainers`,
       `AtomicCommitCrashAfterRocksdbAllContainers`,
       `NullifierMigrationCrashMidDrain`,
       `NullifierMigrationCrashBeforeSentinel`,
       `AnchorHistoryMigrationCrash*` — all required to pass
       with the flag on, off, and in flag-toggle
       configuration before default-on flip.
     - DSRH v2 used as the byte-equality oracle in every
       property assertion.

  6. **Phase 5: delete the recovery maze.**
     - After 3b has soaked default-on for an operator-confirmed
       window, the 5-branch
       `RecoverShieldedStateFromTipMarker` logic becomes
       structurally unreachable (see §1.4 — startup is one
       branch: journal row valid → accept; absent or
       mismatched → roll back to parent).
     - Delete the dead branches. Same for any other
       startup-recovery code that depends on partial-commit
       state.

#### What 3b closes

| Audit row | How 3b closes it |
|---|---|
| **#7** (4-container fsync lifecycle) | One atomic write boundary replaces four. By construction. |
| **#9** (nullifier content coverage in DSRH) | Step 1: ChainDB-backed nullifier set is enumerable; DSRH v2 hashes sorted contents. |
| **#6** (5-branch recovery maze) | Step 6 deletes it after soak. The §1.4 startup recovery rule collapses to "journal row present + valid → accept; otherwise roll back to parent." |

#### Phase 3b is therefore six commits, in order:

  1. nullifier ChainDB migration + DSRH v2 + content-level
     property test (closes #9)
  2. anchor history ChainDB migration
  3. real `ConsensusWriteBatch` for ConnectTip (closes #7)
  4. real `ConsensusWriteBatch` for DisconnectTip
  5. crash-injection + property test surface
  6. (phase 5) delete `RecoverShieldedStateFromTipMarker` and
     siblings (closes #6)

The original §3.2 ordering and the per-step crash injections
already enumerated in §7 stand. This addendum names the
operator's six-step sequence so a future agent picking up phase
3b doesn't re-litigate the order. **Migration commit (D2 ¶2)
in the previous revision** is folded into step 1 here:

  1. **Migration commit:** drain legacy sqlite nullifier database
     into a new `nullifiers` ChainDB column family on first
     start, write the migration sentinel key, preserve the
     sqlite file as read-only. Same shape for anchor history's
     flat file → ChainDB column. No staging-layer change yet.
  2. **Boundary extension commit:** extend `ConsensusWriteBatch`
     to stage all 11 containers from §2. `AtomicCommitCrashAfterSqlite`
     is renamed `AtomicCommitCrashAfterRocksdbAllContainers` —
     its purpose collapses to "the rocksdb commit failed,"
     which is the same as `AtomicCommitCrashPreCommit` for
     the 3b-extended set.

Phase 3b is the point at which the bypass paths become deletable;
the actual deletion is still phase 5.

---

## 5. Out of scope, explicitly

Per the operator's call ("design-first patch, not a sweeping
rewrite") and the §6 locked decisions:

- **No deletion of any existing recovery code in phase 3.** The
  `RecoverShieldedStateFromTipMarker` 5-branch logic stays
  exactly as it is. Phase 5 deletes it after phase 3b is soaked
  and the recovery rule §1.4 has empirically collapsed to one
  branch.
- **No StatelessNode work.** Phase 3 is stateful-mode-only per
  §1.5 ("two-mode separation"). The stateless path's
  `RewindToCheckpoint`/`ReplayBlock` gets its own future phase
  with its own atomic-batch contract mirroring this document
  for that mode's narrower container set.
- **No shielded persistence changes in phase 3a.** Nullifier
  storage (D2) is the pivot, and we do not touch shielded
  persistence until that decision is locked AND 3a has soaked.
  Operator's standing instruction.
- **No DSRH v2 (gap #9).** Nullifier content coverage in the
  composite hash is a separate scoped piece. It naturally falls
  out of phase 3b once nullifiers live in ChainDB
  (range-scannable enumeration replaces the sqlite query),
  but is not in 3b's definition of done.
- **No drifted-fixture or `--inject-v2-forest-serialize` knob
  (gap #10).** That gates the actual LA-9291-style regression
  pinning; not part of phase 3. Phase 3 prevents new drift; gap
  #10 pins existing on-disk drift. Distinct.

---

## 6. Design decisions (locked)

§6 was open questions in the previous revision; the operator
review on 2026-04-28 locked all four. Re-opening any of them
requires updating this section AND re-soaking the property
tests, not just changing the code.

**D1 — Journal key shape: `consensus_journal:<height_be>:<block_hash>`.**

Big-endian height prefix, then block hash. Hash-only would have
been enough for lookup-by-hash, but startup recovery is exactly
when cheap ordering and tail inspection matter — `seek_to_last`
on the column lands the most recent journal row in O(1), and
audit/debug tooling can range-scan a height window without
de-referencing every block hash. Hash collisions are not a
concern (the canonical chain has unique hashes per height) but
the height prefix means even a corrupted hash byte still sorts
into the right neighborhood for forensics. Key encoding: 4-byte
big-endian height (so lexicographic ordering matches numeric),
literal `:`, then the 32-byte hash bytes verbatim.

**D2 — Nullifier storage: fold into ChainDB long-term (phase 3b).**

The separate sqlite database is the source of the cross-backend
two-phase window in §3 (steps 5/6). If nullifiers are consensus
state — and they are, every shielded validation reads them —
they live under the same atomic write backend as the other
consensus containers. Phase 3a may continue to ignore the sqlite
backend (nullifiers are still routed through the legacy per-call
path); phase 3b targets a `nullifiers` ChainDB column family
unless a hard performance blocker surfaces during 3a soak. Once
3b lands, §3 step 6 disappears entirely and §1.4's recovery
collapses from "what if rocksdb committed but sqlite didn't" to
just "what if the rocksdb commit failed" — one branch.

The migration path: phase 3b ships a one-shot startup migration
that drains the existing sqlite database into the new column
family on first run with the new binary, then writes a sentinel
key marking the migration complete. The sqlite file is preserved
(not deleted) so an operator can manually compare on the rare
"missing nullifier" debug session.

**D3 — Test surface cost: pay 3× for one soak window.**

Every property test runs flag-off, flag-on, and flag-toggle
(toggled across a Connect/Disconnect cycle to catch hidden state
that survives the toggle). Phase 3a CI matrix grows by 2×, and
that is the deal until the soak window's "default-on" flip
lands. After flip, the matrix collapses back to a single config
plus a "legacy off" sanity test that runs only on release
branches.

This is where hidden-state bugs surface — a container that looks
correct under either pure-on or pure-off can still leak under
toggle. Worth it.

**D4 — `ConsensusWriteBatch` destructor without Commit/Abort:
hard abort in debug/regtest/test, safe-mode in release.**

  - Debug builds, ctest, regtest: `std::abort()`. The whole
    point of phase 3 is "no third state"; a leaked batch IS a
    third state and the test must fail loudly the instant it
    happens.
  - Release / mainnet: enter `safe_mode_active_` with reason
    `"consensus_write_batch_dropped"`, refuse template generation
    and block connect, log a stack trace, and require operator
    `safemode.exit { confirm: true }` after they've inspected
    the chain. This preserves the "no silent third state" rule
    without casually `abort()`-ing a live fleet node from a
    non-consensus coding bug elsewhere in the daemon.

The branch is compile-time (`#ifdef NDEBUG` or equivalent), not
runtime, so the debug-build behavior cannot accidentally ship to
mainnet. CI explicitly tests both: a "leak the batch on purpose"
test in debug builds asserts on the abort; the same test in a
release build asserts that safe mode is entered.

---

## 7. Definition of done

### 7.1 Existing tests must stay green

The two property tests already landed are the floor. Any change
to staging that breaks either of them is a regression, full stop:

- `ShieldedReorgInvertibility` (`test_shielded_reorg_invertibility.sh`):
  in-place Connect/Disconnect/Connect cycle, asserts byte-equal
  composite state hash.
- `ReindexCopiedDatadir` (`test_reindex_copied_datadir.sh`):
  copy + reindex, asserts byte-equal composite state hash.
  This test is the **gate** per the operator's call: phase 3
  cannot regress copy-then-reindex invertibility.

### 7.2 New crash-injection tests phase 3 must add

Phase 3 introduces a real commit boundary, so it has to be
tested against a real crash. The `DINERO_CRASH_AT` mechanism
already exists (used by
`test_reindex_chainstate_utreexo_equivalence.sh`).

#### Phase 3a injections (steps reference §3.1)

| Injection point | Crash before | Expected post-restart state |
|-----------------|--------------|-----------------------------|
| `consensus_batch_staged_pre_commit` | §3.1 step 4 (rocksdb commit) | tip = N-1 (rollback rule §1.3) |
| `consensus_batch_post_rocksdb_pre_inmem_swap` | §3.1 step 5 (in-memory swap) | tip = N (commit succeeded; in-memory swap on next restart re-loads from disk and is consistent) |

ctest targets: `AtomicCommitCrashPreCommit`,
`AtomicCommitCrashAfterRocksdb`. Both plus the existing
property tests must pass for phase 3a to be done.

Note: phase 3a does NOT have a "rocksdb committed but sqlite
didn't" injection because phase 3a does not route nullifiers
through the batch (D2's reasoning: the cross-backend window is
the bug to eliminate, not test against). That injection only
makes sense in 3b — and in 3b it is structurally absent because
there is only one backend.

#### Phase 3b injections (steps reference §3.2)

| Injection point | Crash before | Expected post-restart state |
|-----------------|--------------|-----------------------------|
| `consensus_batch_staged_pre_commit_full` | §3.2 step 4 (rocksdb commit, all containers) | tip = N-1 (rollback rule §1.3) |
| `consensus_batch_post_rocksdb_pre_inmem_swap_full` | §3.2 step 5 (in-memory swap) | tip = N (commit succeeded) |

ctest targets:
`AtomicCommitCrashPreCommitAllContainers`,
`AtomicCommitCrashAfterRocksdbAllContainers`. Same shape as 3a,
just with all 11 containers in §2 inside the boundary.

#### Migration injection (3b prereq)

| Injection point | Crash before | Expected post-restart state |
|-----------------|--------------|-----------------------------|
| `nullifier_migration_mid_drain` | the migration sentinel write | re-run migration on next start; idempotent (sentinel absent) |
| `nullifier_migration_post_drain_pre_sentinel` | sentinel write commit | re-run migration on next start; column-family writes are idempotent (insert-or-replace), so re-drain is a no-op modulo the sentinel |

ctest targets: `NullifierMigrationCrashMidDrain`,
`NullifierMigrationCrashBeforeSentinel`. Both must pass before
3b's boundary-extension commit lands.

#### D4 destructor coverage

One additional test family asserts the D4 contract: a deliberate
"destruct without Commit/Abort" trip. ctest target
`ConsensusWriteBatchLeak`:

  - Debug build: must abort.
  - Release build: must enter safe mode with reason
    `consensus_write_batch_dropped`.

Both must pass for 3a to be done.

### 7.3 Gap #10 stays open

The drifted-fixture / `--inject-v2-forest-serialize` work to pin
the actual LA-9291-style failure (audit gap #10) is **not**
satisfied by phase 3. Phase 3 prevents *new* drift; gap #10 pins
*existing on-disk* drift. Phase 3 done does not mean gap #10
done. The doc must keep saying that out loud.

### 7.4 Phase 3a checklist

Phase 3a is done when:

1. `ConsensusWriteBatch` exists with the exact mutation surface
   in §3.1 for the §4-phase-3a container subset.
2. The hidden flag (`consensus.atomic_persist`, default off)
   wires `ConnectTip`/`DisconnectTip` through it.
3. `ShieldedReorgInvertibility` and `ReindexCopiedDatadir` pass
   in all three configurations per D3 (flag off, flag on,
   flag-toggle).
4. `AtomicCommitCrashPreCommit` and
   `AtomicCommitCrashAfterRocksdb` exist and pass with the flag
   on.
5. `ConsensusWriteBatchLeak` passes per D4 (abort in debug, safe
   mode in release).
6. The commit message enumerates the in-scope containers and
   reaffirms the out-of-scope list from §2 / §5.

### 7.5 Phase 3 (a + b) checklist

Phase 3 overall is done when:

7. The nullifier migration commit (3b prereq) has shipped, the
   migration sentinel exists, and `NullifierMigrationCrashMidDrain`
   + `NullifierMigrationCrashBeforeSentinel` pass.
8. All 11 containers in §2 are inside the boundary
   (`ConsensusWriteBatch` extended per §3.2).
9. The journal row keyed `consensus_journal:<height_be>:<block_hash>`
   is the single durable commit pointer; startup-recovery code
   that depends on partial-commit state is marked deletable and
   tracked under phase 5.
10. `AtomicCommitCrashPreCommitAllContainers` and
    `AtomicCommitCrashAfterRocksdbAllContainers` pass.
11. Cross-backend recovery is structurally absent — sqlite is no
    longer in the consensus path. §3.3 anchor history flat-file
    is read-only-preserved for one release, then deletable.
12. Gap #10 has its own follow-up — phase 3 does not
    retroactively heal the existing on-disk drift on the fleet,
    and the doc says so out loud.

---

## 8. Why this is the right shape

The framing landed in `3351132b8`: rings broke Utreexo, shielded
adds the same risk pattern, the next consensus container will
do it again unless we lock the contract structurally. Phase 3a's
opt-in flag means we can prove the contract on regtest before
the live fleet touches it, and back out cleanly if we find a
problem. Phase 3b extends the boundary one layer at a time
rather than rewriting four containers at once.

The copied-mainnet reindex regression (`ReindexCopiedDatadir`,
landed in `bade60b6d`) is the gate, exactly as the operator
called: phase 3 cannot regress copy-then-reindex invertibility,
and any change to staging that does so trips that test loud.
