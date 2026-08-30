# Dinero Shielded Protocol v1

**Status:** Normative description of the deployed v1 consensus profile.

**Implementation baseline:** Dinero v8.1.9. Historical blocks remain governed
by the activation table in §3. The spend-authority extension described below
is implemented but dormant at `UINT32_MAX` on every shipped network.

**Security status:** This document describes what the implementation does. It
is not a claim that the custom composition has inherited the security proofs of
Sapling, BIP340, Spartan, or libsecp256k1-zkp. Independent cryptographic review
remains required; §15 identifies the review boundary and the
[external-review package](../audits/SHIELDED_V1_EXTERNAL_REVIEW_PACKAGE.md)
defines the assignment, evidence, and acceptance criteria.

If this document and deployed consensus code disagree, nodes follow the code.
The disagreement is a consensus-spec defect and MUST be resolved explicitly;
implementations MUST NOT choose whichever interpretation is more convenient.

## 1. Scope and terminology

This specification covers:

- shielded transaction versions 5 and 6;
- the canonical shielded-bundle encoding;
- key and address derivation;
- note encryption, commitments, nullifiers, and the commitment tree;
- spend/output proofs, per-value range proofs, and the binding signature;
- activation boundaries and state transitions.

It does not define transparent script validation, Utreexo, block difficulty,
wallet policy, RPC schemas, or peer-to-peer relay policy.

`MUST`, `MUST NOT`, `SHOULD`, and `MAY` are normative. Byte strings are written
in wire order. `LE32`, `LE64`, `BE32`, and `BE64` denote fixed-width integer
encodings. `CompactSize` is Bitcoin's minimally encoded variable-length
integer. `varbytes(x)` is `CompactSize(len(x)) || x`.

The secp256k1 scalar order is:

```text
n = FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFEBAAEDCE6AF48A03BBFD25E8CD0364141
```

All 32-byte values entering the shielded Poseidon implementation are interpreted
as big-endian scalar representations.

## 2. Deployed construction at a glance

The protocol is a Dinero-specific composition:

| Function | Deployed primitive |
|---|---|
| Note/tree/nullifier hash | custom Poseidon permutation over the secp256k1 scalar field |
| Spend/output proof | custom Spartan/Hyrax R1CS implementation |
| Value commitment | libsecp256k1-zkp Pedersen commitment |
| Value range | one libsecp256k1-zkp Borromean range proof per value commitment |
| Bundle authorization | BIP340 Schnorr signature under a derived binding key |
| Address ECDH | x-only secp256k1 |
| Note encryption | HKDF-SHA256 and ChaCha20-Poly1305 |

The field named `aggregated_range_proof` is historical and misleading. It
contains a list of independent proofs; no range-proof aggregation is deployed.

## 3. Consensus activation and epochs

### 3.1 Mainnet

| Rule | First affected height | Behavior |
|---|---:|---|
| Non-empty shielded bundles accepted | 8,650 | legacy proof behavior |
| Public-input binding, mandatory range proofs, and binding signature | 32,300 | legacy proof versions 0x01/0x02, but public inputs are bound |
| Epoch reset and value-commitment-bound circuits | 61,000 / 61,001 | height 61,000 resets and rejects shielded activity; new epoch activity begins at 61,001 with proof versions 0x03/0x04 |

At height 61,000 the commitment tree, anchor history, and nullifier set are
discarded. Every pre-reset note becomes unspendable. The reset height MUST equal
the value-commitment-binding activation height; chain-parameter selection
enforces this invariant.

### 3.2 Testnet and regtest

Testnet leaves shielded activation and both binding activations dormant at
`UINT32_MAX`.

Regtest activates shielded and public-input binding at height 0.
Value-commitment binding and epoch reset are dormant unless the regtest-only
`--consensus-shielded-epoch-reset-height` override sets both to the same height.

Changing any activation boundary changes block validity and requires a
coordinated network upgrade.

## 4. Transaction envelope

Only transaction versions 5 and 6 carry a shielded bundle.

- Version 5 includes the bundle in witness serialization and therefore in the
  wtxid, but excludes it from the legacy txid.
- Version 6 includes the bundle in both txid and wtxid serialization.

For a shielded transaction, the serialization order is:

```text
version
transparent inputs
transparent outputs
explicit-fee marker [and LE64 fee when marker = 1]
witness stacks, when included
varbytes(canonical shielded bundle)
locktime
```

