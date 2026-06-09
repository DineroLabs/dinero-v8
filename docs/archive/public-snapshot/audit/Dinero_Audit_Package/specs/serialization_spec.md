# Confidential Transaction Serialization Specification

**Version:** 1.0
**Date:** 2025-01-17
**Status:** Implemented

---

## 1. Overview

This document specifies the binary serialization format for confidential transactions in DineroCoin. All network messages, block storage, and transaction relay use this format.

### 1.1 Design Goals

- **Deterministic:** Same transaction always serializes to same bytes
- **Compact:** Minimal overhead for confidential data
- **Backward Compatible:** Old nodes can parse transparent parts
- **Extensible:** Version field allows future upgrades

---

## 2. Basic Types

### 2.1 Primitive Types

```
uint8_t   - 1 byte unsigned integer
uint16_t  - 2 bytes unsigned integer (little-endian)
uint32_t  - 4 bytes unsigned integer (little-endian)
uint64_t  - 8 bytes unsigned integer (little-endian)
bool      - 1 byte (0x00 = false, 0x01 = true)
```

### 2.2 Variable-Length Types

**VarInt (Compact Size):**
```
Value           | Encoding
----------------|------------------
0-252           | 1 byte: value
253-65535       | 0xFD + 2 bytes LE
65536-2^32-1    | 0xFE + 4 bytes LE
2^32-2^64-1     | 0xFF + 8 bytes LE
```

**VarBytes:**
```
VarInt(length) || bytes[length]
```

Example: `0x05 0x48656c6c6f` = "Hello" (5 bytes)

---

## 3. Transaction Structure

### 3.1 Top-Level Format

```
Transaction {
    uint32_t version;
    VarInt   vin_count;
    TxInput  vin[vin_count];
    VarInt   vout_count;
    TxOutput vout[vout_count];
    uint32_t locktime;
}
```

**Size Calculation:**
```
min_size = 4 + 1 + 1 + 4 = 10 bytes (empty TX)
max_size = 500,000 bytes (consensus limit)
```

### 3.2 Transaction Version

```
version = 2  // Confidential transactions
version = 1  // Transparent only (legacy)
```

**Encoding:** 4 bytes little-endian
**Example:** `0x02000000` = version 2

---

## 4. Transaction Input

### 4.1 TxInput Format

```
TxInput {
    uint8_t  prevout_hash[32];  // Previous TX hash
    uint32_t prevout_index;      // Output index
    VarBytes script_sig;         // Signature script
    uint32_t sequence;           // Sequence number
}
```

**Fixed size:** 32 + 4 + 4 = 40 bytes (excluding script_sig)

### 4.2 Example

```
Previous TX: 0xabcd...1234 (32 bytes)
Output Index: 0
Script Sig: <sig> <pubkey> (73 + 33 = 106 bytes)
Sequence: 0xFFFFFFFF

Serialization:
  [32 bytes: hash]
  04 00 00 00           // index = 0
  6A                    // VarInt(106)
  [106 bytes: script]
  FF FF FF FF           // sequence
```

---

## 5. Transaction Output

### 5.1 Transparent Output

```
TxOutput (transparent) {
    uint64_t value;           // Amount in una
    VarBytes script_pubkey;   // Output script
    bool     is_confidential; // = 0x00
}
```

**Example:**
```
Value: 1000 una
Script: OP_DUP OP_HASH160 <pubkey_hash> OP_EQUALVERIFY OP_CHECKSIG

Serialization:
  E8 03 00 00 00 00 00 00  // value = 1000
  19                        // VarInt(25) - script length
  [25 bytes: script]
  00                        // is_confidential = false
```

### 5.2 Confidential Output

```
TxOutput (confidential) {
    uint64_t value;           // ALWAYS 0x0000000000000000
    VarBytes script_pubkey;   // Output script
    bool     is_confidential; // = 0x01
    uint8_t  commitment[33];  // Pedersen commitment
    VarBytes range_proof;     // Bulletproof (~714 bytes)
    uint8_t  nonce[65];       // ECDH nonce field
}
```

**Field Order is Critical:** Nodes parse in exact order.

### 5.3 Confidential Output Example

```
Value: 0 (confidential)
Script: OP_DUP OP_HASH160 <pubkey_hash> OP_EQUALVERIFY OP_CHECKSIG
Commitment: 0x02abcd...ef01 (33 bytes)
Range Proof: 714 bytes
Nonce: 65 bytes

Serialization:
  00 00 00 00 00 00 00 00  // value = 0 (MANDATORY)
  19                        // VarInt(25) - script length
  [25 bytes: script]
  01                        // is_confidential = true
  [33 bytes: commitment]    // Fixed size, no VarInt
  CA 02                     // VarInt(714) - proof length
  [714 bytes: proof]
  [65 bytes: nonce]         // Fixed size, no VarInt
```

