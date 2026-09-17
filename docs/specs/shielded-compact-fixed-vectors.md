# Compact regtest fixed vectors and resource boundaries

This qualification layer consumes saved randomized proofs. It does not change
the accepted format, consensus rules, activation settings, wallet behavior or
Utreexo implementation. `DINERO_ENABLE_COMPACT_REGTEST` remains default-off.

## Corpus and provenance

`tests/vectors/compact_regtest_v1/manifest.json` pins transaction bytes,
expanded bundle bytes, hashes and state expectations. The source implementation
is commit `f18ac667ad0b4e7ebfdbdb59db297f8a33b24318` (PR #760).

| Case | Spends / outputs | Wire bytes | Source |
| --- | --- | --- | --- |
| shield | 0 / 1 | 27,906 | Three-node regtest, mined at height 124 |
| transfer | 1 / 2 | 97,094 | Same chain, mined at height 125 |
| unshield | 1 / 0 | 42,053 | Same chain, mined at height 126 |
| maximum | 4 / 2 | 222,747 | Separate synthetic prestate, generated and fully verified locally |

The unshield also includes its captured Utreexo inclusion proof and forest roots.
The first three cases retain their preceding shielded commitments and nullifiers.
The maximum case uses a separate four-note prestate; it is not represented as a
mined transaction or as an extension of the other three cases.

Proof randomness is captured once. CI never regenerates a proof or rewrites
expected values. The excluded-from-default-build maintenance target
`capture_compact_regtest_maximum` can generate a replacement maximum fixture in
a new directory, using only the public test seed in its source. The Python
checker has an explicit `--record` mode for reviewed fixture maintenance; CI
does not invoke it.

## Two consumers

`scripts/check_compact_regtest_vectors.py --self-test` independently parses the
transaction envelope and bundle, expands only the two fixed proof layouts, and
reconstructs signing preimages, txid/wtxid, byte/weight accounting, commitment
tree roots and transparent-output Utreexo leaves. It uses Python's standard
library and the existing independent Python Poseidon implementation. It does
not call the C++ codec or verifier and does not verify Spartan cryptography.

`CompactRegtestFixedVectors` uses the production C++ implementation to:

- Decode and reserialize every saved transaction exactly.
- Match identity, signing hashes, proof and expanded-bundle hashes, and complete
  expanded bundle bytes against the saved Python expectations.
- Verify every real proof with its captured prestate, including the maximum
  four-spend/two-output shape.
- Reject false public claims after successful verification and reject the same
  valid bundle below activation, without changing commitment/nullifier state.
- Apply valid shielded state transitions and match independently computed roots.
- Reproduce each transparent-output Utreexo leaf, verify the captured unshield
  inclusion proof, and reject a changed leaf. An expanded validation view must
  have a different txid and cannot supply the original output's leaf identity.
- Account for every spend/output proof slot despite the smaller encoding.

The Python negative controls cover nonminimal/truncated length encodings,
wrong fixed proof length/profile/dimensions/circuit hash, transaction truncation
and trailing bytes, altered expected identity/expanded bytes/shape, and an
altered Utreexo leaf. These are byte-oracle checks, not an independent
cryptographic security review.

## Resource boundaries

The compact-enabled `ShieldedResourceLimits` target exercises:

- 511,999 / 512,000 / 512,001 serialized transaction bytes, including the real
  stateless relay envelope and activation boundary.
- Maximum four spends and two outputs, and each count plus one.
- Seven / eight / nine proof slots per block, including an eight-slot mixed
  shape followed by an additional proof.
- 999,999 / 1,000,000 / 1,000,001 aggregate shielded bytes per block.
- Unchanged counters after rejection, including invalid `SIZE_MAX` counters.

Resource-only fixtures intentionally use placeholder proof payloads. Passing
that cheap gate is not cryptographic acceptance. The fixed-vector target
separately validates the actual proofs.

For an empty-transparent-input compact transaction, the mandatory two-byte
witness marker makes weight `4 * wire_bytes - 6`. The byte limit therefore binds
before the 2,048,000 weight limit. There is no independently reachable exact
weight-limit case for that shape; the tests pin the actual accounting instead.

These tests do not qualify actual ancestor/descendant package admission at the
600,000-byte/25-transaction limits. Existing tests pin the package constants;
an end-to-end package boundary test remains separate work. Nor do these tests
raise the existing eight-proof block limit or establish a proportional increase
in mined shielded throughput.

## Execution and evidence

The new `Compact regtest fixed vectors` workflow runs identical fixture bytes on
Linux x86-64 and Linux ARM64. It records architecture, source commit, fixture
SHA-256s and CTest results. Both compact workflows use the same named ON
configuration for the mandatory-test coverage check, compared with an OFF
inventory. All three ON-only tests have executing lanes; no exemption is added.

Local macOS ARM64 qualification:

- `ShieldedResourceLimits`: 14 GoogleTest cases passed.
- `CompactRegtestFixedVectors`: 2 GoogleTest cases passed, consuming four real
  transactions and all their proofs.
- `CompactRegtestVectorOracle`: all four fixtures plus six negative-control
  cases passed.
- Combined CTest: 3/3 passed in about 24 seconds.
- Deliberately replacing the shield fixture's expected txid made both the Python
  and C++ consumers fail. Restoring the manifest byte-for-byte restored green.
- Mandatory execution check: 3 registered ON-only tests, 3 executed, 0 omitted.

Linux results must come from the new workflow on this branch; the successful
parent lifecycle run is not a substitute for these architecture checks.

Separate remaining gates include instrumented daemon/wallet qualification,
independent format/consensus review, pool-protocol qualification and a reviewed
activation plan. This corpus and its results do not authorize production use.
