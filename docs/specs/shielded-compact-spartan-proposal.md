# Compact ordinary Spartan proofs: prototype and activation proposal

Status: research prototype, 2026-09-16. **No production decoder or activation.**
The native codec is compiled only into test/fuzz executables. The wallet,
existing proof verifier and current compact-format rejection remain unchanged.
A separately tested [batch proof RPC fix](../benchmarks/utreexo-batch-canonical-20260916.md)
uses canonical coins instead of asynchronously updated wallet metadata; it does
not change Utreexo commitments or enable compact transactions.

## Problem and measured result

Ordinary Auth spend (`0x06`) and cv-bound output (`0x04`) proofs serialize every
row of the zero error commitment. The existing verifier requires every such
row to be the identity. We can omit their deterministic encodings and restore
the exact original bytes before verification. This preserves the commitment's
place in the Fiat–Shamir transcript and the zero-error soundness check.

The native prototype extends the [offline study](../shielded-zero-error-encoding-study.md)
with circuit-derived parsing bounds, genuine proof verification, full bundle
validation and a transaction-identity/accumulator experiment.

| Shape | Original serialized bytes | Experimental serialized bytes | Reduction |
| --- | ---: | ---: | ---: |
| Shield, one output | 46,373 | 27,798 | 40.06% |
| Unshield, one spend | 75,714 | 42,021 | 44.50% |
| Transfer, one spend/two outputs | 167,935 | 97,094 | 42.18% |

These are freshly built, serialized **experimental transactions**, including
actual length-prefix changes. They are rejected by current consensus. Fees
were held fixed; these are not paid-fee measurements, accepted network
transactions, or demonstrating the speed of proof generation. Full bundle
verification succeeds only on the separate, exactly reconstructed proof view.

## Native codec contract

`contrib/benchmarks/compact_spartan_codec.{h,cpp}` uses the study's `DZE1` file
tag. It is not an assigned proof version. Its payload retains the inner
historical profile byte and every original field except the `comm_E` row-point
bytes. There is exactly one representation per supported original byte string.

The caller supplies a trusted verifier circuit and historical profile. Before
allocating an expansion buffer, the codec checks:

- Exact input length, profile and file tag.
- Both Hyrax matrices' row count, column count and total, derived from the
  circuit's variable and constraint counts using the existing integer rules.
- The expected circuit structure hash, both sumcheck counts and both IPA
  counts, including legitimate zero-round sections.
- No trailing data. Packing also requires every omitted byte to be zero.

The prototype bounds each trusted circuit count at `2^22` before computing
layout sizes. That is an experimental implementation ceiling, **not a new
consensus constant**. Wire-supplied dimensions never set allocation sizes.
Unsupported versions, private-covenant proofs and nonzero-error relaxed proofs
are excluded. This byte codec is not a cryptographic verifier: retained-field
tampering remains visible to the existing verifier and must still fail there.

## Transaction identity and format authorization

Auth v6 includes the bundle bytes in txid. Compact and expanded transactions
therefore have different txids even when they prove the same shielded action.
An unshield's transparent output must use the **final compact transaction's**
txid. Expanding the transaction before hashing would produce the wrong outpoint
and the wrong Utreexo leaf.

There is another reason not to make both encodings interchangeable inside v6:
the binding signature covers the transparent envelope, transaction version,
value balance and value commitments; it does not cover proof serialization.
Accepting repacked legacy proofs with the same version/signature would let a
third party change an unconfirmed unshield's txid and invalidate its pending
child. The prototype demonstrates this binding distinction explicitly.

**Proposed activation design:** introduce a separately signed transaction
version for compact ordinary shielded transactions. The version number and
activation height are deliberately unassigned in this prototype. Require one
canonical compact representation in that version; preserve historical v5/v6
decoding and validation. Proof expansion stays inside the proof-verification
view, never in the transaction object, txid, outpoint or accumulator hash path.
The new version's inclusion in binding signatures prevents unauthenticated
conversion between legacy and compact transaction envelopes.