**Total Overhead:** 33 + 714 + 65 = 812 bytes per confidential output

---

## 6. Commitment Encoding

### 6.1 Compressed Ristretto255 Point

Commitments are compressed Ristretto255 points:

```
commitment[0]      = 0x02 or 0x03  // Compression prefix
commitment[1..32]  = 32-byte Y-coordinate
```

**Valid Prefixes:**
- `0x02` - Even X-coordinate
- `0x03` - Odd X-coordinate

**Invalid:** Any other prefix is rejected at consensus.

### 6.2 Identity Point

The identity (zero) commitment is NOT allowed in outputs:

```
REJECT: 0x0000000000000000000000000000000000000000000000000000000000000000
```

**Enforcement:** `src/consensus/confidential_validation.cpp:353`

---

## 7. Range Proof Encoding

### 7.1 Rewindable Proof Structure

```
Offset | Size  | Field
-------|-------|---------------------------
0      | 8     | encrypted_value (uint64_t LE)
8      | 32    | encrypted_blinding (bytes)
40     | ~674  | bulletproof (variable)
-------|-------|---------------------------
Total: ~714 bytes
```

### 7.2 Encoding

```
proof_bytes[0..7]   = encrypted_value (little-endian uint64)
proof_bytes[8..39]  = encrypted_blinding (32 bytes)
proof_bytes[40..]   = serialized Bulletproof
```

**Length Validation:**
- Minimum: 690 bytes (40 + 650)
- Maximum: 840 bytes (40 + 800)
- Typical: 714 bytes (40 + 674)

**Consensus Rule:** Reject if `proof_len < 650 || proof_len > 800`

---

## 8. Nonce Field Encoding

### 8.1 Structure

```
nonce[0..32]   = ephemeral_pubkey (33 bytes compressed secp256k1)
nonce[33..64]  = encrypted_blinding (32 bytes)
```

**Total:** Exactly 65 bytes (fixed size)

### 8.2 Ephemeral Public Key Format

```
ephemeral_pubkey[0]      = 0x02 or 0x03  // secp256k1 compression
ephemeral_pubkey[1..32]  = 32-byte X-coordinate
```

**Note:** Uses secp256k1 curve (NOT Ristretto255)

### 8.3 Encrypted Blinding Format

```
encrypted_blinding = blinding_factor ⊕ SHA256("dinero_blind_key" || ECDH_nonce)
```

32 bytes of XOR-encrypted data, no special structure.

---

## 9. Complete Transaction Example

### 9.1 Transparent Transaction (Version 1)

```
Hex:
01000000              // version = 1
01                    // 1 input
[32 bytes: prevout hash]
00000000              // prevout index = 0
6A                    // script_sig length = 106
[106 bytes: script_sig]
FFFFFFFF              // sequence
01                    // 1 output
E803000000000000      // value = 1000
19                    // script_pubkey length = 25
[25 bytes: script_pubkey]
00                    // is_confidential = false
00000000              // locktime = 0

Total Size: ~180 bytes
```

### 9.2 Confidential Transaction (Version 2)

```
Hex:
02000000              // version = 2
01                    // 1 input
[32 bytes: prevout hash]
00000000              // prevout index = 0
6A                    // script_sig length = 106
[106 bytes: script_sig]
FFFFFFFF              // sequence
01                    // 1 output
0000000000000000      // value = 0 (MANDATORY for confidential)
19                    // script_pubkey length = 25
[25 bytes: script_pubkey]
01                    // is_confidential = true
[33 bytes: commitment]
CA02                  // proof length = 714 (VarInt)
[714 bytes: range_proof]
[65 bytes: nonce]
00000000              // locktime = 0

Total Size: ~990 bytes
```

---

## 10. Hash Calculation

### 10.1 Transaction ID (TXID)

```
TXID = SHA256(SHA256(serialized_tx))
```

**Includes:** ALL transaction fields including proofs

**Byte Order:** TXID is reversed for display (RPC shows big-endian)

### 10.2 Signature Hash

For signing inputs:

```
SigHash = SHA256(SHA256(
    version ||
    prevout_hash || prevout_index ||
    script_pubkey ||  // From previous output
    value ||          // From previous output (0 if confidential)
    sequence ||
    outputs_hash ||   // Hash of all outputs
    locktime
))
```

**Note:** Confidential outputs use `value = 0` in signature hash.

---

## 11. Network Messages

