# Canonical State And Recovery Plan

**Date**: 2026-04-19  
**Status**: Working architecture plan / high-priority reference  
**Scope**: Consensus state, derived state, restart/reindex/recovery semantics

---

## Purpose

Recent chain incidents were not caused by one feature in isolation. Removing the
legacy ring stack reduced one source of divergence, but the deeper failure mode
remained:

- live sync and rebuild paths did not always produce the same result
- restart and steady-state paths applied different logic
- some derived state behaved like authoritative consensus state
- recovery code assumed future block activity could heal partial state

This document sets the rule Dinero must move toward:

> Every consensus-relevant state object must have exactly one canonical
> derivation from accepted block history, and every code path must converge to
> that same result.

That includes:

- normal sync
- restart
- reorg
- `--reindex-chainstate`
- restore-from-block-files
- invalidate/reconsider flows

If two paths can reach the same tip but leave different effective state behind,
the system is not recovery-safe.

---

## Ownership And Enforcement

This document must not become a dead reference.

- **Primary owner (role)**: the maintainer/reviewer responsible for consensus,
  chainstate, or restart/recovery changes on the active branch
- **Update obligation**: any PR that changes a Class A or Class B object must
  update this document, or explicitly state why no canonical-state contract
  changed
- **PR gate**: any PR touching consensus/storage/reindex/recovery code must
  answer the review checklist in this document, either in the PR description or
  in linked design notes
- **Review rule**: if a PR changes a Class A/B object without explaining its
  canonical source, rebuild path, and restart/reindex effect, it is not ready

Until Dinero has a more formal architecture committee, this role-based rule is
the enforcement mechanism.

---

## First Concrete Application

This plan is not speculative. The branch already contains fixes that implement
its principles:

- `b6df458b4` `chainstate(v7): fix three crash-safety invariants in ConnectTip / IBD / reindex`
- `7cc2c2d26` `reindex(v7): rebuild Utreexo forest + checkpoints + tip marker inline`

Those commits are the first concrete application of:

- undo-before-tip durability
- restart semantic preservation
- clearing stale recovery state only after a successful canonical rebuild
- making derived Utreexo state converge with the active chain after reindex

Future work in this document should be read as extending that same line of
repair, not replacing it.

---

## Core Principles

### 1. Single Source Of Truth

Consensus state must be rooted in accepted block history, not in mutable helper
indexes or caches.

### 2. Replay Equivalence

A node that replays canonical block history from scratch must land in the exact
same consensus-relevant state as a node that reached the tip by uninterrupted
live sync.

### 3. Sticky Invalidity

Invalid block state must remain invalid across relay, restart, reindex, and
candidate-chain churn unless an explicit reconsider operation clears it.

### 4. No Healing By Accident

Consensus-relevant state may not be left partially broken on startup with the
assumption that future block traffic will repair it.

### 5. Derived State Must Be Honest

If a structure is derived from canonical history, then one of two things must be
true:

- it is **consensus-checked** and rebuilds deterministically from canonical
  inputs, or
- it is **disposable helper state** and cannot influence block acceptance,
  chain selection, safety gating, or restart behavior

---

## State Classes

Dinero should explicitly classify every persisted object into one of these
classes.

### Class A: Canonical Consensus State

These objects are authoritative and directly define validity or active-chain
selection.

Examples:

- block headers / block index metadata
- active-chain tip selection metadata
- UTXO set
- undo availability required for safe disconnect/reorg
- persistent invalid-block markers

### Class B: Derived But Consensus-Checked State

These objects are derived from Class A inputs, but they still influence
consensus checks, restart gates, or safety decisions. They must therefore be
deterministic and replay-equivalent.

Examples:

- Utreexo forest / root / checkpoints
- shielded commitment frontier if shielded validation is active
- shielded nullifier set if shielded spends are active
- consistency markers that gate startup or safe-mode entry

### Class C: Disposable Auxiliary State

These objects may be expensive or useful, but they must not alter consensus or
make a healthy chain appear unhealthy.

Examples:

- wallet address indexes
- wallet shielded note discovery state
- explorer indexes
- mempool-only caches
- precomputed UI or telemetry projections

If a Class C object is lost, the node may become slower or less user-friendly,
but it must not change block acceptance behavior.

---

## Deterministic Rebuild Semantics

"Rebuild deterministically" needs an explicit comparison level. Different state
objects require different kinds of equivalence.

