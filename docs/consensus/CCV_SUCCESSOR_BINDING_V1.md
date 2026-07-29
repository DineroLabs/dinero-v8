# CCV Successor Binding v1

**Status:** Normative implementation specification; mainnet and testnet
activation heights are not yet assigned.

**Opcode:** `OP_CHECKCONTRACTVERIFY` (`0xbe`)

**Verification flag:** `SCRIPT_VERIFY_CCV_SUCCESSOR_BINDING` (bit 24)

## 1. Purpose

`OP_CHECKCONTRACTVERIFY` (CCV) advances a stateful contract without permitting
the contract coin to escape, change value, or silently fork into duplicate
successors. The legacy CCV rule checked only a witness-supplied counter, code
hash, and successor state hash. It did not bind the previous state to the spent
UTXO or require any output to contain the successor state.

This specification defines the completed transparent-value rule. It is a new
Dinero consensus protocol; similarity to Taproot or other covenant proposals
does not transfer their security analysis to this construction.

## 2. Contract state encoding

A witness-serialized `ContractState` is:

| Field | Size | Encoding |
|---|---:|---|
| `stateHash` | 32 bytes | Raw bytes |
| `codeHash` | 32 bytes | Raw bytes |
| `counter` | 4 bytes | Unsigned little-endian |
| `dataLen` | 4 bytes | Unsigned little-endian |
| `data` | `dataLen` bytes | Contract-defined bytes |

The encoding must be exact: no bytes may follow `data`, and `dataLen` must equal
the remaining byte count. `data` is limited to 448 bytes so the complete state
fits Tapscript's 520-byte stack-element limit.

The commitments are:

```text
codeHash  = SHA256(revealed_tapscript)
stateHash = SHA256(codeHash || counter_le32 || data)
```

The length field is part of the witness encoding but not the `stateHash`
preimage. The fixed-width `codeHash` and counter make the preimage boundary
unambiguous.

## 3. State-derived Taproot internal key

Each state commits to a NUMS-style x-only secp256k1 internal key. For retry
values beginning at zero:

```text
candidate = TaggedHash(
    "Dinero/CCVInternalKey/v1",
    stateHash || retry_le32
)
```

The first candidate accepted by `secp256k1_xonly_pubkey_parse` is the internal
key `Q_state`. `TaggedHash(tag, message)` means:

```text
SHA256(SHA256(tag) || SHA256(tag) || message)
```

This is hash-to-x, not `hash * G`. No secret scalar is constructed from public
state, so the derivation does not create an intentionally known key-path
private key.

For the already-verified Tapscript Merkle root `m`, the contract output is the
standard P2TR script:

```text
t = TaggedHash("TapTweak", Q_state || m)
P = Q_state + t*G
scriptPubKey = OP_1 PUSH32 xonly(P)
```

The control-block parity bit must equal the parity of `P`.

## 4. Activated verification rule

For CCV executing at input index `i`, let `prev` and `next` be the two states
removed from the witness stack, and let `spent[i]` be the corresponding UTXO.
The opcode succeeds only if every rule below holds:

1. `i` exists in `tx.vin`, `tx.vout`, and the complete input-UTXO vector.
2. Both state encodings and hashes are valid and each `data` field is no more
   than 448 bytes.
3. `prev.counter` is not `UINT32_MAX`.
4. `next.counter == prev.counter + 1`; skipping, repeating, or wrapping fails.
5. `next.codeHash == prev.codeHash`.
6. `prev.codeHash == SHA256(revealed_tapscript)`.
7. The internal key and parity in the verified control block are exactly those
   derived from `prev`.
8. `spent[i].scriptPubKey` is exactly the P2TR output derived from `prev` and
   the verified Merkle root.
9. `spent[i]` is transparent.
10. `tx.vout[i]` is transparent and has exactly the same `AmountUna` value as
    `spent[i]`.
11. `tx.vout[i].scriptPubKey` is exactly the P2TR output derived from `next`
    and the same Merkle root.
12. No other transaction output has that exact successor scriptPubKey.

Input index equals successor output index. This deterministic mapping prevents
multiple CCV inputs from claiming one successor and makes multi-input
verification independent.

Exact value preservation means a CCV input does not pay its own transaction
fee. A transaction that needs a fee must include separate fee-paying input(s).

## 5. Script responsibilities

