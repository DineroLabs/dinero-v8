# Shielded integration cryptographic and consensus review

Reviewer: Codex, designated by the user on 2026-09-09. The reviewer also made
implementation changes. This is an implementation-agent review, not an
independent third-party audit, formal proof, or network activation approval.
An external audit remains an additional assurance option; it is not being
represented as completed by this report.

## Scope and method

Reviewed recipient key derivation and note ownership, Auth spend R1CS and
transcript/version selection, output value binding, range proofs and binding
signatures, envelope recovery checks, resource gates, snapshot load ordering,
selected-header burial, coinbase root prediction, live/reindex enforcement and
epoch undo. Read actual call paths and exercised the existing adversarial,
external-vector, persisted reorg and Gate D tests. The vendored cryptographic
primitives and proving-system security assumptions are dependencies; this review
does not establish their security from first principles.

## Findings and disposition

| Finding | Disposition/evidence |
|---|---|
| Epoch reset dropped anchor eviction history, so deeper rollback/reconnect diverged | Fixed by full persistence capture/restore. Minimal regression demonstrated red before the repair; persisted restart/reorg/reindex lifecycle passed. Existing lossy undo still requires historical reconstruction. PR #719 isolates the existing-main repair. |
| Snapshot v5 authentication occurred after live import | Fixed: staged decoding and binding/burial validation precede forest import, lifecycle publication and metadata writes. Six checksum-valid mutations and five controls pin rejection and unchanged state across restart. Arbitrary storage-fault atomicity after valid import is outside that claim. |
| Auth nullifier domain had previously been only a witness assignment | Prior implementation fixed an explicit constant constraint; direct witness mutation pins it. |
| Auth address domain remained only a witness assignment | This review added `auth_address_domain` to the dormant Auth circuit and a direct witness-mutation regression. All ten Auth tests pass, including valid proofs, wrong spend/view keys, odd-y and zero-scalar rejection, and profile downgrade rejection. This is a constraint-completeness finding, not a demonstrated theft. Historical spend/output circuits remain byte-compatible. |
| GBT advertised mutations incompatible with Utreexo/DNRS commitments | Empty mutable list, explicit DNRS root/script/index metadata, and canonical reference-miner behavior. New external-mining test checks actual accepted post-state equality. |
| Coordinator TODO identified as a missing live assembler | Source-list and call-path audit found an uncompiled obsolete prototype, including a reference to a removed context member. Retired it and its unused workers/controller. The supported immutable `mining.getjob`/`mining.submit` path passes a solved-block/restart test. |
| Snapshot membership did not explicitly require the carried transaction to be a coinbase | Added an `IsCoinbase` check after Merkle verification. A red-before/green-after regression covers wrong outpoint index, nonzero previous transaction and missing input even when membership matches. Seven binding-helper tests pass. |
| Consensus checksum omitted shielded activation/reset settings | Added all eight shielded heights and mutation coverage to operator drift telemetry. The pinned mainnet checksum changes; block and peer protocols do not. |
| GCC build relied on a transitive standard-library include | Explicit `<algorithm>` added; the missing `std::any_of` declaration was the common failure in the first Linux CI runs. |

The strict IBD sweep also found a pre-existing header continuation defect.
A separately built main control failed after repeated genesis..2000 batches.
A minimal side-branch regression failed before the repair: a valid 2,000-header
batch did not yet exceed the local best-work tip, so the next locator repeated
the local tip instead of the received frontier. Per-peer validated continuation
now advances that frontier while retaining serialized request ownership and
normal header validation. All four rebuilt header-sync suites pass. The strict
five-node scenario passes without manual withheld-block replay: S/A/B/C agree at
height 3022 after the post-heal block; F remains the competing-fork source and
is intentionally outside the harness's final convergence set. This finding does
not establish the cause of the separate #709/#717 intermittent tests.

