# Dinero Miner ↔ Daemon Interface Specification (v1)

**External Mining Protocol**
**Utreexo Enabled by Default (Activation Height = 2)**

---

## 0. Scope & Intent

This document defines the authoritative and mandatory protocol contract between:

- **Daemon**: `dinerod` — the sole consensus authority
- **Miner**: `dinero-miner` — an external, untrusted block producer

This specification governs:

- Mining template generation
- Block assembly
- Proof-of-Work execution
- Utreexo accumulator commitment
- Block submission and validation

**This document is consensus-critical.**

---

## 1. Consensus Activation Rule

### Utreexo Activation

- Utreexo is **enabled by default**
- **Activation height**: block 2

For all blocks with `height >= 2`, the following rules apply:

- A valid Utreexo commitment is **REQUIRED**
- Blocks missing or altering the commitment are **consensus-invalid**
- Miners MUST include the commitment exactly as provided by the daemon

Block 0 (genesis) and block 1 (premine) are exempt.

---

## 2. Trust Model (Strict)

### Daemon (Authoritative)

The daemon is the only trusted component and is responsible for:

- Consensus rules
- Chainstate
- Difficulty
- UTXO validation
- Utreexo accumulator state
- Block acceptance and rejection

### Miner (Untrusted)

The miner:

- Has no chainstate
- Has no Utreexo state
- Cannot validate spends
- Cannot compute accumulator roots
- Can only propose blocks

**Miner compromise MUST NOT compromise consensus.**

---

## 3. Transport & Authentication

### Transport

- JSON-RPC over HTTP
- Loopback or LAN
- No shared memory, no IPC shortcuts

### Authentication

- Cookie-based authentication
- Miner reads: `~/.dinero/.cookie`
- Daemon validates the cookie on every RPC call

---

## 4. Required RPC Calls

### 4.1 `getblocktemplate`

**Purpose**: Request an authoritative mining template.

**Method names** (both supported):
- `getblocktemplate`
- `mining.getblocktemplate`

**Required parameters**:
```json
{
  "address": "din1..."
}
```

**Daemon guarantees**:

- Template reflects the current best chain tip
- Difficulty (bits) is correct
- For `height >= 2`:
  - A valid Utreexo commitment is included
  - All included transactions are fully Utreexo-verifiable

**Miner guarantees**:

- Treats all template fields as read-only
- Uses `prevhash`, `bits`, `height`, and Utreexo fields verbatim
- Assumes the template may become stale at any time

### 4.2 `submitblock`

**Purpose**: Submit a fully assembled block candidate.

**Daemon guarantees**:

- Performs full validation:
  - POW
  - Merkle root
  - Coinbase correctness
  - UTXO rules
  - Utreexo validation
- Accepts or rejects atomically

**Miner guarantees**:

- Submits only fully serialized blocks
- Does not retry rejected blocks blindly
- Fetches a new template after rejection

---

## 5. Block Assembly Rules (Miner)

### Miner MAY

- Construct the coinbase transaction
- Increment extranonce
- Adjust timestamp within consensus limits
- Search the nonce space freely

### Miner MUST NOT

- Modify difficulty
- Modify previous block hash
- Modify block height
- Modify or recompute Utreexo commitments
- Invent consensus fields

---

## 6. Coinbase Transaction Rules

### Mandatory Coinbase Fields

- Encoded block height (BIP34)
- Miner-controlled extranonce
- Miner payout script

---

## 7. Utreexo Commitment (DineroCoin-Specific)

### Placement: Block Header

DineroCoin places the Utreexo commitment directly in the **block header** at a fixed offset.

This is a deliberate design choice with the following properties:

- Commitment is always present (cannot be omitted)
- Checked before transaction validation
- No coinbase parsing ambiguity
- Cleaner for light clients
- Supports future AssumeUTXO / snapshot sync

**Header Layout** (128 bytes):