### Equivalence Levels

1. **Acceptance-equivalence**

Two reconstructed states accept and reject the same blocks and transactions.

2. **Root-equivalence**

Two reconstructed states produce the same consensus commitment at the same
height, even if their internal helper layout differs.

3. **Byte-equivalence**

Two reconstructed persisted artifacts are byte-for-byte identical. This is
required when restart, proof serving, checksums, or sidecar validation consume
the serialized form directly.

### Utreexo-Specific Rule

For the Utreexo forest and related artifacts:

- **minimum consensus requirement**: root-equivalence at every active-chain
  height
- **required if persisted artifacts are consumed by restart, proof serving, or
  safety checks**: byte-equivalence for checkpoints, checksum/version fields,
  and any persisted sidecars whose interpretation affects restart or proof
  generation
- **required if positions are persisted and later reused**: deterministic
  position-equivalence for the same accepted chain history

If an artifact does not meet that bar, it must be treated as disposable helper
state and rebuilt before use.

---

## Inventory Template

Every consensus-adjacent subsystem should be documented using this template:

| Object | Class | Authoritative Source | Persisted Where | Mutated By | Rebuild Story | Equivalence Level | May Block Startup? |
|---|---|---|---|---|---|---|
| Example: UTXO set | A | Accepted block history + active chain | Chainstate DB | `ConnectTip` / `DisconnectTip` | Replay canonical blocks | Acceptance-equivalence | Yes |

The table below is **not yet the full canonical inventory**. It is the current
Phase 1 high-risk subset: the objects most likely to produce consensus or
recovery divergence if they drift.

---

## Current High-Risk State Inventory (Phase 1 Subset)

| Object | Class | Authoritative Source | Persisted Where | Mutated By | Required Rule |
|---|---|---|---|---|---|
| Block headers and block index metadata | A | Validated block history | headers CF / block index / chain DB | header acceptance, block connect, invalidate/reconsider | Must survive restart/reindex without alternate interpretations of tip or work |
| Active chain selection (`best block`, candidate ordering, tip metadata) | A | Best validated chain by total work | chain DB / in-memory mirrors | block acceptance, reorg, invalidation | Must be identical after replay and restart |
| Persisted ChainDB tip vs in-memory `active_tip_` / tip caches | A | Accepted active chain | chain DB + process memory | startup, `ConnectTip`, reorg, restore, reindex | Restart must not create a second truth between disk tip and in-memory tip |
| UTXO set | A | Accepted main-chain transactions | chainstate DB | `ConnectTip`, `DisconnectTip`, reorg | Must be the same after fresh replay, restart, and `--reindex-chainstate` |
| Undo data and undo metadata | A | Pre-state consumed by connected blocks | undo storage + block metadata | block connect/disconnect | Must be written in an order that never leaves tip advancement ahead of durable undo |
| Persistent invalid-block markers (`BLOCK_FAILED_VALID`, similar status) | A | Prior validation outcome | block index / persistent metadata | invalidate, validation failure, reconsider | Must remain sticky across restart and relay unless explicitly reconsidered |
| Utreexo forest / in-memory forest / persisted checkpoints / root mirrors | B | Deterministic function of active-chain UTXO state | forest/checkpoint storage + in-memory state | block connect/disconnect, recovery, reindex | Must either rebuild deterministically from canonical chainstate or be fully disposable and re-derived before use |
| Utreexo position index / proof-serving sidecars | B if consumed after restart, otherwise C | Accepted active chain plus deterministic leaf ordering | sidecar DB / proof cache / checkpoint artifacts | connect/disconnect, proof serving, restart | If reused after restart, position semantics must be deterministic for the same chain |
| Utreexo root in headers vs locally computed root | B | Header commitment plus canonical post-state | header field + local forest | block validation, mining template creation, restart consistency checks | Validation, restart, and template-building must use one oracle for root derivation |
| Utreexo checkpoint checksum / version flags | B | Canonical checkpoint encoding for a given active-chain state | checkpoint metadata | checkpoint write/read, restore, startup | Checksum/version interpretation must be byte-equivalent whenever consumed by recovery or proof logic |
| Reorg / recovery markers (`reorg_in_progress`, `chainstate_recovery.marker`) | B | Actual interrupted recovery state | UTXOIndex / datadir marker files | startup, reorg, reindex, recovery logic | Markers may gate startup, but stale markers must never outlive a successful canonical rebuild |
| Shielded commitment tree / frontier | B when shielded consensus is active | Accepted shielded outputs on active chain | shielded state DB / in-memory frontier | block connect/disconnect, shielded validation | Must be replay-equivalent to normal sync; must not rely on wallet-side repair |
| Shielded nullifier set | B when shielded consensus is active | Accepted shielded spends on active chain | shielded nullifier DB | block connect/disconnect, shielded validation | Must rollback deterministically on reorg and rebuild from chain history |
| Shielded pre-block frontier snapshots / disconnect rollback state | B when shielded consensus is active | Connected active-chain pre-block frontier | block undo storage | block connect/disconnect, reorg | Must restore the exact pre-block frontier on disconnect; must never be synthesized from wallet-local state |
| Wallet address indexes / receive tables / balance projections | C | Wallet-owned records plus chain scan results | wallet DB | wallet sync, RPC, UI refresh | Loss or drift must not affect block acceptance or daemon startup |
| Wallet shielded note store / local shielded leaf cache | C today, unless promoted into consensus behavior | Wallet-local detection state | wallet DB | wallet worker, note discovery, UI | Must never be treated as proof of consensus truth |

