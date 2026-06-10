# AssumeUTXO Fatal-On-Mismatch State Machine

Status: draft
Target: dinero-v8 daemon / NodeCore / operator surfaces
Date: 2026-06-09

## Purpose

Dinero can use an AssumeUTXO snapshot to make a node usable quickly, then
background-validate historical blocks from genesis to the snapshot base. This is
the right stable UX only if the background path is a real security property, not
a progress display.

This spec defines the required lifecycle for a fast-default node:

- snapshot bootstrap may make the node usable before full historical validation
- background validation must eventually prove the snapshot from genesis
- snapshot-vs-recomputed mismatch is fatal
- background validation stall is loud and machine-readable
- only a completed genesis-to-base comparison may retire the snapshot trust
  assumption

## Non-Goals

- This spec does not define snapshot file format.
- This spec does not define snapshot generation or signing.
- This spec does not change consensus validity rules.
- This spec does not require mobile devices to store archival block history.
- This spec does not define scheduling, throttling, or resource budgeting for
  background validation running concurrently with foreground sync/serving. That
  is specified separately; this spec assumes background validation makes forward
  progress when resources and peers allow.

## Trust Model

AssumeUTXO is a Layer 1 state-representation optimization. It must never become
a Layer 0 validity authority.

At load time, the node may trust a snapshot only because its metadata is bound
to an anchor compiled into the authenticated binary. That trust is temporary.
The final authority is a full replay from genesis to the snapshot base using the
same block connection rules used by normal IBD.

If background validation cannot prove the snapshot, the node must not present
itself as fully validated.

### Anchor Binding and Threat Layers

The compiled-in anchor MUST bind both:

- the snapshot base block hash, and
- a snapshot-content commitment at that base. In the current v8 registry this
  is the whole-file snapshot SHA256; an equivalent future format may commit
  directly to a canonical UTXO/accumulator state.

Load-time verification checks the loaded snapshot against both. Therefore a
swapped or content-forged snapshot file whose committed state does not match the
binary's anchor is rejected at load and never reaches `validating_history`.

Given that, the security background validation adds over load-time is precise and
should not be overstated:

1. It defends against a compromised or buggy binary that shipped a
   self-consistent but wrong anchor (correct-looking base hash + matching but
   incorrect committed UTXO set). Only a genesis-to-base replay exposes this,
   because both load-time inputs came from the same untrusted binary.
2. It retires the trust assumption entirely, moving the node to a state that
   depends on no anchor at all going forward.

It does not add protection against a swapped snapshot file; load-time
already covers that. This bounds the threat model honestly: the residual trust
at load is trust in the authenticated binary, which is why signed releases
(not the snapshot) are the real root of trust here.

An external manifest may add transport metadata or require a signature, but it
MUST NOT replace the compiled-in anchor unless that manifest is itself
authenticated by an equal-or-stronger release trust root. Unauthenticated
manifest data cannot be a stable-server trust anchor.

If a future implementation's anchor binds only the base block hash and not the
snapshot content/state commitment, this section and Test 1 must be rewritten: in
that case the UTXO content is trusted blindly at load and background validation
becomes the first check of it, a materially stronger claim.

## State Machine

### States

`disabled`

No snapshot is configured or loaded. The node is doing normal IBD / steady-state
operation.

`snapshot_loaded`

The snapshot passed load-time gates and the node may serve foreground reads from
the assumed state. This state is not fully validated.

`validating_history`

The node is reconstructing pre-snapshot history from genesis to the snapshot
base. Progress is monotonic and machine-readable.

`validation_stalled`

The node has not made background-validation progress within the configured stall
window. The snapshot may remain foreground-usable, but the node must expose that
the historical proof has stalled.

`fatal_mismatch`

Background validation produced a recomputed state that does not match the
snapshot commitment, or encountered a hard validation failure proving the
snapshot cannot be trusted.

`fully_validated`

The node replayed all blocks from genesis through the snapshot base, recomputed
the expected state commitment, matched it to the loaded snapshot, and cleared
the AssumeUTXO trust marker.

### Allowed Transitions

```text
disabled
  -> snapshot_loaded
  -> disabled

snapshot_loaded
  -> validating_history
  -> fatal_mismatch
  -> disabled

validating_history
  -> validation_stalled
  -> fatal_mismatch
  -> fully_validated
  -> disabled

validation_stalled
  -> validating_history
  -> fatal_mismatch
  -> disabled

fatal_mismatch
  -> disabled only after explicit operator reset

fully_validated
  -> disabled only by normal shutdown/reset semantics
```

