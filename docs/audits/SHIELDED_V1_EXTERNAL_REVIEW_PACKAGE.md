# Dinero shielded v1 external-review package

**Review target:** the deployed Dinero shielded v1 consensus protocol described
in [`shielded_protocol_v1.md`](../specs/shielded_protocol_v1.md).

**Review status:** ready for independent cryptographic and consensus review.
This package defines the assignment and evidence expected from the reviewer. It
does not constitute that independent review.

**Code baseline:** PR #433, including its prerequisite parser hardening in
PR #432 and generator fail-closed hardening in PR #414. Reviewers MUST record
the exact commit IDs they inspect.

## 1. Purpose

The objective is to determine whether the deployed composition preserves:

1. conservation of value;
2. spend authorization and resistance to double-spends;
3. proof soundness and public-input binding;
4. note confidentiality and recipient-only recovery;
5. transaction and block identity under mutation;
6. deterministic consensus behavior across activation and reorg boundaries;
7. bounded, deterministic rejection of malformed inputs.

The review MUST treat this as a Dinero-specific protocol. References to
Sapling, Spartan, Hyrax, BIP340, Poseidon, or libsecp256k1-zkp do not transfer
the security argument of those systems to this composition.

Two questions MUST be reported separately:

- **Current protocol soundness:** whether blocks accepted under the active
  post-height-61,000 rules preserve the objectives above.
- **Historical exposure:** whether earlier activation profiles admitted a
  practical violation, and whether the height-61,000 epoch reset structurally
  prevents that historical state from affecting the current pool.

The active-chain evidence in
[`SHIELDED_V1_BLOCK_COMMITMENT_AUDIT_2026-07-29.md`](SHIELDED_V1_BLOCK_COMMITMENT_AUDIT_2026-07-29.md)
answers one historical block-identity question only. It is not evidence of
cryptographic soundness.

## 2. Adversary model

Assume an adversary can:

- construct arbitrary transaction and shielded-bundle bytes;
- choose all sender-controlled keys, randomness, values, proofs, ciphertexts,
  commitments, anchors, and ordering;
- combine honest outputs with adversarial spends and outputs;
- observe the public chain, mempool, timing, bundle shape, and transparent
  endpoints;
- submit blocks at activation boundaries and create reorgs within normal
  consensus limits;
- send maximally sized, truncated, non-canonical, and resource-intensive
  encodings;
- run a modified prover, wallet, node, or cryptographic library;
- exploit disagreement between native computations and R1CS constraints.

Do not assume an honest wallet, honest proof generator, canonical in-memory
objects, or secret values sampled by repository code.

The primary review is not expected to cover host compromise, wallet-database
encryption, endpoint anonymity, traffic-analysis defenses, denial of service
outside shielded parsing/verification, or physical side channels. Timing and
secret-dependent behavior inside the cryptographic implementation SHOULD still
be identified, even when a full side-channel assessment is out of scope.

## 3. Security invariants to establish

### 3.1 Value conservation

For every accepted non-empty bundle, establish that:

```text
sum(value_spend) - sum(value_output) = value_balance

sum(cv_spend) - sum(cv_output) + value_balance * V = bvk
```

and that the binding signature proves knowledge of the corresponding blinding
scalar without permitting sign, point-normalization, infinity, generator, or
encoding ambiguity.

The reviewer MUST analyze the complete composition of:

- unsigned 64-bit circuit ranges;
- signed `value_balance`;
- transparent input/output/fee accounting;
- CV-bound circuit public inputs;
- per-CV Borromean range proofs;
- the Pedersen tally;
- BIP340 verification under the x-only binding key.

### 3.2 Spend authorization

Establish that an accepted spend requires knowledge of the `rcm`-derived
`sk_note` for a committed note, binds the public anchor and nullifier to the
same witness, and cannot reuse a nullifier within a bundle or accumulated chain
state.

### 3.3 Output and note consistency

Establish that the circuit commitment, encrypted plaintext, recipient trial
decryption, and wallet-derived spend key describe the same note. Analyze the
fact that `pk_d` transports the plaintext while the commitment binds
`pk_note = Poseidon(DeriveNoteSpendKey(rcm), 0)`.

### 3.4 Proof-system binding

Establish native/R1CS equivalence for every public statement and prove that all
public values are absorbed into the intended transcript. Specifically test for:

- unconstrained or weakly constrained public inputs;
- transcript-label collisions or ambiguous serialization;
- generator derivation differences;
- accepting malformed field or curve encodings;
- witness/public-value substitution;
- proof-version confusion across activation epochs.

### 3.5 Identity and malleability

Establish the effect of modifying each shielded field on:

- the custom transparent-envelope digest;
- the binding digest and signature;
- v5 txid and wtxid;
- v6 txid and wtxid;
- the transaction Merkle root and `DINW` witness commitment;
- reconstructed shielded state.

Report historical v5 behavior separately from the current v6 construction.

### 3.6 State transition and recovery