| Offset | Size | Field |
|--------|------|-------|
| 0x00   | 4    | version |
| 0x04   | 32   | prev_block_hash |
| 0x24   | 32   | merkle_root |
| 0x44   | 32   | **utreexo_root** |
| 0x64   | 8    | timestamp |
| 0x6C   | 4    | difficulty (bits) |
| 0x70   | 4    | nonce |
| 0x74   | 12   | reserved |

**Utreexo commitment offset**: 68 (0x44)

### Template Response Format

For blocks with `height >= 2`, `getblocktemplate` returns:

```json
{
  "height": 123,
  "previousblockhash": "...",
  "bits": "1d31ffce",
  "target": "00000031ffce...",
  "coinbasevalue": 10000000000,
  "utreexo": {
    "enabled": true,
    "required": true,
    "activation_height": 2,
    "commitment": "9d7c60e00ed90bcd37652318ae7d3cb1c18160bc400c0af1bab9a747ab1ba75d",
    "placement": "block_header",
    "commitment_offset": 68,
    "commitment_version": 1
  },
  "rules": ["csv", "segwit", "utreexo"],
  ...
}
```

### Miner Rules

- MUST include the commitment verbatim at header offset 68
- MUST reject the template if commitment is missing (height >= 2)
- MUST NOT attempt to compute or validate accumulator state
- SHOULD refuse to mine if commitment is all zeros at height >= 2

---

## 8. Merkle Root & Endianness (Consensus-Critical)

### Rules

- Hashes are little-endian internally
- Merkle root is written to the block header **without reversal**
- For a single-transaction block: `merkle_root == coinbase_txid`

**Any byte reversal of the merkle root is consensus-invalid.**

---

## 9. Determinism Guarantees

For a given template:

### Immutable

- `prevhash`
- `bits`
- `height`
- Utreexo commitment

### Mutable

- `nonce`
- `timestamp` (bounded)
- `extranonce` (coinbase)

All miners operate over the same header space.

### RPC Determinism Invariant

**Nodes MUST return identical Utreexo commitments for identical chainstate across repeated and concurrent `getblocktemplate` calls.**

This invariant is enforced by CI and guarantees:

- No thread races in Utreexo accumulator
- No nondeterminism in template generation
- Safe parallel mining operations
- Reproducible block templates across nodes

---

## 10. Failure Semantics

### Valid Rejection Reasons

| Error | Meaning |
|-------|---------|
| `bad-prevblk` | Previous block hash mismatch |
| `bad-diffbits` | Difficulty mismatch |
| `bad-txnmrklroot` | Merkle root mismatch |
| `bad-utreexo-commitment` | Invalid Utreexo commitment |
| `utxo-not-found` | Referenced UTXO doesn't exist |
| `duplicate` | Block already exists |
| `stale-template` | Template outdated |

### Miner Behavior

1. Log rejection reason
2. Fetch new template
3. Continue mining

**Retrying rejected blocks is forbidden.**

---

## 11. Daemon Obligations (Utreexo)

The daemon MUST:

- Maintain authoritative Utreexo accumulator state
- Validate all template transactions under Utreexo rules
- Reject blocks with invalid or missing commitments
- NEVER include transactions it cannot validate

**Violation of this rule is a daemon consensus bug.**

---

## 12. Safety Checks (Recommended)

Miners SHOULD verify before mining:

- Network matches expected genesis
- `height >= 2` implies Utreexo commitment present
- Commitment is non-zero for `height >= 2`
- Daemon reports healthy state

---

## 13. Canonical Summary

```
Utreexo:
  - enabled by default
  - mandatory from height 2
  - placement: block_header (offset 68)

Miner:
  - external
  - untrusted
  - RPC-only
  - includes daemon-provided commitment verbatim

Daemon:
  - authoritative
  - owns Utreexo state
  - validates everything
```

---

## Revision History

| Version | Date | Changes |
|---------|------|---------|
| v1.0 | 2025-01-29 | Initial specification |
| v1.1 | 2025-01-29 | Added RPC Determinism Invariant (Section 9) |

---

*This design makes Utreexo first-class consensus, keeps miners stateless and replaceable, prevents accumulator divergence, and aligns with long-term light-client and proof goals.*