Forbidden transitions:

- `snapshot_loaded -> fully_validated` without replay and comparison.
- `validating_history -> fully_validated` when any required historical block was
  skipped because the body was missing.
- `fatal_mismatch -> validating_history` without explicit operator reset.
- `validation_stalled -> fully_validated` without resumed progress and complete
  comparison.

## Completion Criteria

The node may enter `fully_validated` only after all of the following are true:

1. Every block from genesis through `snapshot_base_height` was available.
2. Every block in that range was validated through the normal block connection
   path.
3. The reconstructed UTXO/state commitment equals the loaded snapshot
   commitment.
4. The snapshot base block hash and snapshot-content commitment equal the
   compiled-in anchor.
5. The node persisted a durable marker that the snapshot trust assumption has
   been retired.

Missing historical block bodies are not success. They are either
`validating_history` with outstanding downloads or `validation_stalled` after
the stall window expires.

## Fatal Mismatch Semantics

A mismatch is not a warning and not an automatic rollback-to-genesis success
path. It means the node has been serving from state that failed later proof.

A from-genesis replay that produces a different, higher-work chain than the
snapshot's chain is also a mismatch and MUST follow this path, not a silent
reorg below the snapshot base.

Detection is after the fact. By the time replay reaches the base, the node may
have run for hours or days in `snapshot_loaded` / `validating_history` and
already credited wallet balances, accepted payments, or mined on the assumed
chain. Halting new decisions is therefore necessary but not sufficient: the node
must also disown the results it already derived from the assumed state.

On mismatch the daemon MUST:

1. Enter `fatal_mismatch`.
2. Persist the fatal state and error reason before shutdown.
3. Stop accepting new foreground wallet/payment/mining decisions that depend
   on the assumed state.
4. Mark every result already derived from the assumed state as untrusted: prior
   balances, confirmation counts, and mined work computed during
   `snapshot_loaded` / `validating_history` MUST NOT be presented as valid. They
   are recomputed only after a clean re-sync, never silently retained.
5. Emit a high-severity log entry containing:
   - snapshot base height
   - snapshot base hash
   - expected commitment
   - recomputed commitment
   - snapshot path or asset identity
6. Expose the mismatch through RPC as machine-readable state.
7. Require explicit operator reset before using another snapshot or reverting to
   from-genesis sync.

The daemon MAY continue running in a restricted diagnostic mode, but it must not
advertise the chainstate as usable or fully validated.

## Stall Semantics

Background validation is mandatory for fast-default nodes. It must not silently
remain incomplete forever.

The daemon MUST track:

- `last_progress_height`
- `last_progress_time`
- `target_height`
- `missing_body_count`
- `last_error`
- peer/download diagnostics sufficient to explain why progress stopped

If no block is validated for `assumeutxo_bg_stall_timeout` while
`current_height < target_height`, transition to `validation_stalled`.

Default stall timeout:

- server/operator node: 30 minutes
- mobile NodeCore: product-specific, but must be visible in app state

Leaving `validation_stalled` requires actual progress: at least one additional
historical block validated, or an explicit operator reset.

## RPC Contract

`getsnapshotbootstrapstatus` or its replacement must expose at least:

```json
{
  "assumeutxo_active": true,
  "history_validation_state": "validating_history",
  "history_fully_validated": false,
  "snapshot_base_height": 33048,
  "snapshot_base_block": "...",
  "current_validation_height": 12000,
  "target_validation_height": 33048,
  "progress_percent": 36.31,
  "last_progress_time": "2026-06-09T12:00:00Z",
  "stall_seconds": 0,
  "missing_body_count": 0,
  "fatal": false,
  "fatal_reason": "",
  "next_action": "Background validation in progress."
}
```

Required booleans:

- `history_fully_validated`: true only in `fully_validated`.
- `fatal`: true only in `fatal_mismatch`.
- `assumeutxo_active`: true while the node still depends on assumed state.

Display strings are not enough. Wallets, payment processors, miners, and
operator dashboards need boolean gates.

## UI Contract

User-facing surfaces should use four visible states:

- `Fast bootstrap active`
- `Background validation: X / N`
- `Background validation stalled`
- `Fully validated`

Fatal mismatch must not be represented as a normal sync error. It should be a
red, blocking safety alert.

## Persistence

