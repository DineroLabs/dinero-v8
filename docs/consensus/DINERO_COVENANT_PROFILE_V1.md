# Dinero covenant profile v1

Status: normative profile v1. Mainnet CTV/CCV activate together at block
100,000. Testnet remains dormant.

This document defines the complete first Dinero covenant profile. The key
words MUST, MUST NOT, SHOULD, SHOULD NOT, and MAY are normative.

## 1. Scope

Profile v1 consists of:

- BIP341 P2TR script-path spending with BIP342 tapscript;
- `OP_CHECKTEMPLATEVERIFY` (CTV), opcode `0xb3`, using the BIP119 default
  template hash for eligible transparent transactions; and
- `OP_CHECKCONTRACTVERIFY` (CCV), opcode `0xbe`, using Dinero successor
  binding v1.

Profile v1 does not activate `OP_CHECKSIGFROMSTACK`,
`OP_CHECKSIGFROMSTACKVERIFY`, or `OP_TXHASH`. It does not define confidential
CTV/CCV, a shielded covenant, a new witness version, or a Utreexo-specific
covenant rule.

The detailed CTV and CCV definitions in `CTV_BIP119_PROFILE.md` and
`CCV_SUCCESSOR_BINDING_V1.md` are incorporated into this profile. If prose in
a non-normative status or audit report conflicts with those definitions or
this document, this document controls the combined profile.

## 2. Inherited consensus behavior

P2TR output-key construction, control-block validation, tapscript leaf
version `0xc0`, Schnorr verification, signature hashing, annex handling,
`OP_SUCCESS`, and tapscript execution inherit BIP341 and BIP342 except where
this profile assigns activated behavior to `0xb3` and `0xbe`.

Bitcoin-derived vectors are authoritative only for the inherited behavior.
Dinero-specific transaction extensions and CCV are governed by Dinero's
native vectors.

An unknown Taproot leaf version MUST succeed after a valid control path unless
relay policy rejects it. Before its Dinero activation, `0xbe` is a BIP342
`OP_SUCCESS` opcode and MUST cause immediate success when decoded anywhere in
the revealed tapscript, including an unexecuted conditional branch. Activated
custom opcodes are removed from the `OP_SUCCESS` set before execution.

Security consequence: a wallet or contract compiler MUST NOT place any
unactivated `OP_SUCCESS` opcode, including dormant CSFS or TXHASH slots, in a
leaf it expects to enforce constraints. That leaf is anyone-can-spend through
its valid Taproot control path until the opcode receives new consensus
semantics.

## 3. Activation

Each feature has an independent height:

| Chain | Script path | CTV | CCV | CSFS/TXHASH |
|---|---:|---:|---:|---:|
| Mainnet | 1 | 100,000 | 100,000 | dormant |
| Testnet | 200 | dormant | dormant | dormant |
| Regtest | 20 | 20 | 20 | dormant |

`dormant` means `UINT32_MAX`, which MUST never be treated as an active height.
CTV and CCV activation MUST NOT precede script-path activation. Mainnet pins
both reviewed opcodes to the same height so there is no partial profile-v1
window.

Block validation uses the candidate block height. Mempool admission and mining
selection use the height of the next candidate block. When a reorg changes
whether script path, CTV, or CCV is active at that candidate height, every
retained mempool transaction MUST be revalidated before it is selectable for
mining.

The five activation heights are committed by `ConsensusChecksum`. Nodes with
different parameters are operationally incompatible even before an activated
opcode is observed.

## 4. CTV

### 4.1 Pre-activation

Before CTV activation, `0xb3` MUST retain `OP_NOP4` consensus behavior. Relay
policy MAY discourage it as an upgradable NOP.

### 4.2 Eligible transaction forms

The BIP119 default template hash is defined only when all of the following
hold:

- the transaction is not a shielded-version transaction;
- it does not use Dinero's explicit-fee serialization;
- it has no confidential outputs; and
- its input and output counts fit unsigned 32-bit values.

An activated 32-byte CTV check on any other form MUST fail. No Dinero extension
is silently omitted from a hash presented as BIP119-compatible.

### 4.3 Template hash

For input index `i`, the preimage is:

```text
version_le32
locktime_le32
[ SHA256(concat(CompactSize(scriptSig.size) || scriptSig))
    if any input scriptSig is non-empty ]
input_count_le32
SHA256(concat(sequence_le32))
output_count_le32
SHA256(concat(value_le64 || CompactSize(scriptPubKey.size) || scriptPubKey))
input_index_le32
```

`DefaultCheckTemplateVerifyHash = SHA256(preimage)`.

The transaction version is serialized as its 32-bit two's-complement
little-endian representation. Counts and the input index are fixed-width
unsigned little-endian values. Script lengths use canonical CompactSize.
Witnesses, prevout identifiers, and spent amounts are not committed by this
hash.

### 4.4 Execution

After activation:

1. CTV requires at least one stack element.
2. If the top element is not exactly 32 bytes, CTV behaves as a reserved NOP;
   relay policy MAY reject it.
3. If it is 32 bytes, it MUST equal the default template hash for the current
   transaction and input index.
4. CTV does not pop or replace the element.
5. At most one CTV may execute in one revealed tapscript. A second executed
   CTV MUST fail, including when either argument uses reserved non-32-byte
   behavior. CTV bytes in an unexecuted branch do not count.

## 5. CCV successor binding v1

### 5.1 State encoding

A state stack element is exactly:

```text
stateHash[32] || codeHash[32] ||
counter_le32 || data_length_le32 || data[data_length]
```