Establish that validation precedes application, reorg rollback restores exactly
the prior tree/nullifier/anchor state, the height-61,000 reset cannot be crossed
ambiguously, and recovery from transaction history reconstructs the same state
without relying on the dormant `DSP` header helper.

### 3.7 Parser and resource safety

For every fixed and variable-length field, establish:

- canonical and minimal decoding;
- no attacker-controlled allocation before the input justifies it;
- no integer truncation, overflow, or out-of-range pointer arithmetic;
- deterministic rejection of trailing or truncated data;
- enforcement of spend/output and proof-container cardinality limits.

PR #432 addresses two concrete CompactSize hazards. The reviewer SHOULD look
for the same pattern in every nested decoder rather than treating that patch as
proof that no related case remains.

## 4. Claim-to-code map

References below identify stable symbols and source files; line numbers are
intentionally omitted because the review target is a PR stack.

| Area | Primary implementation | Primary evidence |
|---|---|---|
| Transaction v5/v6 identity and envelope | `src/primitives/transaction.cpp`; `Transaction::Serialize` | `src/test/v030_wire_vectors_tests.cpp`; witness tests under `tests/consensus/` |
| Bundle wire format and canonicality | `src/consensus/shielded/shielded_serialization.cpp`; `SerializeShieldedBundle`; `DeserializeShieldedBundle` | `ShieldedV030Vectors`; `ShieldedSerialization` |
| Activation parameters | `src/consensus/chainparams_impl.cpp`; `include/consensus/chainparams.h` | shielded validation and epoch-reset tests |
| Validation and state application | `src/consensus/shielded/shielded_validation.cpp`; `ValidateShieldedBundle`; `ApplyShieldedBundle` | `ShieldedValidation`; `ShieldedValidationContext` |
| Epoch reset and rollback | `src/consensus/shielded/shielded_epoch.cpp`; `src/consensus/shielded/shielded_block_validation.cpp` | `ShieldedEpochReset`; reverse-apply/reorg tests |
| Poseidon native computation | `src/consensus/shielded/commitment_tree.cpp`; `Poseidon` | commitment-tree and shielded vector tests |
| Poseidon R1CS computation | `src/zk/zkvm/poseidon_gadget.cpp`; `poseidon2_gadget` | `ShieldedCircuit`; native/R1CS comparison cases |
| Note commitment, tree, and nullifier | `src/consensus/shielded/commitment_tree.cpp`; `src/consensus/shielded/shielded_circuit.cpp` | `CommitmentTree`; `ShieldedCircuit`; `ShieldedV030Vectors` |
| Account/address derivation and encryption | `src/wallet/shielded_derivation.cpp`; public API in `include/wallet/shielded_derivation.h` | `ShieldedDerivation`; addressed-recipient validation tests |
| Wallet transaction construction | `src/wallet/shielded_wallet_ops.cpp`; `src/consensus/shielded/bundle_builder.cpp` | `ShieldedValidation`; `ShieldedProverKit` |
| Spartan/Hyrax proof system | `src/zk/zkvm/r1cs_spartan.cpp`; `src/zk/zkvm/hyrax.cpp`; `src/zk/zkvm/ipa.cpp`; transcript definitions under `src/zk/zkvm/` | `ShieldedCircuit`; `ShieldedCvBinding` |
| Spend/output circuit statements | `src/consensus/shielded/shielded_circuit.cpp` | `ShieldedCircuit`; `ShieldedCvBinding` |
| Pedersen generators and commitments | `src/consensus/shielded/pedersen_generators.cpp`; `src/consensus/shielded/pedersen_commit.cpp` | `Pedersen`; generator fail-closed test from #414 |
| Per-CV range proofs | `src/consensus/shielded/range_proof.cpp`; `DecodeAggregated`; `VerifyBundleRangeProofs` | `ShieldedV030Vectors`; `ShieldedValidation` |
| Binding tally, signature, and digests | `src/consensus/shielded/binding_sig.cpp`; `ComputeShieldedTxSighash`; `ComputeBindingSighash`; `VerifyBinding` | `ShieldedV1ProtocolVectors`; `ShieldedCvBinding`; `ShieldedValidationContext` |
| Witness/block commitment | `src/consensus/witness_commitment.cpp`; `src/consensus/block_validation.cpp`; `src/mining/block_assembler.cpp` | `WitnessMerkleIsolation`; `WitnessCommitment`; `WitnessCommitmentEnforcement`; block-commitment audit |

The reviewer SHOULD expand this map when a helper, dependency, or caller
materially affects a conclusion. In particular, do not stop at public wrapper
functions when consensus behavior depends on library serialization or point
normalization.

## 5. Required independent checks

The review deliverable MUST include:

1. An independently written implementation of the canonical bundle decoder for
   the supplied vectors, including rejection vectors. It MUST NOT call Dinero
   serialization helpers.
2. Independent recomputation of the Poseidon round constants and at least one
   complete native permutation trace.