The current state, base height, base block, validation progress, stall metadata,
and fatal mismatch reason must survive restart.

On startup:

- `fully_validated` remains fully validated only if the durable retirement
  marker is present and the chainstate still matches it. If the marker is present
  but the chainstate does not match (corruption or tampering), the node MUST NOT
  trust the marker: it transitions to `fatal_mismatch` and follows the fatal
  path. A present marker is never sufficient on its own.
- `validating_history` resumes from the last durable progress marker.
- `validation_stalled` remains stalled until progress resumes.
- `fatal_mismatch` remains fatal until explicit operator reset.

## Operator Reset

Fatal reset must be explicit and auditable. Acceptable reset paths:

- delete the datadir and start over
- run a dedicated RPC/CLI command that requires a confirmation token and writes
  an audit log entry

Reset must clear:

- assumed UTXO state
- snapshot metadata
- fatal state
- partial background-validation state

Reset must not silently mark the prior snapshot valid.

## Implementation Notes

The current `BackgroundValidationStatus` enum has:

- `NotStarted`
- `InProgress`
- `Completed`
- `Failed`

This is too coarse for the stable fast-default model. The implementation should
separate at least:

- not started
- snapshot loaded / assumed
- validating history
- stalled
- fatal mismatch
- fully validated

The current background validation path must also stop treating unavailable
historical block bodies as skippable success. Missing bodies must keep the state
in progress or stalled; they cannot contribute to `fully_validated`.

## Required Tests

All five tests run on a small deterministic synthetic chain (regtest-style
fixture: a few dozen blocks with the snapshot base at a low height), not a
mainnet replay. This keeps them runnable as CI unit/integration tests. A real
mainnet from-genesis run is a separate release gate (see Release Gate, item 2),
not one of these tests.

### 1. Poisoned Snapshot Is Fatal

Setup:

- simulate a compromised/buggy binary by injecting an anchor whose committed
  UTXO/state value passes load-time verification against a poisoned snapshot,
  but does not equal the true genesis-to-base state of the fixture chain (i.e.
  load-time gates pass, genesis replay will not match)

Expected:

- background validation enters `fatal_mismatch`
- `history_fully_validated == false`
- `assumeutxo_active == true` or the node is in restricted fatal mode; it must
  not silently clear the trust marker
- RPC exposes `fatal=true`
- logs contain the expected and recomputed commitments
- restart preserves `fatal_mismatch`

### 2. Missing Historical Bodies Cannot Complete

Setup:

- load a valid snapshot
- make at least one pre-snapshot historical block body unavailable

Expected:

- background validation does not enter `fully_validated`
- state remains `validating_history` until the stall timeout
- after timeout, state becomes `validation_stalled`
- `history_fully_validated == false`

### 3. Stall Is Loud And Recoverable

Setup:

- pause all historical block-body progress before the snapshot base
- wait for `assumeutxo_bg_stall_timeout`
- then restore serving peers / body availability

Expected:

- state transitions `validating_history -> validation_stalled`
- RPC exposes stall metadata
- when a new historical block validates, state transitions back to
  `validating_history`
- if validation reaches the base and matches, state becomes `fully_validated`

### 4. Good Snapshot Retires Trust Marker

Setup:

- load a valid snapshot
- provide all historical blocks from genesis to base

Expected:

- background validation replays through `snapshot_base_height`
- recomputed commitment matches loaded snapshot commitment
- state becomes `fully_validated`
- `assumeutxo_active == false`
- `history_fully_validated == true`
- restart preserves fully validated state

### 5. Fatal State Requires Explicit Reset

Setup:

- enter `fatal_mismatch`
- restart daemon
- try to load a new snapshot without reset

Expected:

- daemon refuses the new snapshot
- RPC still reports `fatal=true`
- explicit reset clears fatal state and permits a new sync attempt

## Release Gate

Fast snapshot bootstrap may be the server/operator default only after:

1. All required tests above pass in CI or a documented integration gate.
2. Fresh from-genesis background validation reaches the snapshot base on a clean
   node under normal mixed fleet peer conditions.
3. The RPC contract exposes machine-readable `history_fully_validated` and
   `fatal`.
4. Release notes document the temporary snapshot trust anchor and the eventual
   full-validation retirement state.
5. Release binaries are signed/notarized through the normal release process.

Until these gates pass, fast snapshot bootstrap remains a useful operator/mobile
path, but should not be described as fully validated at startup.
