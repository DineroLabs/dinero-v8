# Recipient-bound shielded authority: implementation review

Branch: `codex/recipient-bound-shielded-authority`, based on `be80b4b7e`.
This is an implementation review of an uncommitted development branch, not an
independent cryptographic sign-off or an authorization to activate the network.

## Purpose

The historical wallet derives a note's spend material from randomness known to
the sender. The abandoned dormant replacement derived it from the incoming
viewing key, which would also grant spending to a viewer. This branch replaces
that authority model before activation:

- Only `ask` derives the diversified spend scalar. Public `ak` and the
  diversifier derive the corresponding public spend key.
- An independent `nvk` derives the note's nullifier key, allowing a full viewer
  to track spends without deriving a spend scalar.
- The note commits to both the recipient spend public key and the nullifier-key
  commitment. The Auth spend circuit proves the spend-key relation and opens
  that nullifier commitment. Its proof version is `0x06`.
- The 107-byte address carries the diversifier, encryption public key, spend
  public key, and nullifier-key commitment.
- The 757-byte outgoing envelope v3 lets the sender recover recipient, amount,
  and memo, authenticating them back to the ciphertext and note commitment.
  Knowledge of the recovered randomness does not authorize an Auth spend.

Spend authority and outgoing recovery remain dormant on shipped networks.
The regtest override rehearses their paired activation after a distinct reset.
This branch does not activate mainnet or remove its fund-moving RPC lockout.
It does replace incomplete 43/75-byte wallet addresses with the 107-byte
authority-complete form, so the wallet-format migration is visible even while
the consensus and outgoing-envelope rules remain dormant.

## Wallet lifecycle

Viewing caches survive `wallet.lock`, are populated before first encryption
erases the seed, and are cleansed on unload. An encrypted wallet needs one
unlock after process restart to restore those in-memory viewing caches.
Persisted outgoing history remains readable while locked.

Live scanning and historical rescan share note recognition. Recipient-bound
rows retain diversifier, commitment, randomness, and nullifier-view material.
Spending re-derives authority from the unlocked seed and checks it against the
stored ownership and nullifier key. Outgoing records move from provisional to
confirmed, are removed on disconnect, and are recomputed on reconnect.

## Findings corrected in this continuation

1. **Auth spend scalars were persisted in plaintext SQLite rows.** Notes
   discovered while unlocked and locally created change took this path.
   Auth insertions now write a zero placeholder. Runtime spending always
   hydrates from the unlocked seed rather than trusting cached spend material.
   Legacy overloads reject Auth so they cannot mirror the spend scalar into
   the nullifier column. Opening an earlier development database retires its
   cached Auth scalar in live rows; historical pages/backups are not claimed
   to have been securely erased.
2. **Nullifier migration could confuse spending and viewing authority.** A
   missing legacy nullifier key can be backfilled from the legacy spend key;
   a missing Auth key cannot. The migration now makes that distinction and
   fails closed for missing Auth viewing material.
3. **The new nullifier domain was only a witness assignment.** `R1CS::alloc`
   does not constrain its argument to a constant. The dormant Auth circuit
   now explicitly constrains its nullifier-key domain tag; a regression
   mutates that witness and checks rejection by the constant constraint.
4. **Template selection starved valid shielded transactions.** Two lifecycle
   attempts mined a coinbase-only block while the accepted shield remained in
   the mempool. Retained logs identified `timeout during ancestor scoring
   after 0 of 1 txs`: proof revalidation consumed the two-second selection
   budget before the transaction was scored. Selection now checks the budget
   before validation and can finish one validated package after the deadline.
   It retains all validity, size, and weight checks and the existing timeout
   for subsequent packages. The change is in mempool selection, not Claude's
   coinbase assembly paths.
5. **The lifecycle harness discarded subshell failure evidence.** Cleanup now
   retains the datadir and daemon log based on the parent exit status. The
   test also asserts initial shield confirmation explicitly and uses a fixed
   fee to avoid duplicating fee-autosizing coverage and proof work.
6. **Successful recovery and self-output construction left secondary secret
   copies to ordinary stack reuse.** Type-owned destructors now cleanse
   outgoing `ovk`/`esk`/`rcm`, recognized note plaintext, and locally derived
   recipient spend/nullifier material on every return path. Auth spend scalars
   remain ephemeral and are never stored in Auth database rows.
7. **A realistic Auth transfer exposed an existing size-policy boundary.** A
   one-spend/two-output transfer serialized to 167,935 bytes and was refused by
   the 101 KiB ancestor-package rule. A coordinated, dormant resource profile
   now covers raw P2P parsing, download, mempool/package admission, orphan
   holding, wallet construction, template selection, block connection and
   reindex. Only tx-v6 can use the larger envelope because it commits the
   bundle into txid; tx-v5 remains historical before activation and is rejected
   after it. Transaction, package, block-byte and aggregate proof boundaries
   are independently pinned. No historical block validity changes.

## Verification

Fresh local checks:

| Check | Result |
| --- | --- |
| Shielded derivation | 30 tests passed |
| Protocol external vectors | 13 tests passed |
| Python protocol and outgoing oracles | Both self-tests and pinned fixtures pass |
| Note-store persistence/migration/rollback | 14 tests passed |
| Encryption and unload viewing-authority regression | 1 test passed |
| Chain parameter selection | 7 tests passed |
| Outgoing recovery, parser, hardware adapter, emission | 10 tests passed, 184 seconds |
| Prover-kit ABI | 15 tests passed |
| Complete circuit suite | 24 tests passed, 375 seconds |
| Activation-sentinel spend-authority boundary | 1 test passed, 115 seconds |
| Mempool/covenant activation, restart and reorg | 4 tests passed |
| Daemon outgoing-recovery lifecycle | Passed: locked discovery, two restarts, disconnect/reconsider, recipient-authorized spend |