No trailing bytes are permitted. `data_length` MUST be at most 448, making the
entire state at most the tapscript 520-byte stack-element maximum.

```text
codeHash  = SHA256(revealed_tapscript)
stateHash = SHA256(codeHash || counter_le32 || data)
```

### 5.2 State-derived internal key

For retry values starting at zero:

```text
candidate =
  TaggedHash("Dinero/CCVInternalKey/v1", stateHash || retry_le32)
```

The first candidate accepted by secp256k1 as an x-only public key is the
internal key. This is deterministic hash-to-x; no known private scalar is
derived.

For the already authenticated Taproot Merkle root `m`:

```text
t = TaggedHash("TapTweak", internalKey || m)
P = internalKey + t*G
scriptPubKey = OP_1 PUSH32 xonly(P)
```

The control-block parity bit MUST match `P`.

### 5.3 Execution

The input stack order is:

```text
<previous_state> <next_state> OP_CHECKCONTRACTVERIFY
```

CCV pops both state elements and pushes nothing. For current input index `i`,
it succeeds only if:

1. `i` exists in the transaction inputs, outputs, and complete spent-output
   vector.
2. Both encodings and both `stateHash` values are valid.
3. `previous.counter` is not `UINT32_MAX`.
4. `next.counter == previous.counter + 1`.
5. `next.codeHash == previous.codeHash`.
6. `previous.codeHash == SHA256(revealed_tapscript)`.
7. The authenticated internal key, Merkle root, and parity match the previous
   state.
8. The spent output is exactly the transparent P2TR output derived from the
   previous state.
9. Output `tx.vout[i]` is transparent, preserves the exact spent value, and is
   exactly the P2TR output derived from the next state under the same tree.
10. No other transaction output has the same successor scriptPubKey.

At most one CCV may execute in one revealed tapscript. A second executed CCV
MUST fail. CCV bytes in an unexecuted branch do not count.

The exact-value rule means the CCV input cannot pay its own transaction fee.
Another input MUST fund any fee. CCV proves state continuity; application
rules for `data` MUST be enforced elsewhere in the authenticated tapscript.
A terminal path requires a separate authenticated leaf that deliberately does
not execute CCV.

## 6. Resource model

Consensus call paths MUST construct immutable transaction-wide precomputation
once per transaction and share it across input verification. The shared data
contains the CTV component hashes, the five BIP341 transaction hashes, and
Dinero's whole-prevout confidential extension.

Input-specific `ANYONECANPAY`, `SIGHASH_SINGLE`, annex, tapleaf, and
code-separator data remains local. BIP342 charges 50 validation-weight units
for each non-empty `CHECKSIG`, `CHECKSIGVERIFY`, or `CHECKSIGADD`.

The 100,000-byte transaction maximum, BIP342 stack/witness limits, signature
budget, and one-execution CTV/CCV limits jointly bound profile-v1 validation.
The measured evidence is in `COVENANT_RESOURCE_LIMITS.md`.

## 7. Utreexo and state storage

Utreexo does not define covenant semantics. A stateless validator may use a
valid Utreexo proof to obtain the same spent output data used by a full UTXO
node. After that authentication, both modes MUST apply identical P2TR, CTV,
CCV, amount, and successor rules.

No covenant state database is consensus-authoritative in profile v1. CCV state
is committed by the spent and successor P2TR outputs and is revealed by the
witness.

## 8. Reorg, persistence, and caching

Script-validation cache keys MUST include the height-derived activation flags.
An entry validated under one activation state MUST NOT authorize a spend under
another.

Mempool persistence is not consensus. After restart, loaded transactions MUST
be subjected to the same current-height validation and mining-selection rules
as newly received transactions.

Disconnecting below an activation boundary restores the exact pre-activation
meaning: CTV returns to NOP4 and CCV returns to `OP_SUCCESS`. Reconnecting
restores profile-v1 enforcement.

## 9. Normative vectors

The following executable sources are part of the profile-v1 review vectors:

- `tests/consensus/test_bip119_ctv_vectors.cpp` contains four byte-serialized
  upstream BIP119 transactions and expected hashes.
- `tests/consensus/test_bip341_sighash_vectors.cpp` contains official BIP341
  sighash messages plus cached/uncached and confidential-extension
  equivalence checks.
- `tests/consensus/test_ccv_successor_binding.cpp` contains state,
  state-derived key, current output, successor output, mutation, and
  authenticated script-path vectors.
- `tests/consensus/test_covenant_activation.cpp` pins every chain parameter,
  consensus-checksum commitment, spend-height semantics, and CTV
  precomputation.

The CCV golden values are repeated in `CCV_SUCCESSOR_BINDING_V1.md` for manual
review. A change to any normative vector requires a versioned specification
change, a new reproducible assurance record, and a new public review window.

## 10. Production activation requirements

This specification records the scheduled mainnet height but does not by itself
authorize release or deployment. Before shipping the activation, the project
MUST:

1. retain the checksummed, watch-only wallet recovery path and live
   two-daemon relay/restart/reorg coverage implemented for regtest;
2. repeat those deployment tests against the proposed activation boundaries
   and release candidates;
3. complete the reproducible open-source consensus and cryptographic assurance
   record for this profile, implementation, vectors, lifecycle tests, and
   resource analysis, and publish it for review;
4. publish the chosen activation parameters and release hashes with enough
   fleet-upgrade lead time; and
5. monitor activation and retain a coordinated incident response procedure.