Additional full-sweep findings: explicit fractional fee-rate conversion truncated
positive rates to zero (reproduced on main and corrected with checked rounding);
the TwoNodeSync harness killed another test's daemon through broad process-name
matching (replaced with owned child-PID cleanup); the macOS dashboard harness
assumed HOME-based datadir/cookie discovery (replaced with explicit isolated
paths and ports). These are separately scoped from shielded cryptography.

## Security properties checked

- Only `ask` opens the diversified spend key. Public `ak` and diversifier derive
  the matching public key; viewing material derives nullifier keys, not spend
  scalars. The ownership commitment binds both the spend public key and the
  nullifier-key commitment. Auth database rows do not persist spend scalars.
- The Auth circuit checks non-identity, even-y normalization, the ownership
  opening, Merkle path, leaf-index nullifier, 64-bit note value, and equality
  between that value and the Pedersen commitment. Its 0x06 profile cannot be
  accepted as legacy/CV-only, nor can those weaker proofs satisfy Auth rules.
- Range proofs and the binding signature fail closed when required generators
  are unavailable. Binding includes transaction context, value balance and
  value commitments. Historical activation behavior is preserved.
- Larger relay envelopes are bounded before parsing and granted only to v6;
  contextual bundle/count limits and block proof/byte limits still apply.
  Aggregate limits are checked before expensive verification/state mutation,
  and rejected package accounting does not consume or evade block budget.
- DNRS encodes raw internal root bytes; RPC displays reversed uint256 hex.
  Exactly one canonical commitment is required. The external test checks bytes
  against the displayed root and against actual connected state.
- Snapshot binding checks selected-chain ancestry/burial, merkle linkage to the
  header and full SHR1, including anchors/nullifiers, before publishing state.
  Dormancy uses the explicit sentinel predicate, not a bare height comparison.

## Review conclusion and release limits

The corrected code is suitable to continue candidate verification. Sign-off for
production activation is withheld until the exact candidate's full and release
matrix results, resource qualification and fleet compatibility evidence are
recorded. Green reruns of #709/#717 cannot be substituted for a convergence
root cause. The user-selected reviewer role does not supply absent hardware,
fleet or release evidence. Mainnet/testnet activation remains dormant.

See [rollout plan](../specs/shielded_upgrade_rollout.md),
[Gate D evidence](SHIELDED_GATE_D_VERIFICATION.md), and
[integration status](SHIELDED_INTEGRATION_STATUS.md).


## Additional deep-reorg finding (2026-09-09)

The stronger IBD scenario requires all three consumers to adopt the competing
fork before healing to the source chain. On candidate `a7c23d3e7`, it exposed a
second anchor-undo defect: an ordinary disconnect beyond the 100-entry eviction
journal exhausted the history. Reconnecting at height 601 computed a different
DNRS root with only one anchor (`anchors_bytes=44`). The already-landed epoch
reset repair does not solve ordinary deep rollback. The bounded ordinary undo
mechanism also exists on main.

New undo records capture the pre-block anchor persistence envelope alongside
the frontier, including for empty blocks. Both undo codecs, live/stateless
conversion and disconnect paths, replay, and reindex preserve it. The shared
disconnect validates the envelope before mutation. Failed DNRS apply restores
ordinary and reset state instead of leaving a rejected anchor published.
The consensus anchor window and SHR1 root bytes are unchanged. Storage grows
by at most 7,217 bytes per newly captured undo record (7,212-byte envelope plus
its optional flag and length). Old records remain readable but cannot supply
history they never stored; replay/reindex remains required for historical deep
rollback. A regenerated record must be reconstructed at its actual active tip.

The minimal 350-block regression failed without the snapshot and passes when
disconnecting to height 120 after a persistence round trip. Codec compatibility,
truncated-trailer rejection, malformed-anchor no-mutation, rejected ordinary/reset
apply restoration, and reindex snapshot checks pass in the three focused suites.
The stronger daemon scenario and final combined verification are pending; this
finding must not be treated as closed solely from the unit tests.