---

## Shielded State Classification (Current Branch)

The current branch should treat shielded state as split cleanly between
chain-owned consensus-checked state and wallet-owned helper state.

### Class B: Chain-Owned Shielded State

On the current v7 branch, shielded state is already consensus-active whenever a
block contains a `TX_VERSION_SHIELDED = 5` transaction. This is not hypothetical
future state:

- `ValidateBlockShielded(...)` is called during block connection
- shielded proof failure or nullifier reuse causes block acceptance to fail
- `pre_block_shielded_frontier` in `BlockUndo` is real disconnect/reorg state

So the frontier/nullifier/rollback objects below must be treated as live Class B
state today, not as parked design placeholders.

These objects participate in consensus validation, restart safety, or reorg
rollback and therefore must remain deterministic:

- `ChainstateService::shielded_tree_` and its persisted frontier file
  (`blockchain/shielded_frontier.bin`)
- `ChainstateService::shielded_nullifiers_` and its persisted nullifier DB
  (`blockchain/shielded_nullifiers.db`)
- per-block shielded frontier snapshots stored in `BlockUndo` for disconnect
  and reorg rollback

### Class C: Wallet-Owned Shielded State

These objects are wallet convenience or discovery state only and must never be
treated as consensus truth:

- wallet shielded note rows
- wallet-side shielded leaf stream / shadow commitment tree
- pending-vs-confirmed note tracking
- encrypted-note discovery / view-key scan metadata

### Current Executable Coverage

The current shielded recovery proof surface is exposed as one CTest label:
`shielded-canonical-recovery`

Run it with:
`ctest --test-dir build -L shielded-canonical-recovery --output-on-failure`

This gate currently proves persistence, reopen, and rollback semantics for the
chain-owned shielded frontier/nullifier state. On the current branch it runs:

- `ShieldedReindexEquivalence`
- `ShieldedPoolRoundTrip`
- `ShieldedAdversarialHardening`
- `ShieldedTipMarkerRestartEquivalence`

That means the shielded gate now covers mined live-vs-reindex replay
equivalence, persisted reopen semantics, adversarial rollback/nullifier
hardening behavior, and shielded tip-marker verify/heal/rewind behavior.

The next critical known gap is no longer reindex rebuild or basic startup
tip-marker consistency. It is the full daemon-valid crash/restart family on
mined shielded blocks.

---

## Required Architecture Rules

### Rule 1: One Rebuild Story

For every Class A or Class B object, the project must be able to answer:

- What canonical inputs define it?
- What exact algorithm rebuilds it?
- Which code path performs that rebuild?
- What invariant proves rebuild success?

If the answer is “normal sync updates it, but reindex does not,” the design is
incomplete.

Persisted copies, checkpoints, and caches are allowed for performance. The rule
is not "reindex every restart." The rule is:

- performance copies may exist
- startup may trust them only if they validate against canonical invariants
- if they are missing or suspect, they must be rebuildable from canonical inputs
- recovery must never depend on a copy that cannot be re-derived

### Rule 2: Restart Must Not Change Semantics

Steady-state validation logic and restart logic must preserve the same truth.

Startup may:

- validate that persisted state is coherent
- rebuild deterministic derived state
- refuse to start

Startup may **not**:

- silently drop consensus-relevant invalidity
- serve with partially reconstructed derived state
- assume live traffic will eventually repair missing pieces

### Rule 3: Recovery Markers Are Not Consensus

Recovery markers exist to coordinate safety, not to define reality. They must be
cleared or regenerated based on canonical state transitions, not carried forward
as immortal poison from an older failed attempt.

### Rule 4: Derived State Cannot Secretly Become Authoritative

A derived object becomes dangerous when:

- validation depends on it
- mining template construction depends on it
- safe mode depends on it
- reorg/restart behavior depends on it

At that point it must be treated as Class B and given a deterministic rebuild
story.

### Rule 5: Invalidation Must Be Durable

No peer-relay path, startup path, or candidate-chain refresh may wash away prior
invalidity. The only legal escape hatch is an explicit reconsider operation with
clear operator intent.

---

## Equivalence Test Matrix

Unit tests are not enough for this class of bug. Dinero needs equivalence tests
that compare whole resulting state across code paths.

### Minimum Required Comparisons

| Scenario A | Scenario B | Expected Result |
|---|---|---|
| Fresh sync from genesis | Replay raw block files into empty chainstate | Same tip, same UTXO set, same invalidity map, same Utreexo root/checkpoints |
| Replay raw block files | Restart at tip | Same tip, same acceptance results, same startup safety status |
| Fresh sync | `--reindex-chainstate` on the same block files | Same tip, same UTXO set, same derived consensus-checked state |
| Live reorg | Restart after interrupted reorg recovery | Same active tip and same disconnect/connect result |
| Invalidate block, restart | Invalidate block without restart | Same failed block status, same active chain |
| Reconsider block, restart | Reconsider block without restart | Same candidate eligibility and same active chain |
| Mining template creation before restart | Mining template creation after restart | Same header-critical commitments and same policy gating |

### Fault-Injection Cases

The system should also be tested under crash points such as:

- after undo written, before tip metadata commit
- after tip metadata commit, before derived-state checkpoint update
- after derived-state checkpoint update, before marker clear
- after block invalidation persisted, before restart

The node must either recover deterministically or halt safely with an explicit
rebuild path.

---

## Crash-Injection Tooling Contract

The fault-injection list above must be backed by a stable harness, not just a
wishlist.

### Proposed Contract

- **build mode**: dev/test only
- **activation**: environment variable such as `DINERO_CRASH_AT=<hook>`
- **optional countdown**: `DINERO_CRASH_AFTER_N=<count>` for "crash on the Nth
  hit" behavior
- **required behavior**: hard process abort at a named hook after the targeted
  write/transition completes

### Initial Hook Set

- `after_undo_before_tip`
- `after_tip_before_utreexo_checkpoint`
- `after_utreexo_checkpoint_before_marker_clear`
- `after_invalidity_persist_before_restart`

If existing lower-level fault-injection hooks already exist, they should be
normalized to this contract rather than multiplied into incompatible ad hoc
switches.

---

## Phase 1 Scope

Phase 1 is the end-to-end audit scope for Q2. It is intentionally small enough
to finish and concrete enough to test.

Objects in scope:

1. Block headers / block index metadata / headers CF
2. Persisted ChainDB tip vs in-memory `active_tip_`
3. UTXO set
4. Undo data and undo metadata
5. Persistent invalid-block markers
6. Reorg / recovery markers
7. Utreexo forest / checkpoints / checksum-version metadata / position sidecars

For each object above, the project should explicitly document:

- canonical source
- mutation path
- rebuild path
- equivalence level
- startup validation rule
- fault-injection hook coverage

The full inventory can expand later. Phase 1 exists to make the document
actionable immediately rather than theoretically complete someday.

---

## Phase 1 Audit Register (Seed Entries)

The table below is the first operational pass for the Phase 1 objects. It is
not the final inventory, but it is specific enough to use in code review and CI
planning right now.