CCV proves state continuity only. It does not decide whether the new `data` is
allowed for a particular auction, vault, escrow, or other application. Those
business rules must be enforced by the revealed Tapscript before or after CCV.
A script consisting only of `OP_CHECKCONTRACTVERIFY OP_TRUE` permits anyone to
choose the next data while still preserving the contract coin and state
lineage.

CCV always requires a successor. A deliberate terminal or recovery transition
must use a separate Taproot leaf that does not execute CCV and that independently
enforces the intended exit conditions.

The same Merkle root is retained across transitions. Changing the contract's
available Taproot leaves is not a CCV v1 state transition.

## 6. Confidential values

CCV v1 rejects confidential inputs and confidential successors. Equality of
Pedersen commitment bytes is not a general proof of equal hidden values when
blinding factors can differ. Confidential CCV requires a separately specified
and reviewed value-equality/conservation proof and a new activation rule.

This fail-closed rule supersedes older design documents that described
confidential stateful covenants as complete.

## 7. Witness and annex handling

For the activated rule, BIP341 annex processing occurs before classifying the
spend path. An annex is the final witness element when that element is nonempty
and begins with `0x50`. It is removed from the effective witness and included
in Taproot signature hashing.

CCV state elements are never treated as annexes merely because their first byte
is `0x50`.

State elements are copied before removal from the execution stack. Retaining a
reference across `pop_back()` is invalid and was a source of nondeterministic
legacy CCV failures.

## 8. Activation and compatibility

The rule is selected by height and chain:

| Chain | Activation |
|---|---|
| Mainnet | Dormant (`UINT32_MAX`) pending an explicitly coordinated height |
| Testnet | Dormant (`UINT32_MAX`) pending an explicitly coordinated height |
| Regtest | Height 20, matching covenant script-path activation |

`UINT32_MAX` is a sentinel meaning “never active,” not a reachable activation
height.

Before activation, the legacy transition predicate remains selected so
historical validation does not silently acquire the successor rule. The
activation bit is included in script-cache keys before cache lookup, preventing
a pre-activation success from bypassing post-activation validation. Mempool
validation uses the current candidate-chain height, not the creation height of
the spent UTXO.

Assigning a mainnet or testnet height requires:

1. a chain-state proof that no incompatible CCV UTXOs or historical CCV spends
   exist;
2. coordinated deployment to all validating nodes;
3. end-to-end wallet construction and recovery tests;
4. activation-boundary, reorg, mempool, mining, and multi-node tests; and
5. external consensus and cryptographic review.

## 9. Golden vector

Given:

```text
tapscript = be51
merkleRoot =
  0102030405060708090a0b0c0d0e0f10
  1112131415161718191a1b1c1d1e1f20
prev.counter = 41
prev.data = 102030
next.counter = 42
next.data = 405060
```

The v1 results are:

```text
codeHash =
  bce3b94e7f9f1a041b490e366d98e384
  42cbcef077610b90c4e5a7b63a80c8f7
prev.stateHash =
  820f04fe93cdb43b27668a99fae6b47c
  1b1ca67258f59ecdeb2261d8615043ac
prev.internalKey =
  1110c456999cb753d39d73a8f57e0b0f
  669760e9ddafc15f339c5ee05a4216ee
prev.scriptPubKey =
  51202c06cfbb5f2149007203323d7cc7
  9fa8cfed6cfab865fb2809bcefac7603b507
prev.outputKeyParity = 0
next.stateHash =
  4488f5efa957c58b78aeffa4d81e02cd
  22ecde7b31c1e47b279eb1d4c203c5e0
next.scriptPubKey =
  5120a4bb46440cba36303dcbc734e5a6
  145340ae8448a057c770ff18bac86a3fa8dd
```

## 10. Reference implementation and tests

The consensus implementation is in:

- `include/consensus/covenants.h`
- `src/consensus/covenants.cpp`
- `src/consensus/tapscript_interpreter.cpp`
- `src/consensus/script_verify.cpp`
- `src/consensus/transaction_validator.cpp`

Adversarial and end-to-end vectors are in
`tests/consensus/test_ccv_successor_binding.cpp`.

Wallet/RPC contract-management surfaces are not made production-ready by this
consensus change. Their construction format and lifecycle behavior must be
ported to this specification and tested before activation.