The fee marker is one byte: `00` means absent and `01` is followed by the
unsigned LE64 fee. Current validation can derive a fee from transparent values
when the explicit fee is absent; consumers MUST NOT assume every historical
shielded transaction carries the explicit value.

Value conservation uses:

```text
value_balance = transparent_input - transparent_output - fee
              = shielded_output - shielded_spend
```

A positive `value_balance` moves value into the shielded pool; a negative value
moves value out.

## 5. Canonical shielded-bundle encoding

The bundle is encoded as:

```text
LE64(value_balance bit pattern)
CompactSize(spend_count)
repeat spend_count times:
    nullifier[32]
    anchor[32]
    cv[33]
    varbytes(spend_proof)
CompactSize(output_count)
repeat output_count times:
    note_commitment[32]
    cv[33]
    varbytes(encrypted_note)
    varbytes(output_proof)
varbytes(range_proof_container)
bvk_commitment[33]
binding_signature[64]
```

`value_balance` is a signed 64-bit two's-complement value whose bit pattern is
written little-endian.

Spends MUST be strictly ordered by ascending nullifier. Outputs MUST be strictly
ordered by ascending note commitment. Duplicates therefore violate ordering.
All CompactSize values MUST be minimal, trailing bytes are forbidden, and
decode followed by re-encode MUST reproduce the original bytes exactly.

A bundle is limited to 200 spends and 200 outputs. An empty bundle is one with
no spends and no outputs.

## 6. Poseidon profile

`Poseidon(a,b)` operates over the secp256k1 scalar field with:

```text
state width t       = 3
initial state       = [0, a, b]
S-box               = x^5
full rounds         = 8 (4 before and 4 after the partial rounds)
partial rounds      = 56 (S-box on state[0] only)
MDS                 = [[2,1,1], [1,2,1], [1,1,2]]
output              = state[1]
```

Each round first adds its three constants, then applies the applicable S-boxes,
then applies the MDS matrix.

For round `r` and state element `e`, form:

```text
candidate = SHA256("PoseidonC_secp256k1" || BE32(r) || BE32(e))
```

Interpret `candidate` as a big-endian integer. If it is zero or not less than
`n`, replace it with `SHA256(candidate)` and repeat until it is a valid non-zero
secp256k1 scalar. This rehash-to-valid rule is consensus behavior; it is not
reduction modulo `n`.

The implementation calls the function “Poseidon-2” in several identifiers, but
the parameters above—not that name—define interoperability.

## 7. Keys and addresses

### 7.1 Account derivation

The hardened BIP32 path is:

```text
m / 99' / 1448' / account'
```

The account private key bytes are `sk`. No BIP32 child is appended for a
receive-address index.

Define `DST32(s)` as ASCII `s` placed at the beginning of a 32-byte buffer and
zero-padded on the right. Define:

```text
PRF(key, label) = Poseidon(key, DST32(label))
```

Derive:

```text
ask_raw = PRF(sk, "DIN/v7/shielded/ask")
nsk_raw = PRF(sk, "DIN/v7/shielded/nsk")
ovk     = PRF(sk, "DIN/v7/shielded/ovk")
dk      = PRF(sk, "DIN/v7/shielded/dk")
```

`ask_raw` and `nsk_raw` MUST be valid non-zero secp256k1 scalars. Their public
points are normalized to the BIP340 even-y representatives; when normalization
negates a point, the corresponding secret scalar is also negated. The resulting
x-only public keys are `ak` and `nk`, and:

```text
ivk = Poseidon(ak, nk)
```

`ivk` MUST be a valid non-zero secp256k1 scalar when used for address derivation
or ECDH.

The current note-encryption path does not use `ovk` for outgoing recovery.
Implementations MUST NOT promise outgoing-view recovery from `ovk` without a
new, specified ciphertext construction.

### 7.2 Diversifiers

For the implemented 64-bit diversifier index `j`, construct the 16-byte OpenSSL
ChaCha20 IV:

```text
bytes 0..3   = LE32 block counter 0
bytes 4..11  = LE64(j)
bytes 12..15 = 00000000
```

Encrypt eleven zero bytes using ChaCha20 under key `dk`; the eleven output bytes
are diversifier `d`.

This is the concrete OpenSSL `EVP_chacha20()` 16-byte-IV convention. It is not
the 12-byte nonce construction described by older drafts.

