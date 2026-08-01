# CHECKTEMPLATEVERIFY BIP119 profile

Status: implemented for regtest review. Mainnet and testnet activation is
dormant (`UINT32_MAX`).

Opcode: `OP_CHECKTEMPLATEVERIFY` / `OP_NOP4` (`0xb3`).

Dinero adopts BIP119's `DefaultCheckTemplateVerifyHash` for transparent
transactions. This document fixes the project-specific applicability and
activation rules; it does not redefine BIP119.

## Template hash

The committed preimage is the concatenation below, followed by one SHA-256:

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

Counts and the input index are fixed-width little-endian values as specified
by BIP119. Script lengths use canonical CompactSize. Witness data, prevouts,
and input amounts are not part of this template hash.

The implementation is checked against selected upstream
`bip-0119/vectors/tx_valid.json` congestion-control vectors.

## Opcode behavior

- Before CTV activation, `0xb3` retains `OP_NOP4` behavior.
- After activation, an empty stack fails.
- A 32-byte top stack element must equal the template hash for the current
  input; the element remains on the stack.
- A non-32-byte element remains consensus NOP behavior, preserving the BIP119
  upgrade reservation. Relay policy may discourage that form.

## Dinero transaction extensions

BIP119 does not define commitments for Dinero's confidential outputs,
shielded transaction versions, or explicit-fee serialization. V1 rejects
those forms rather than assigning a custom hash with misleading BIP119
compatibility.

Supporting any of those forms requires a separately versioned specification,
vectors, implementation, activation flag, and independent review.

## Activation

| Chain | CTV |
|---|---:|
| Mainnet | dormant |
| Testnet | dormant |
| Regtest | 20 |

Activation is derived from the block or mempool validation height, not the
creation height of the spent coin. The height-derived flags are part of script
cache keys, and activation parameters are committed by the v2 consensus
checksum.

Boundary, reorg, mempool, mining, and adversarial-cost component coverage is
recorded in `COVENANT_PROTOCOL_STATUS.md` and
`COVENANT_RESOURCE_LIMITS.md`. Production activation still requires approved
wallet/recovery and live multi-node coverage, coordinated validator
deployment, and independent consensus review.
