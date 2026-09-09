# Shielded integration cryptographic and consensus review

Reviewer: Codex, designated by the user on 2026-09-09. The reviewer also made
implementation changes. This is an implementation-agent review, not an
independent third-party audit, formal proof, or network activation approval.
An external audit remains an additional assurance option; it is not being
represented as completed by this report.

## Final designated-review disposition

**Approved as a dormant development candidate**, reviewed runtime
`ce100c9867c1c410c22e7147d1fb848e4e727a04`, with the test-only follow-up
recorded in the [final verification](SHIELDED_INTEGRATION_FINAL_VERIFICATION.json).
All twelve runtime CI workflows and all nine test-follow-up workflows pass.
Named coverage accounts for 574 executed
passes and one explicitly unrun external opt-in soak. Strict five-node IBD
converges at height 3022, and the corrected full release meta-suite passes,
including Phase 2 shielded recovery and restart/churn. Artifact builds and proof
resource qualification pass with the scope and hashes recorded in the manifest.

No unresolved implementation blocker was found in this review's scope after
the recorded repairs. This is not a claim of absence of vulnerabilities.
Production activation is not approved: minimum-host/fleet measurements, canary
compatibility, migration/empty-pool evidence and burial/height decisions remain
as documented in the rollout plan. Historical lossy undo must be regenerated
where deep historical rollback is required. These results do not establish the causes of #709/#717. The discovery notes below retain the intermediate evidence; this
final disposition supersedes their then-pending verification statements.

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

The final candidate verification and proof-component qualification pass.
Production activation remains withheld for the fleet and protocol decisions
listed above. Green reruns of #709/#717 cannot be substituted for a convergence
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


## Checkpoint-recovery anchor rewind

The frozen `a7c23d3e7` sweep also failed
`CsnContaminatedCheckpointRecovery`: after detecting a checksummed but stale
Utreexo checkpoint at height 260, recovery rewound the shielded tree and
nullifiers to genesis but retained the high-tip anchor history. DNRS rejected
block 1 with `anchors_bytes=3608`. This is a separate omission in the existing
startup/self-heal rewind helper, not an intermittent convergence failure.

The helper now restores the next block's pre-block anchor undo before replay;
at genesis (or before pool activation) it uses an empty history. Missing or
malformed historical anchor undo at an active non-genesis height fails with an
explicit reindex requirement instead of guessing from an exhausted journal.
The existing persistence and tip-marker workflow is retained; this change does
not claim to redesign its multi-write crash protocol or the recovery of
nullifier state across historical epoch resets.

The recovery regression remains DNRS-active and now compares the full shielded
root as well as tip, Utreexo commitment, roots and leaf counts. It passes both
the offline contaminated-checkpoint repair and a second offline restart. The
realign, block-validation, reindex-anchor and shared shielded-section suites
also pass. Evidence: `/tmp/shielded-checkpoint-anchor-fixed.log` and
`/tmp/shielded-checkpoint-focused.log` on the review host.

## Local header-rate-drop recovery

The frozen e792 sweep failed `CsnBridgeAssistedSpendFlow` at its single
refresh block. The CSN log shows its requested headers response discarded by
the existing 32-message/second receive limiter. The global header request
remained reserved for the 15-minute peer timeout, blocking subsequent refresh
probes. The silent-drop block is byte-identical on main plus the epoch fix and
e792. Five ordinary daemon reruns passed on each binary; no failing baseline
daemon run or resolution of #709/#717 is claimed.

The service now notifies header sync of the local discard. Atomic cancellation
returns a retry hint only for the matching nonzero request owner. It does not
insert headers, penalize the peer, weaken the rate limit, cancel another peer's
flight or send immediately. The existing coalescer schedules one retry after
the receive window, retaining subsequent INV hints. A deterministic no-op model
of the original drop failed the release-ownership assertion; the implemented
hook passes, including unrelated/duplicate drops, no-owner sentinel, unchanged
peer health and subsequent requests. The retry-window coalescer test passes.
Daemon and final matrix verification of this additional repair is tracked in
PR #720; the e792 artifact hashes remain evidence of that predecessor only.

The strengthened e792 strict IBD run adopted the competing branch on all three
consumers. A returned to the source branch, but B/C stopped with one missing
body; no DNRS root mismatch appeared. This is not a passing run. Its previous
45-iteration cutoff did not guarantee the scheduler's 90-second recovery
interval and mislabeled iterations as seconds. The harness now uses elapsed
seconds, allows a 150-second quiet interval and 180-second progress extension,
and retains its overall deadline. Final diagnostics include download state.
The next run must demonstrate actual convergence; the timing correction alone
does not close the failure.

## Stored side-branch body retained in unreadable quarantine

Tracing the final missing bodies from the e792 strict run identified a second
concrete P2P recovery defect. B's source-branch body at height 1517 and C's at
1737 had been read too early during activation and marked unreadable. Both were
later received, checked against their headers, written to flatfiles and given
persisted metadata, but their quarantine markers remained. They were below the
active competing tip, so the ordinary scheduler drain that cleared the marker
never ran. Header-branch import kept excluding the already stored bodies.
The timeout adjustment alone cannot repair this state.

`PersistStoredBodyPosition` now serializes publication with activation and
clears a quarantined hash only after a successful strict archival read verifies
the requested block hash. This covers both existing metadata and newly created
header-selector metadata; failed storage reads and wrong-hash positions stay
unusable. It changes readability, not consensus validity or active-chain state.
The additional read is restricted to quarantined hashes. Both production callers
invoke this outside the scheduler mutex; the recursive activation lock also
prevents a stale failed read from marking a newly repaired body afterward.

The original publication function is identical on main plus the epoch fix and
e792. The existing pre-base persistence test, extended to stage quarantine,
fails before the repair and passes after it. Existing-row repair, replacement
repair, and a physically present wrong-hash negative control pass. The running
rate-drop-only IBD attempt was intentionally retired after this finding, not
counted as a pass. Final daemon verification must include this repair.

## Release meta-suite ancestor fixture

The first ce100 ReleaseSuite passed parity, P2P storm, release-profile IBD,
Utreexo and canonical recovery, then failed the mempool ancestor test. The
fixture reused a funding outpoint selected before the burst phase, directly
cleared the pool without reconciling wallet spend state, and expected automatic
wallet selection to construct an unconfirmed chain. Separately built main plus
the epoch repair also fails the original fixture after one accepted spend; the
candidate failed before the first. This is not an Auth runtime regression.

The corrected fixture settles prior traffic through mined blocks, chooses a
current confirmed input, constructs/signs explicit single-parent transactions,
and verifies the dependency and rejection boundary. The existing policy admits
26 transactions (the last has 25 predecessors), then rejects the 27th for
26 ancestors; rejected admission leaves pool size unchanged. Both unchanged
main-baseline and candidate binaries pass all six checks. Cleanup now targets
only the owned daemon PID. The original failed meta log is retained and a full
meta rerun is required; no daemon rebuild or runtime change is involved.

## Proof-mix qualification gap closed

The initial eight-proof benchmark used balanced transfers only. The review
identified that spend proofs cost more, so a single proof-count case did not
qualify all relevant extremes. Test-only commit 25fe32473 adds eight independent
Auth unshields and eight shields with distinct spends/outpoints, verifies exact
mix counts and cross-transaction nullifier uniqueness, and applies the same
30-second budget to all block shapes. Expanded 24-process matrices pass on Mac
and Linux. The final manifest and hardware table record the slower all-spend
mix and retain the original narrower reports; no consensus or daemon limit
changed to obtain these results.