### 7.3 Diversifier point and address

Compute:

```text
seed = SHA256(DST32("DIN/v7/shielded/div") || d)
P_d  = secp256k1_generator_generate(seed)
```

Serialize the generator using libsecp256k1-zkp's 33-byte generator encoding,
discard its first byte, and interpret the x-coordinate as the even-y
secp256k1 point.

Then derive independent discovery and spend-authority keys:

```text
pk_d_enc   = xonly_even_y(ivk * P_d)
s_raw      = Poseidon(ivk, zero_pad_32(d))
(s, pk_d_spend) = even_y_normalize(s_raw, s_raw * G)
payload = d[11] || pk_d_enc[32] || pk_d_spend[32]
```

The 75-byte payload is converted from 8-bit to 5-bit groups with padding and
encoded using raw Bech32m:

| Network | HRP |
|---|---|
| Mainnet | `dins` |
| Testnet | `tdins` |
| Regtest | `rdins` |

Decoders MUST require Bech32m, one of these HRPs, exactly 75 decoded bytes, and
on-curve x-only encodings for both keys. Legacy 43-byte addresses MUST be
rejected: they do not identify a recipient-controlled spend key.

`pk_d_enc` is used only for ECDH note discovery. `pk_d_spend` is committed by
post-spend-authority notes and spending proves knowledge of its unique even-y
scalar `s`. A sender knows both public keys but does not learn `s`.

## 8. Note encryption and ownership

### 8.1 Plaintext

The 563-byte note plaintext is:

```text
d[11] || LE64(value_una) || rcm[32] || memo[512]
```

The spend key and note public key are:

```text
sk_note = Poseidon(rcm, DST32("DIN/v7/shielded/sk_note/v1"))
pk_note = Poseidon(sk_note, 0)
```

`pk_note`, not address transmission key `pk_d`, is committed by the note circuit.
The address key is used to deliver the plaintext that contains `rcm`, from which
the recipient derives `sk_note`.

### 8.2 ECDH and AEAD

The sender chooses a valid non-zero `esk` and computes:

```text
epk = xonly_even_y(esk * P_d)
```

If even-y normalization negates the point, the sender MUST negate `esk` before
computing the shared secret:

```text
shared_sender    = xonly((normalized esk) * pk_d)
shared_recipient = xonly(ivk * epk)
```

Derive a 32-byte encryption key using HKDF-SHA256:

```text
PRK = HMAC-SHA256(key = epk, data = shared)
key = HMAC-SHA256(key = PRK,
                  data = "DIN/v7/shielded/note" || 01)
```

Encrypt the 563-byte plaintext with ChaCha20-Poly1305 using a 12-byte all-zero
nonce and `epk` as 32-byte associated data:

```text
encrypted_note = epk[32] || ciphertext[563] || tag[16]
```

The canonical wallet construction therefore emits exactly 611 bytes. Consensus
bundle parsing treats this field as variable length; circuit proof and wallet
decryption supply the semantic checks.

## 9. Note commitments, nullifiers, and tree

Pack `d` into the first eleven bytes of a 32-byte buffer and zero-pad the rest.
Encode the note value as a 32-byte big-endian scalar. Let:

```text
ADDR_TAG = DST32("DIN/v7/shielded/addr/v1")
addr_key = Poseidon(d_packed, pk_note)
addr_bind = Poseidon(ADDR_TAG, addr_key)
note_commitment = Poseidon(Poseidon(addr_bind, value), rcm)
nullifier = Poseidon(sk_note, BE32_256(leaf_index))
```

`BE32_256(x)` is the 32-byte big-endian scalar encoding of the 64-bit leaf
index.

The append-only note-commitment tree has depth 32. Its empty leaf is
`Poseidon(0,0)` and higher empty roots recursively hash each empty root with
itself. Leaves are appended in block transaction order and canonical bundle
output order.

An anchor is valid when it equals the current root or one of the last 100 roots
held by anchor history. Published nullifiers MUST be unique both within the
bundle and in accumulated chain state.

Shielded commitments and nullifiers are separate from Utreexo. No shielded note
is inserted into the Utreexo accumulator.

The deployed block header has no direct shielded-state root. Its reserved bytes
remain zero, and the experimental `DSP` coinbase shielded-commitment encoder has
no production caller.

