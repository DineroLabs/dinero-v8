# Exact block reward checks at admission, proof validation and replay

## Problem and behavior

`BlockAcceptor::ValidateContextual` previously treated 1,000,000 una (0.01
DIN) per transparent transaction as an upper bound on its fee. Fees are the
spent input value minus the output value; transaction size or relay fee policy
does not bound that difference. A daemon-generated template paying 100 DIN
subsidy plus a valid 0.1 DIN fee was rejected by the same daemon.

Admission retains coinbase framing and BIP34 checks. Exact reward accounting
runs with the parent coin view or the supplied spent-output metadata. Every
coinbase output counts, and checked addition rejects overflowing totals.
`block_reward.cpp` provides one implementation of the existing transparent,
shielded and confidential fee semantics to ordinary validation and CSN paths.
It lives in the consensus library so standalone network targets do not gain
a daemon dependency.

Removing the admission estimate requires guarding paths that can update a CSN
forest without first executing normal `ConnectBlockInternal`:

- Forward batch proofs, speculative branch proofs and transition proofs bind
  input values to their targets or actual intra-block outputs, then check
  rewards before the first forest/stump mutation. Existing cryptographic proof
  validation remains required.
- Stored-block replay runs the same reward check before its transactional
  working-copy update and header-root comparison.
- CSN branch activation checks the complete branch's reward metadata before
  rewind. A missing or inconsistent replay record aborts this preflight.

These arithmetic/metadata checks are not a replacement for script, shielded
bundle, PoW, header-root or transaction validation. In particular, the leaf
format binds value/script and applicable maturity fields; it does not bind
`is_confidential` or `commitment`. For supported serialized transactions, the
explicit-fee lane is already determined by shielded version or confidential
outputs. An input flag cannot manufacture an explicit fee on an ordinary
transparent transaction. This patch does not change the leaf format.

## Legacy replay compatibility

A historical hash-only replay record identifies deleted leaves but cannot
recover the amounts used to calculate transparent fees. For a block with
inputs, replay needs complete spent-output metadata from its stored Utreexo
block data or its replay sidecar. Coinbase-only blocks and zero-input unshield
fee accounting do not need such metadata.

Missing metadata is an operational recovery failure, not evidence that the
historical block or transaction is invalid. CSN activation aborts before
rewind; individual replay leaves the canonical forest unchanged. These failures
do not persist an invalid-block flag. The patch does not implement automatic
metadata recovery. Existing databases with affected hash-only records need
separate compatibility qualification and a verified recovery/redownload path
before rollout; there is no guessed-fee or reward-check bypass fallback.

Preflight binds metadata to stored targets but does not independently prove
those targets against the fork forest. Replay retains that transactional
forest/header-root verification. This is not a claim that arbitrary corruption
of all stored branch data can never interrupt a reorg.

## Regression coverage

- `BlockAcceptorFees`: low, 0.1 DIN and 90 DIN admission; retained BIP34 check;
  real signed spends through ordinary stateful and stateless block validation;
  exact rewards accepted and single/split coinbase overpayment rejected while
  funding coins and forest remain unchanged.
- `TransitionProofForestSync`: independently root-correct exact/excess rewards,
  legacy metadata absence/presence, coinbase-only blocks, batch/scratch/transition
  paths, a forged intra-block child amount, read-only reorg preflight, metadata
  cardinality, checked arithmetic, and zero-input unshield fee accounting.
- `BlockAcceptorFeeLifecycle`: real PoW with enforced ASERT; full and CSN nodes;
  0.1 DIN fee and a 90.2 DIN parent/child package; exact coinbase template sums;
  Utreexo proof agreement and consumed-output rejection; invalidation, restart
  and reconsideration restoring the same tips/roots/proofs. DNRS stays enabled.
  This test is assigned explicitly to the serial CI lane.

Local evidence includes failing-before/passing-after runs and behavioral
neuters. The local daemon diagnostic links freshly compiled modified objects
with cached Release dependencies; it is not a clean release artifact. Linux CI
must build this branch from source and run the registered checks before merge.
The combined migration/compact/60-second release has further independent gates.
