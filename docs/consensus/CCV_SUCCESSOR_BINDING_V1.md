# CCV successor binding v1

Status: implemented for regtest review. Mainnet and testnet activation is
dormant (`UINT32_MAX`).

Opcode: `OP_CHECKCONTRACTVERIFY` (`0xbe`).

This is a Dinero-specific consensus protocol. Its security does not follow
from BIP341/BIP342 merely because it uses Taproot commitments.

## State encoding

A witness state is encoded exactly as:

| Field | Size | Encoding |
|---|---:|---|
| `stateHash` | 32 bytes | raw |
| `codeHash` | 32 bytes | raw |
| `counter` | 4 bytes | unsigned little-endian |
| `dataLen` | 4 bytes | unsigned little-endian |
| `data` | `dataLen` | raw |

No trailing bytes are permitted. `data` is limited to 448 bytes so the whole
state fits the 520-byte tapscript stack-element limit.

Commitments:

```text
codeHash  = SHA256(revealed_tapscript)
stateHash = SHA256(codeHash || counter_le32 || data)
```

## State-derived internal key

For retry values beginning at zero:

```text
candidate = TaggedHash(
    "Dinero/CCVInternalKey/v1",
    stateHash || retry_le32
)
```

The first candidate accepted as an x-only secp256k1 public key is the internal
key. This is hash-to-x, not a publicly known scalar multiplied by `G`.

Given the already authenticated Taproot Merkle root `m`, the state output is:

```text
t = TaggedHash("TapTweak", internalKey || m)
P = internalKey + t*G
scriptPubKey = OP_1 PUSH32 xonly(P)
```

The control-block parity bit must equal the parity of `P`.

## Verification rule

For CCV at input index `i`, with witness states `previous` and `next`, the
opcode succeeds only if:

1. `i` exists in the inputs, outputs, and complete spent-UTXO vector.
2. Both state encodings, state hashes, and size limits are valid.
3. `previous.counter != UINT32_MAX`.
4. `next.counter == previous.counter + 1`.
5. `next.codeHash == previous.codeHash`.
6. `previous.codeHash == SHA256(revealed_tapscript)`.
7. The authenticated control-block internal key and parity match `previous`.
8. The spent script is exactly the P2TR output derived from `previous` and the
   authenticated Merkle root.
9. The spent output is transparent.
10. `tx.vout[i]` is transparent and preserves the spent value exactly.
11. `tx.vout[i]` is exactly the P2TR output derived from `next` under the same
    Merkle root.
12. No other output has the same successor script.

The index mapping separates multiple CCV inputs. Exact value preservation
means a CCV coin cannot pay its own fee; a separate input must fund fees.

CCV proves state continuity, not application business rules. The tapscript
must constrain which new `data` is permitted. A deliberate terminal path must
use another authenticated leaf that does not execute CCV.

## Confidential outputs

V1 rejects confidential inputs and successors. Equality of Pedersen
commitments is not a value-equality proof when blinders can differ. Supporting
confidential CCV requires a separately specified proof and activation.

## Activation

| Chain | Script path | CTV | CCV | CSFS / TXHASH |
|---|---:|---:|---:|---:|
| Mainnet | 1 | dormant | dormant | dormant |
| Testnet | 200 | dormant | dormant | dormant |
| Regtest | 20 | 20 | 20 | dormant |

Before its activation, `0xbe` retains BIP342 `OP_SUCCESS` semantics. There is
no weak, partially bound CCV mode. Activation heights are committed by the
chain-parameter consensus checksum and height-derived flags are included in
script-cache keys.

Activation-boundary, reorg, mempool, mining, and restart component coverage is
recorded in `COVENANT_PROTOCOL_STATUS.md`. Assigning a production activation
height still requires approved wallet recovery and live multi-node tests,
coordinated deployment, and independent cryptographic and consensus review.

## Golden vector

For tapscript `be51`, Merkle root
`0102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f20`,
previous counter/data `41/102030`, and next counter/data `42/405060`:

```text
codeHash =
  bce3b94e7f9f1a041b490e366d98e38442cbcef077610b90c4e5a7b63a80c8f7
previous.stateHash =
  820f04fe93cdb43b27668a99fae6b47c1b1ca67258f59ecdeb2261d8615043ac
previous.internalKey =
  1110c456999cb753d39d73a8f57e0b0f669760e9ddafc15f339c5ee05a4216ee
previous.scriptPubKey =
  51202c06cfbb5f2149007203323d7cc79fa8cfed6cfab865fb2809bcefac7603b507
next.stateHash =
  4488f5efa957c58b78aeffa4d81e02cd22ecde7b31c1e47b279eb1d4c203c5e0
next.scriptPubKey =
  5120a4bb46440cba36303dcbc734e5a6145340ae8448a057c770ff18bac86a3fa8dd
```