The normal coinbase `DINW` witness commitment indirectly commits every wtxid
and therefore the v5 bundle bytes. Mainnet validation requires `DINW` for
witness-bearing blocks from height 10,670; the honest block assembler emitted
it before then. A chain scan of the five shielded blocks in the optional window
8,650–10,669 found `DINW` in every one. See
[`SHIELDED_V1_BLOCK_COMMITMENT_AUDIT_2026-07-29.md`](../audits/SHIELDED_V1_BLOCK_COMMITMENT_AUDIT_2026-07-29.md).

Version 6 additionally commits the bundle directly through txid. Shielded state
is reconstructed from transaction history; operator state digests are recovery
checks, not wire commitments.

## 10. Value commitments

Let `G` be the standard secp256k1 generator. Derive value generator `V` by:

```text
seed = SHA256("DIN/v7/shielded/cv/V/v1")
V = secp256k1_generator_generate(seed)
```

For value `v` and blinding scalar `rcv`:

```text
cv = rcv * G + v * V
```

`cv` is serialized using libsecp256k1-zkp's 33-byte Pedersen-commitment
encoding: prefix `08` or `09` followed by the 32-byte x-coordinate. The prefix
is part of the point encoding and MUST be preserved. Implementations MUST use
the library's exact encoding/decoding convention rather than substituting SEC1
compressed-point prefixes.

## 11. Spend and output proofs

Proof byte zero selects the circuit:

| Proof | Legacy circuit | CV-bound circuit |
|---|---:|---:|
| Spend | `01` | `03` |
| Output | `02` | `04` |

The remaining bytes are the custom serialized Spartan proof.

The spend transcript root is `dinero.shielded.spend.v1`; it binds labels `nf`
and `an`. CV-bound proofs additionally bind `cv0` as a u64 and `cvx` as a
scalar. The output transcript root is `dinero.shielded.output.v1`; it binds
`cm`, and CV-bound proofs additionally bind `cv0` and `cvx`.

The spend circuit proves:

- `pk_note = Poseidon(sk_note, 0)`;
- the address-bound note commitment from §9;
- a depth-32 Merkle path to public anchor;
- `nullifier = Poseidon(sk_note, leaf_index)`;
- note value is in the unsigned 64-bit range;
- after CV-binding activation, public `cv = rcv*G + value*V`.

Private spend inputs are `sk_note`, value, `rcm`, packed diversifier,
leaf index, Merkle siblings, and `rcv`. Public inputs are nullifier, anchor,
and—after activation—the full CV point.

The output circuit proves:

- the address-bound note commitment from §9;
- note value is in the unsigned 64-bit range;
- after CV-binding activation, public `cv = rcv*G + value*V`.

Private output inputs are value, `pk_note`, `rcm`, packed diversifier, and
`rcv`. Public inputs are note commitment and—after activation—the full CV
point.

The proof system, generator derivation, transcript framing, and proof
serialization are implementation-specific. Interoperable implementations need
byte-level vectors for these internals; naming Spartan or Hyrax alone is not
sufficient.

## 12. Per-CV range-proof container

Despite its field name, the v1 range-proof container is:

```text
CompactSize(N)
repeat N times:
    varbytes(libsecp256k1-zkp Borromean range proof)
```

`N` MUST equal `spend_count + output_count`. Proof order is canonical spends
first, followed by canonical outputs. Each proof establishes that the value
inside its corresponding CV is in `[0, 2^64)`.

A non-empty bundle at or above public-input-binding activation MUST carry this
container and every proof MUST verify. Generator unavailability is a validation
failure. Below that activation, historical opportunistic behavior is retained.

Replacing this list with a Bulletproof or another aggregated construction would
change consensus semantics. Such a change requires a new explicit encoding,
test vectors, activation rule, and coordinated upgrade; the existing bytes MUST
NOT be silently reinterpreted.

## 13. Binding equation and sighashes

Let:

```text
bsk = sum(rcv_spend) - sum(rcv_output)
bvk = bsk * G
```

Consensus verifies the Pedersen tally:

```text
sum(cv_spend) - sum(cv_output) + value_balance * V = bvk
```

`bvk` is carried using the same 33-byte Pedersen encoding with zero value
component. Its x-coordinate, interpreted as a BIP340 x-only public key, verifies
the 64-byte binding signature.

### 13.1 Transparent-envelope digest

The transaction digest is one SHA256 over:

```text
"DIN/v7/shielded/tx-sighash/v1"
LE32(version)
LE32(input_count)
repeat inputs:
    prev_txid[32]
    LE32(prev_vout)
    LE32(sequence)
LE32(output_count)
repeat outputs:
    LE64(value_una)
    LE32(scriptPubKey_length)
    scriptPubKey
LE32(locktime)
```

This is a custom fixed-layout digest. It is not BIP143, despite stale historical
comments that used that name.

### 13.2 Binding digest

Sort copies of spend CVs lexicographically by all 33 bytes; independently sort
output CVs the same way. The binding digest is one SHA256 over:

```text
"DIN/v7/shielded/binding/v1"
LE64(value_balance bit pattern)
tx_sighash[32]
LE64(spend_cv_count)
sorted spend CVs, 33 bytes each
LE64(output_cv_count)
sorted output CVs, 33 bytes each
```

The bundle carries a BIP340 signature of this digest under x-only `bvk`.

## 14. Validation and state transition

For each non-empty bundle, consensus:

1. enforces shielded activation and the 200/200 count limits;
2. rejects missing spend/output circuit proofs;
3. rejects duplicate or previously seen nullifiers;
4. validates spend anchors;
5. validates the mandatory or historical range-proof rule;
6. validates the mandatory or historical binding-signature rule;
7. verifies every spend and output circuit under the height-selected mode;
8. checks `value_balance == transparent_input - transparent_output - fee`.

Only after successful block validation does state application append outputs to
the shielded commitment tree and insert spend nullifiers. Neither operation
touches Utreexo.

## 15. Privacy properties, non-claims, and review boundary

Public data includes transaction version, transparent inputs and outputs, fee
behavior, `value_balance`, spend/output counts, nullifiers, anchors, note
commitments, all CVs, proof lengths and bytes, encrypted-note lengths and bytes,
`bvk`, and the binding signature. The protocol does not hide transaction graph
timing, bundle shape, shield/unshield amount, or transparent endpoints.

The recipient address is not placed directly on chain, but the encrypted note
is trial-decryptable by the incoming viewing key. The current construction has
no implemented outgoing-view recovery using `ovk`.

Version 5 does not bind the shielded bundle into its legacy txid. Its block
identity relies on the coinbase `DINW` witness commitment to bind wtxid.
Version 6 also binds the bundle directly into txid. Consumers MUST use the
correct identity semantics.

An external review MUST treat the following as one new protocol rather than as
independent inherited components:

1. Poseidon parameters, scalar encodings, and native/R1CS equivalence;
2. Spartan/Hyrax generator derivation, transcript framing, serialization, and
   public-input binding;
3. note ownership: address ECDH versus `rcm`-derived spend key;
4. CV binding, per-CV range proofs, binding tally, sign convention, and BIP340
   key normalization;
5. v5/v6 transaction identity and replay/malleability behavior;
6. activation boundaries, the height-61,000 reset wall, and reorg behavior;
7. parser resource bounds and rejection behavior for every nested length;
8. recovery from transaction history without a block-header shielded-state
   commitment.

## 16. Test vectors and change control

Existing deterministic vectors are documented in
[`shielded_v030_test_vectors.md`](shielded_v030_test_vectors.md) and exercised by
the `ShieldedV030Vectors` CTest target. Key/address/encryption vectors are
exercised by `ShieldedDerivation`.

`ShieldedV1ProtocolVectors.EnvelopeAndBindingSighashes` additionally pins:

```text
transparent-envelope digest:
6c8bd3394e75889ab9a9e10a0f2574a6538ac6bfeaf058c635a60145cb2b43c4

binding digest:
09c6dc4754fc5c2d543aa712b9ff4d923e9828ff10388e84a3baeb0f1f441eb9

range container for proofs aabb and cc:
0202aabb01cc
```

Before calling this specification independently implementable, the repository
SHOULD add byte-level vectors for:

- the exact Poseidon round-constant sequence;
- canonical bundle decoding failures;
- every proof version and transcript;
- one full shield, transfer, and unshield transaction at each active epoch;
- epoch-reset and reorg boundary cases.

Any change to a formula, domain separator, byte order, proof version, point
encoding, activation height, or canonical ordering rule is consensus-sensitive.
It requires explicit versioning or historical compatibility analysis, failing
tests before the change, cross-implementation vectors, and coordinated
activation where block validity changes.