Before implementation, review the version dispatch, signature domain, proof
version assignment, historical acceptance and activation-boundary mempool
behavior together. No node-local flag may change mainnet consensus acceptance.

## Utreexo and resource contract

The prototype checks final compact bytes survive serialization, txid remains
stable through verification, the final unshield leaf differs from the expanded
transaction's leaf, and only the correct leaf verifies against its forest.
It then restores a serialized forest, removes that leaf, verifies it cannot
be proved in the resulting current forest, and restores the prior snapshot.
This is an accumulator unit/integration experiment, **not daemon mining,
disk-restart, real reorg or stateless-node qualification**.

Smaller bytes do not justify more verification work. Existing spend/output
counts and the eight-proof block limit remain independent of byte budgets.
The test fills that block budget with the compact candidate and confirms an
additional candidate is rejected without changing the usage counters.

## Test-first evidence

- Identity/no-op codec: seven of eight initial codec tests fail; both full
  wallet prototype tests fail. The retained-data tamper case already passes
  against the unchanged cryptographic verifier, as expected.
- Native codec: exact reconstruction/repacking, malformed length/profile/tag,
  circuit dimensions, round counts, circuit hash, nonzero error rows and genuine
  relaxed-proof forgery rejection. Every single-byte mutation is either rejected
  structurally or round-trips exactly, with no silent rewriting.
- Negative control: disabling just the omitted-zero-row guard makes both
  nonzero-error/forgery tests fail. The guard is restored for the final run.
- Fresh shield, unshield and transfer: original bundle valid; compact bundle
  rejected under current rules; reconstructed bundle valid under the unchanged
  verifier and original binding signature. A changed transaction version fails
  the original binding signature.

Final local result: **50/50 ShieldedValidation and 13/13 SpartanSoundness**
(including ten compact-codec cases), plus six offline Python tests. UBSan passed
all 13 codec/soundness cases with the codec and test translation units
instrumented; existing crypto libraries were not instrumented. Local ASan did
not reach `main`: a sampled Apple sanitizer-runtime initialization stall forced
termination of that owned test process. No ASan pass is claimed; Linux sanitizer
qualification and broader parser fuzzing remain required.

The tests extend the existing `SpartanSoundness` and `ShieldedValidation`
CTest entries, both already selected by CI. They require no local proof files
or production datadirs. Full native Linux evidence remains a separate gate.

## Required follow-up before production eligibility

1. Cryptographic/consensus review of the concrete canonical format and signed
   transaction version. Review scalar/point canonicality and transcript inputs;
   add adversarial parser fuzzing with memory/undefined-behavior sanitizers.
2. Implement separately activated verification and wallet construction with
   correct fee sizing/signing and a single finalized txid. Keep disabled by
   default until activation is approved.
3. Cross-architecture vectors and activation-boundary tests; old nodes must
   reject the new format predictably, not reinterpret it as historical data.
4. Real full-node/CSN, pool-template/DNRS, relay, restart/reindex and reorg tests.
   Mine the compact unshield and a signed child spending its exact output;
   verify matching commitments, spent-leaf removal and disconnect restoration.
5. Review proof-count CPU budgets, migration/rebroadcast handling and coordinated
   rollout. Choose an activation height only after qualification and owner approval.

## Reproduce

Use the project's usual headless Release configuration, then:

```sh
cmake --build build --target test_spartan_soundness test_shielded_validation -j8
ctest --test-dir build --no-tests=error --output-on-failure \
  -R '^(SpartanSoundness|ShieldedValidation)$'
build/test_shielded_validation --gtest_filter='*CompactPrototype*'
```

The focused wallet tests print `COMPACT_CANDIDATE` size records. Timings depend
on the machine; no wall-clock threshold is used as a correctness assertion.