### 11.1 TX Message

```
Message {
    uint32_t magic;      // Network magic bytes
    char[12] command;    // "tx\0\0\0\0\0\0\0\0\0\0"
    uint32_t length;     // Payload length
    uint32_t checksum;   // First 4 bytes of SHA256(SHA256(payload))
    bytes    payload;    // Serialized transaction
}
```

### 11.2 Size Limits

```
MAX_TX_SIZE = 500,000 bytes  // Consensus limit
MAX_MESSAGE_SIZE = 32 MB     // Network layer limit
```

**Enforcement:**
- Mempool: Reject TX > 500 KB
- Network: Reject message > 32 MB

---

## 12. Block Serialization

### 12.1 Block Header

```
BlockHeader {
    uint32_t version;
    uint8_t  prev_block_hash[32];
    uint8_t  merkle_root[32];
    uint32_t timestamp;
    uint32_t bits;           // Difficulty target
    uint32_t nonce;
}
```

**Size:** Exactly 80 bytes

### 12.2 Block Body

```
Block {
    BlockHeader header;
    VarInt      tx_count;
    Transaction txs[tx_count];
}
```

### 12.3 Merkle Tree

Transaction IDs are hashed into Merkle tree:

```
merkle_root = merkle_tree_root([tx1.txid, tx2.txid, ..., txN.txid])
```

**Includes:** Confidential transaction TXIDs (with full proof data in hash)

---

## 13. Parsing Algorithm

### 13.1 Safe Parsing Steps

```cpp
bool ParseTransaction(const uint8_t* data, size_t len, Transaction* tx) {
    BufferReader reader(data, len);

    // 1. Read version
    if (!reader.Read(&tx->version, 4)) return false;

    // 2. Read inputs
    uint64_t vin_count;
    if (!reader.ReadVarInt(&vin_count)) return false;
    if (vin_count > 10000) return false;  // DoS protection

    for (uint64_t i = 0; i < vin_count; i++) {
        TxInput input;
        if (!ParseTxInput(reader, &input)) return false;
        tx->vin.push_back(input);
    }

    // 3. Read outputs
    uint64_t vout_count;
    if (!reader.ReadVarInt(&vout_count)) return false;
    if (vout_count > 10000) return false;  // DoS protection

    for (uint64_t i = 0; i < vout_count; i++) {
        TxOutput output;

        // Read value
        if (!reader.Read(&output.value, 8)) return false;

        // Read script
        uint64_t script_len;
        if (!reader.ReadVarInt(&script_len)) return false;
        if (script_len > 10000) return false;  // DoS
        if (!reader.ReadBytes(&output.script_pubkey, script_len)) return false;

        // Read confidential flag
        uint8_t is_conf;
        if (!reader.Read(&is_conf, 1)) return false;
        output.is_confidential = (is_conf == 0x01);

        if (output.is_confidential) {
            // Validate value is 0
            if (output.value != 0) return false;  // CON-01

            // Read commitment (fixed 33 bytes)
            if (!reader.ReadBytes(&output.commitment, 33)) return false;

            // Read range proof
            uint64_t proof_len;
            if (!reader.ReadVarInt(&proof_len)) return false;
            if (proof_len < 650 || proof_len > 800) return false;  // CON-04
            if (!reader.ReadBytes(&output.range_proof, proof_len)) return false;

            // Read nonce (fixed 65 bytes)
            if (!reader.ReadBytes(&output.nonce, 65)) return false;
        }

        tx->vout.push_back(output);
    }

    // 4. Read locktime
    if (!reader.Read(&tx->locktime, 4)) return false;

    // 5. Verify no extra data
    if (reader.BytesRemaining() != 0) return false;

    return true;
}
```

### 13.2 Error Handling

**On Parse Failure:**
1. Reject transaction
2. Score peer negatively
3. Log error with details
4. Do NOT crash or throw exception

---

## 14. Serialization Validation

### 14.1 Consensus Checks

After parsing, validate:

```cpp
bool ValidateSerialization(const Transaction& tx) {
    // 1. Check total size
    size_t tx_size = tx.GetSerializedSize();
    if (tx_size > 500000) return false;  // CON-10

    // 2. Check output count
    size_t conf_count = 0;
    for (auto& out : tx.vout) {
        if (out.is_confidential) conf_count++;
    }
    if (conf_count > 100) return false;  // CON-08

    // 3. Check total proof data
    size_t total_proof = 0;
    for (auto& out : tx.vout) {
        if (out.is_confidential) {
            total_proof += out.range_proof.size();
        }
    }
    if (total_proof > 100000) return false;  // CON-09

    // 4. Validate each confidential output
    for (auto& out : tx.vout) {
        if (!out.is_confidential) continue;

        // CON-01: Value must be 0
        if (out.value != 0) return false;

        // CON-02: Commitment size
        if (out.commitment.size() != 33) return false;

        // CON-03: Commitment prefix
        uint8_t prefix = out.commitment[0];
        if (prefix != 0x02 && prefix != 0x03) return false;

        // CON-04: Proof size
        size_t plen = out.range_proof.size();
        if (plen < 650 || plen > 800) return false;

        // CON-06: Nonce size
        if (out.nonce.size() != 65) return false;

        // CON-07: Ephemeral pubkey prefix
        uint8_t nonce_prefix = out.nonce[0];
        if (nonce_prefix != 0x02 && nonce_prefix != 0x03) return false;
    }

    return true;
}
```

---

## 15. Deterministic Serialization

### 15.1 Canonical Encoding Rules

To ensure same TX always hashes to same TXID:

1. **VarInt Encoding:** Use minimal encoding
   - ❌ WRONG: `0xFD 05 00` for value 5
   - ✅ RIGHT: `0x05`

2. **Field Order:** Never reorder fields

3. **No Padding:** No extra null bytes

4. **Fixed Sizes:** commitment (33), nonce (65) always exact

### 15.2 Test Vector

```json
{
  "description": "Canonical serialization test",
  "transaction": {
    "version": 2,
    "inputs": [{
      "prevout": "0xabcd...1234",
      "index": 0,
      "script_sig": "0x...",
      "sequence": 4294967295
    }],
    "outputs": [{
      "value": 0,
      "script_pubkey": "0x76a914...88ac",
      "is_confidential": true,
      "commitment": "0x02abcd...ef01",
      "range_proof": "0x...",
      "nonce": "0x..."
    }],
    "locktime": 0
  },
  "expected_serialization": "0x020000000...",
  "expected_txid": "0xabcd1234..."
}
```

---

## 16. Backward Compatibility

### 16.1 Old Node Behavior

**Version 1 nodes:**
- Parse `version`, `vin`, `vout` (partially)
- Stop parsing at `is_confidential = 0x01`
- Reject transaction as "unknown format"

**Result:** Soft fork - old nodes reject confidential TXs

### 16.2 Mixed Transaction Support

Transactions can have BOTH transparent and confidential outputs:

```
vout[0] = transparent (is_confidential = 0x00)
vout[1] = confidential (is_confidential = 0x01)
```

**Parsing:** Each output checked independently

---

## 17. Security Considerations

### 17.1 DoS Prevention

**Size Limits:**
```cpp
MAX_TX_SIZE = 500 KB
MAX_SCRIPT_SIZE = 10 KB
MAX_PROOF_SIZE = 800 bytes
MAX_INPUT_COUNT = 10,000
MAX_OUTPUT_COUNT = 10,000
```

**Enforcement:** Reject before allocating memory

### 17.2 Buffer Overflow Prevention

```cpp
// WRONG - Vulnerable
uint64_t len;
reader.ReadVarInt(&len);
uint8_t* buffer = malloc(len);  // ❌ Attacker controls len!

// RIGHT - Safe
uint64_t len;
reader.ReadVarInt(&len);
if (len > MAX_SAFE_SIZE) return ERROR;
uint8_t* buffer = malloc(len);  // ✅ Bounded
```

### 17.3 Malleability

**Non-Malleable Fields:**
- Commitment (fixed point encoding)
- Range proof (cryptographically binding)
- Nonce (ephemeral pubkey binding)

**Malleable Fields:**
- Script signatures (standard Bitcoin malleability)

**SegWit Compatibility:** Future upgrade can address signature malleability

---

## 18. Test Vectors

### 18.1 Minimal Confidential TX

```
Version: 2
Inputs: 1 (minimal)
Outputs: 1 confidential
Locktime: 0

Hex: 02000000 01 [prevout] 00 FFFFFFFF 01 0000000000000000 00 01 [33:commitment] [714:proof] [65:nonce] 00000000

Size: ~850 bytes
```

### 18.2 Maximum Confidential TX

```
Inputs: 100
Outputs: 100 confidential
Total proof data: 100 KB
Total size: ~499 KB (under 500 KB limit)
```

---

## 19. References

1. **Bitcoin Serialization:** https://en.bitcoin.it/wiki/Protocol_documentation
2. **VarInt Encoding:** BIP-0141 (SegWit)
3. **Ristretto Encoding:** https://ristretto.group/formulas/encoding.html
4. **secp256k1 Compression:** SEC 2 Standard

---

**End of Specification**