3. Independent recomputation of account/address derivation, note encryption,
   note commitment, and nullifier vectors.
4. Independent recomputation of the transparent-envelope digest, binding
   digest, CV tally, and BIP340 verification.
5. A native-versus-R1CS constraint audit for spend and output statements,
   including deliberate mutation of each public input.
6. Adversarial range-proof and binding tests covering reordered proofs,
   duplicated CVs, sign changes, infinity/invalid encodings, count mismatch,
   and values at `0`, `2^64 - 1`, and prohibited `2^64`.
7. Activation-boundary cases immediately before, at, and after heights 8,650,
   32,300, 61,000, and 61,001, plus reorgs crossing 61,000.
8. A v5/v6 field-mutation matrix showing which identity or authorization check
   rejects each mutation.
9. Parser fuzzing under memory and undefined-behavior sanitizers with a stated
   corpus, duration, and result.
10. A review of random-scalar generation, nonce reuse, secret erasure, and
    secret-dependent branches in prover and wallet paths.

If a required check cannot be completed, the reviewer MUST state why and how
that limitation affects confidence. “Covered by upstream” is insufficient
unless the reviewer proves that Dinero uses the same parameters, encodings,
preconditions, and transcript.

## 6. Reproduction

Fetch the exact PR stack and record its head:

```sh
git fetch origin \
  fix/shielded-generator-fail-closed \
  codex/fix-shielded-range-container-bounds \
  codex/docs-shielded-protocol-v1
git checkout --detach origin/codex/docs-shielded-protocol-v1
git rev-parse HEAD
git submodule status --recursive
```

Configure and build using the repository's supported OpenSSL baseline. Then run
the focused consensus evidence:

```sh
ctest --test-dir build \
  -R '^(ShieldedCircuit|Pedersen|ShieldedValidation|ShieldedCvBinding|ShieldedValidationContext|ShieldedV030Vectors|ShieldedDerivation)$' \
  --output-on-failure --no-tests=error

ctest --test-dir build \
  -R '^(WitnessMerkleIsolation|WitnessCommitment|WitnessCommitmentEnforcement)$' \
  --output-on-failure --no-tests=error

python3 scripts/ci/check_ctest_integrity.py build
```

The reviewer MUST record all compiler, OpenSSL, secp256k1-zkp, platform, and
build-mode versions. The exact configure command is platform-specific; copying
a cached build directory is not acceptable evidence.

The historical block-commitment scan is read-only and requires a fully indexed
mainnet node. Its RPC method sequence and expected five shielded blocks are
recorded in the block-commitment audit. A different tip is expected; any
difference in the audited historical window is not.

## 7. Finding format and severity

Each finding MUST state:

- violated invariant;
- affected source symbols and consensus epochs;
- attacker capabilities and prerequisites;
- minimal reproducer or test vector;
- current-chain versus historical impact;
- whether exploitation changes validity, privacy, availability, or wallet
  behavior;
- recommended remediation and whether it requires activation/versioning.

Use these severity definitions:

| Severity | Meaning |
|---|---|
| Critical | Permissionless inflation, unauthorized spend, undetectable consensus split, or practical recovery of shielded secrets |
| High | Consensus divergence with realistic prerequisites, proof forgery under restricted conditions, or material deanonymization |
| Medium | Consensus/resource failure requiring unusual conditions, malleability without theft, or significant defense-in-depth failure |
| Low | Hardening, diagnosability, documentation, or low-impact interoperability defect |
| Informational | No demonstrated security impact; useful design or maintenance observation |

Severity MUST reflect the deployed activation epoch and exploitability, not only
the theoretical primitive failure.

## 8. Acceptance criteria

The external review is complete only when:

- every invariant in §3 has an explicit conclusion and evidence;
- every required independent check in §5 is completed or its limitation is
  documented;
- all deterministic values are reproduced without calling the implementation
  that generated them;
- all Critical and High findings have a reproducer and are either remediated or
  explicitly accepted by project owners;
- consensus-changing remediations have historical compatibility analysis and
  an activation plan;
- the final report identifies residual assumptions and gives an overall
  confidence statement separately for current and historical protocol rules.

Passing existing tests is necessary but not sufficient. Absence of a finding
is not evidence unless the reviewer records the analysis or experiment that
supports the conclusion.

## 9. Known open items supplied to the reviewer

1. Issue #431: witness-commitment activation has conflicting sources;
   production currently uses a hard-coded height while chain parameters
   advertise another value.
2. The `aggregated_range_proof` field contains independent Borromean proofs;
   aggregation is not deployed.
3. The block header has no direct shielded-state root; v5 block identity relies
   on `DINW`.
4. The current ciphertext construction has no outgoing-view recovery through
   `ovk`.
5. The specification lists missing cross-implementation vectors for Poseidon,
   proof transcripts, canonical rejection, full epoch transactions, and reset
   reorgs.

These are starting facts, not a limit on review scope.