| Object | Primary Owner (Role) | Canonical Source | Steady-State Mutation Path | Rebuild / Restart Path Today | Equivalence Target | Startup / Recovery Rule | First Hook / Test Coverage |
|---|---|---|---|---|---|---|---|
| Block headers / block index metadata / headers CF | Consensus + storage maintainer | Validated block history and accumulated work | Header acceptance, block-index persistence, active-chain promotion | Header/block-index reload during `ChainstateService::Init`; full replay via `BlockReindexer` | Acceptance-equivalence | Startup must not produce an alternate view of tip/work because headers CF, persisted best-header markers, and block index were rebuilt in a different order | Covered by `HeaderRestartSafety`, `HeaderBacklogRestartEquivalence`, `HeaderFilterReplayEquivalence`, and `HeaderCFRestartEquivalence` |
| Persisted ChainDB tip vs in-memory `active_tip_` | Chainstate / restart maintainer | Accepted active chain after the last successful canonical transition | `ConnectTip`, `DisconnectTip`, `ActivateBestChain`, tip/meta persistence | `ChainstateService::Init`, checkpoint restore, AssumeUTXO restore, startup self-realignment | Acceptance-equivalence | Startup may not silently choose between disk tip and `active_tip_`; if they differ, the rule for which one wins must be explicit and reproducible | Add crash hook around `after_undo_before_tip`; add restart test that compares DB tip, UTXO tip, and `active_tip_` |
| UTXO set | Consensus / chainstate maintainer | Accepted active-chain transactions | `ConnectTip` / `DisconnectTip` coin application | `--reindex-chainstate`, full replay, snapshot/restore paths, interrupted reindex-promotion recovery in `DaemonApp` | Acceptance-equivalence | If the persisted UTXO set is missing or suspect, recovery must rebuild it from canonical blocks before serving consensus, and a crashed reindex promotion must resume to one coherent live chainstate before startup continues | Covered by `ReindexChainstateUtreexoEquivalence` and `ReindexPromotionRestartEquivalence` |
| Undo data and undo metadata | Consensus + storage maintainer | Pre-state required to disconnect accepted blocks safely | Undo flatfile write + ChainDB metadata update in `ConnectTip` | Rebuilt by `BlockReindexer`; consumed by `DisconnectTip` and reorg paths | Acceptance-equivalence | Tip advancement may never become durable before the matching undo record is durable and addressable | Crash hook `after_undo_before_tip`; interrupted-connect restart test |
| Persistent invalid-block markers | Consensus / candidate-selection maintainer | Prior validation result for a specific block and descendants | Validation failure, explicit invalidate, reconsider | Reload from block index and candidate rebuild on startup | Acceptance-equivalence | Invalidity must remain sticky across restart, relay, candidate refresh, and crash-interrupted reconsider until explicit reconsider completes durably | Covered by `InvalidityRestartSticky`, `InvalidityCrashRestartEquivalence`, `ReconsiderCrashRestartEquivalence`, and `InvalidityImportEquivalence` |
| Reorg / recovery markers (`reorg_in_progress`, `chainstate_recovery.marker`) | Recovery / startup maintainer | Actual interrupted recovery state, never consensus truth | Reorg start/finish, recovery entry/exit, reindex orchestration | Startup marker handling in `DaemonApp` and `ChainstateService::Init` | Acceptance-equivalence | Markers may trigger recovery, but stale markers must be cleared after a successful canonical rebuild and must not poison healthy state forever | Covered by `RecoveryMarkerRestartEquivalence`, `ReorgMarkerAlignedRestartEquivalence`, and `InterruptedReorgFailSafe` |
| Utreexo forest / checkpoints / checksum-version metadata / position sidecars | Consensus + Utreexo maintainer | Active-chain UTXO state plus deterministic leaf ordering where positions are persisted | `ConnectTip` / `DisconnectTip`, checkpoint emission, position-index updates | Checkpoint restore in `ChainstateService::Init`; full forest/checkpoint rebuild in `BlockReindexer`; position index rebuild at startup | Root-equivalence minimum; byte-equivalence for checkpoints/checksum metadata; position-equivalence if persisted sidecars are reused | Recovery may trust checkpoints only after checksum/version validation; if not, rebuild before proof serving or startup gating | Fresh sync vs replay vs reindex Utreexo-root test; crash hook `after_tip_before_utreexo_checkpoint` |

### Phase 1 Code Anchors

These are the first places reviewers should inspect when a Phase 1 object is
modified:

- Block/index reload and startup alignment:
  `src/daemon/services/chainstate_service.cpp`
- Reindex and canonical rebuild logic:
  `src/consensus/reindexer.cpp`
- Reindex promotion and recovery-marker hygiene:
  `src/daemon/daemon_app.cpp`
- ChainDB persistence for undo, checkpoints, and Utreexo metadata:
  `src/storage/chain_db.cpp`