The daemon and relevant test targets build successfully. `git diff --check`
and the lifecycle shell syntax check pass. This is focused verification, not
the entire repository test suite or a physical-device test.

Previous task logs separately recorded 30
derivation, 13 protocol-vector, 8 outgoing-view, 15 prover-kit, 9 note-store,
22 circuit, and 43 validation tests passing. Those older runs predate the
corrections above and are not substituted for their regression checks.

## Resource-boundary finding and resolution

The original failing lifecycle attempt ran for 577 seconds. The mining-selection fix
allowed the initial shield to confirm, with its Auth spend-secret field zero
in SQLite. The addressed transfer then produced this admission result:

```text
reject_code: ancestor-size-limit-exceeded
reject_reason: Ancestor size 167935 bytes exceeds limit 103424 bytes
vsize: 167934
fee_una: 1000000
fee_autosized: false
wallet_rollback: complete
```

This was one confirmed shielded input, a 70,000,000-una recipient output, and
change. It had no unconfirmed transparent ancestors. The failure is therefore
not convergence, an insufficient fee, a timeout, or Claude's Gate E work.

The original mismatch extended beyond this first rejection:

- `src/daemon/mempool.cpp` included the transaction itself in its 101 KiB
  ancestor-package size limit.
- `src/p2p/structural_validator.cpp` rejected raw transactions larger than
  `consensus::MAX_TX_SIZE` (100,000 bytes) and weight exceeding
  `consensus::MAX_TX_WEIGHT` (400,000).
- `src/p2p/download_coordinator.cpp` also applied a 100,000-byte transaction
  download cap.
- `Mempool::validateTransaction` measured `Serialize().size()/2`
  even though `Serialize()` returns bytes. This explains why the transaction
  reaches the later ancestor check; that behavior must not be relied on as
  permission to relay oversized transactions.

The existing bundle-limit unit test compares proof size against the one-MB
block cap; it does not establish transaction-level relayability. Passing the
cryptographic unit suites is insufficient to declare this feature usable.

The branch resolves this with the coordinated dormant profile specified in
`../specs/shielded_auth_resource_profile.md`: 512,000 serialized bytes and
2,048,000 weight for tx-v6 Auth transactions, at most four spends and two
outputs per transaction, eight proofs and 1,000,000 shielded transaction bytes per
block, and a 600,000-byte shielded package ceiling. Stateless parsing grants
the larger allocation bound only to a raw tx-v6 claim; canonical decoding and
contextual admission remain mandatory. At activation, tx-v5 Auth bundles are
refused because their shielded bytes do not participate in txid.

The same predicate is applied at next-block mempool admission, candidate-block
selection, connection and reindex. Wallet builders reject unsupported shapes
before proving and re-check exact bytes and weight afterward. The resource
tests pin both sides of every boundary, historical dormancy and activation
rollback. A two-node lifecycle supplies the consequence test: a real
one-spend/two-output Auth transfer is admitted by an independent peer, mined by
that peer and accepted by the source before the restart/reorg/recovery checks.
A mempool-only exception, test-only admission bypass or timeout increase would
not satisfy that contract.

## Resource upgrade verification

- All five real transaction shapes built and passed complete validation in
  isolated Release processes: shield; one-, two-, and four-input transfers
  with recipient/change; and unshield. The largest was 394,953 bytes.
- The complete Release shielded validation suite passed 44 tests; outgoing
  recovery passed 10; prover-kit passed 15.
- Ten resource boundary tests and six mempool/block lifecycle tests passed,
  including actual tip changes, v6 enforcement and rejection before state
  mutation. Reindex equivalence and block invariants passed.
- P2P structural, download, relay/orphan, CSN proof refresh, and mempool query/
  staleness regressions passed. The orphan pool retains a 10 MB aggregate
  byte budget. Historical v5 block witness semantics have a dedicated
  structural regression separate from standalone relay byte limits.
- The final Release two-node daemon lifecycle passed: source submission, independent
  peer admission and mining, locked discovery, two restarts, disconnect/
  reconsider, and the final recipient-authorized spend mined by the peer.

[Resource profile and measurements](../specs/shielded_auth_resource_profile.md)
record the selected limits, exact byte decomposition, Release timing and RSS,
reproduction commands, and remaining target-hardware qualification. No proof
system has been replaced and no network activation height has been selected.

## Remaining release boundaries

- Claude's Gate E snapshot/enforcement work remains in its own worktree. The
  shared chain-parameter/CMake integration files need a deliberate union when
  the branches are combined. The resource upgrade also touches
  `src/consensus/block_validation.cpp` and `src/consensus/reindexer.cpp`; preserve
  both branches' checks and rerun the combined enforcement/lifecycle suites.
- Hardware tests exercise a capability-gated host adapter with a fake device.
  They do not establish physical Ledger/Trezor firmware support.
- Mobile callers must adopt the explicit `ak`/`nvk` address API and 107-byte
  payload. Retaining the old ABI symbol while failing closed is not a mobile
  release migration by itself.
- The dormant Auth resource profile still needs independent review and
  release-build capacity measurements on each supported target before its
  activation height is selected. Its benchmark is deliberately outside the
  ordinary correctness lane and is reproducible one transaction shape per
  process.
- The external review and activation conditions in
  `../specs/shielded_spend_authority_activation.md` still apply. This local
  review does not establish a security proof for the custom composition.