- Undo flatfile durability:
  `src/storage/block_storage.cpp`
- Persistent invalidity propagation:
  `src/daemon/block_acceptor.cpp`, `src/consensus/block_lifecycle.cpp`
- Utreexo position sidecars:
  `src/indexing/utxo_position_index.cpp`

The immediate goal is not to document every file in the repository. The goal is
to ensure every Phase 1 object has:

- a named owner role
- a known canonical source
- a known steady-state mutator
- a known rebuild path
- a known equivalence target
- at least one explicit crash/restart test plan

The current executable Phase 1 recovery gate is exposed as one CTest label:
`phase1-canonical-recovery`

Run it with:
`ctest --test-dir build -L phase1-canonical-recovery --output-on-failure`

---

## Immediate Engineering Priorities

### Priority 1: State Inventory

Create and maintain a living inventory for every Class A and Class B object.
Start by keeping the Phase 1 audit register above current. This should be
treated like protocol documentation, not tribal knowledge.

### Priority 2: Utreexo Rebuild Equivalence

`--reindex-chainstate` and any restore path must produce the same effective
Utreexo state as uninterrupted main-chain replay. If Utreexo remains consensus-
checked, rebuilding it cannot be optional.

### Priority 3: Persistent Invalidity Audit

Audit every path that touches failed block status, candidate selection,
reconsider, and relay admission. Invalidity must remain sticky unless
reconsidered.

### Priority 4: Recovery-Marker Discipline

Document which markers are:

- advisory only
- hard safety gates
- rebuild triggers

Then ensure they are created and cleared only at canonical transition points.

### Priority 5: Shielded-State Classification

Keep the shielded classification above current and extend its executable proof
surface. Do not let wallet convenience state leak into consensus assumptions,
and do not treat frontier/nullifier persistence as equivalent to full replay
proofs until mined shielded transaction replay is covered.

### Priority 6: Shielded Startup Equivalence

Because shielded validation is already consensus-active on v7, startup must
prove that the persisted shielded frontier/nullifier state belongs to the
persisted active tip the same way Utreexo startup does with
`ForestTipMarker`. Reindex equivalence and `ShieldedTipMarker` startup
alignment are now covered, and the Phase 2 daemon-valid crash/restart gate is
green under `phase2-shielded-canonical-recovery` with four real-`dinerod`
proofs:

- `after_undo_before_tip`
- `after_tip_before_checkpoint`
- `after_disconnect_tip_before_shielded_flush`
- second clean restart after rollback retry, proving the invalidated
  shielded block stays dead across a later clean boot

Phase 2 proof construction also confirmed a load-bearing daemon behavior:
after shielded rollback retry, reorg reconciliation restores the disconnected
shielded spend to mempool, so the remine path is daemon-native rather than an
operator-assisted re-submission flow.

See [SHIELDED_STATE_CLASSIFICATION.md](./SHIELDED_STATE_CLASSIFICATION.md)
for the current classification, rebuild stories, and known gaps. The
main structural items still open are the remaining daemon-valid crash/restart
boundaries on longer shielded histories, even though the current
`shielded-canonical-recovery` gate now proves mined
replay/reindex equivalence, persistence/reopen, adversarial rollback
behavior, and tip-marker verify/heal/rewind behavior, and the separate Phase 2
gate proves four live crash/restart boundaries.

---

## Practical Review Checklist

When reviewing any consensus/storage/recovery PR, ask:

1. Which state object does this change mutate?
2. Is that object Class A, B, or C?
3. What is the canonical source of truth?
4. Does restart preserve the same result?
5. Does `--reindex-chainstate` preserve the same result?
6. If the process crashes halfway through, what remains on disk?
7. Can any stale marker or cache make a healthy chain look inconsistent?
8. If a block is invalid before restart, can it become eligible after restart?

If those answers are not crisp, the change is not ready.

---

## Non-Goals

This document does **not** say:

- every structure must be recomputed on every startup
- no auxiliary indexes are allowed
- shielded or Utreexo features are invalid by design

It says:

- consensus-relevant truth must be canonical
- derived state must be deterministic or disposable
- recovery paths must be equivalent to steady-state paths

---

## Bottom Line

The right question is no longer:

> “Which feature caused the latest incident?”

The right question is:

> “Which state transition was not canonically specified across sync, restart,
> reindex, and recovery?”

That is the level where Dinero becomes robust enough to operate without manual
forensics every time a node takes damage.
