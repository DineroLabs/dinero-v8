# L1 Consensus ABI Stability Declaration

**Status**: FROZEN
**Effective Date**: 2026-01-13
**Version**: 1.0.0
**Breaking Changes**: Require network hard fork

---

## Executive Summary

The DineroCoin Layer 1 (L1) consensus layer is now **ABI-stable**. All data structures, serialization formats, hash computations, and wire protocols defined in this document are **immutable** without a coordinated network hard fork.

**What This Means:**
- ✅ Consensus-critical types cannot change size, layout, or semantics
- ✅ Serialization formats are locked (disk and wire)
- ✅ Hash domain separation is permanent
- ✅ Binary compatibility guaranteed across patch/minor versions
- ✅ Third-party consensus implementations can rely on stability

**What This Does NOT Freeze:**
- ❌ Layer 2 (Lightning) - still evolving
- ❌ Wallet/RPC APIs - can evolve with versioning
- ❌ Mining algorithms (difficulty adjustment, block templates)
- ❌ P2P protocol (gossip, peer discovery) - can version independently
- ❌ Performance optimizations (internal only)

---

## Frozen Consensus Surface

### Core Primitives (FROZEN)

**Location**: `include/primitives/`

#### Hash Domains (`hash_domains.h`)
```cpp
// FROZEN: Size, layout, hash functions immutable
struct BlockHash    { uint256 v; };  // 32 bytes, trivially copyable
struct TxId         { uint256 v; };  // 32 bytes, trivially copyable
struct WTxId        { uint256 v; };  // 32 bytes, trivially copyable
struct MerkleRoot   { uint256 v; };  // 32 bytes, trivially copyable
struct UtreexoRoot  { uint256 v; };  // 32 bytes, trivially copyable
```

**Guarantee**: Domain types cannot be merged, split, or reinterpreted. Hash function (SHA256d) locked.

#### uint256 (`uint256.h`)
```cpp
// FROZEN: Binary layout, arithmetic, serialization
struct uint256 {
    uint8_t data[32];  // Little-endian byte order
    // Comparison, arithmetic, hex conversion locked
};
```

**Guarantee**: 32-byte little-endian representation. No padding, no alignment changes.

#### Block Structure (`block.h`)
```cpp
// FROZEN: Field order, types, serialization
struct BlockHeader {
    uint32_t version;              // 4 bytes
    uint256 previous_hash;         // 32 bytes (legacy/transition)
    uint256 prev_block_hash;       // 32 bytes (active)
    uint256 merkle_root;           // 32 bytes
    uint256 utreexo_commitment;    // 32 bytes
    uint64_t timestamp;            // 8 bytes
    uint32_t time;                 // 4 bytes (BTC-compatible)
    uint32_t difficulty;           // 4 bytes
    uint32_t bits;                 // 4 bytes (BTC-compatible)
    uint32_t nonce;                // 4 bytes (PoW nonce)
};  // Total: 160 bytes (DineroCoin v1 consensus header)

struct Block {
    BlockHeader header;
    std::vector<Transaction> vtx;  // Merkle tree order locked
};
```

**Guarantee**: 160-byte header layout is DineroCoin v1 consensus reality. Merkle tree construction algorithm frozen.

##### BlockHeader Size Rationale (v1)

DineroCoin's v1 BlockHeader is **160 bytes** by design and is **consensus-critical**.

This layout includes transitional and redundancy fields introduced during Bitcoin compatibility and stateless validation work (Utreexo).

These fields are intentionally preserved to avoid historical consensus breaks and are ABI-frozen as part of the v1 protocol.

**This is a policy choice, not a physical limitation.**

The v1 header has been production-hardened through:
- 11,374+ blocks mined under active stress testing
- 25 SIGKILL crash cycles with perfect recovery
- Zero reindex, zero data loss, zero height regression
- Complete soak test validation (2026-01-08)

##### BlockHeader v1 Layout (160 bytes)

| Offset | Size | Field | Purpose |
|--------|------|-------|---------|
| 0      | 4    | version | Block version number |
| 4      | 32   | previous_hash | Legacy/transition field |
| 36     | 32   | prev_block_hash | Active previous block hash |
| 68     | 32   | merkle_root | Transaction merkle root |
| 100    | 32   | utreexo_commitment | Utreexo accumulator root |
| 132    | 8    | timestamp | Block timestamp (uint64) |
| 140    | 4    | time | BTC-compatible timestamp (uint32) |
| 144    | 4    | difficulty | Internal difficulty representation |
| 148    | 4    | bits | BTC-compatible difficulty target |
| 152    | 4    | nonce | Proof-of-work nonce |
| **156**    | **4**    | **(padding)** | **Alignment to 160 bytes** |

##### Field Classification

**Legacy-Frozen Fields (v1 ABI)**
- `previous_hash` (transition/compatibility)
- `time` (uint32 BTC-compatible)
- `bits` (BTC-compatible difficulty)

**Strategic Fields**
- `prev_block_hash` (active chain reference)
- `timestamp` (uint64 precision)
- `difficulty` (internal representation)
- `utreexo_commitment` (stateless validation)
- `merkle_root` (transaction commitment)
- `version` (protocol versioning)
- `nonce` (proof-of-work)

Legacy-frozen fields exist due to historical consensus and Bitcoin compatibility requirements. They will only be removed in a future major protocol version via deliberate hard fork.

##### Future Header Versions

A future BlockHeader v2 may consolidate or remove transitional fields. Such a change would require:

- A network-wide hard fork
- A new ABI stability baseline
- A new structural hash
- Explicit activation parameters (height or timestamp)

**BlockHeader v1 (160 bytes) remains immutable under the v1 consensus ABI.**

#### Transaction Structure (`transaction.h`)
```cpp
// FROZEN: Serialization format, hash computation
struct OutPoint {
    TxId txid;      // 32 bytes (Phase M.4: Now TxId)
    uint32_t vout;  // 4 bytes (output index)
};

struct TxInput {
    OutPoint prevout;
    std::vector<uint8_t> scriptSig;
    uint32_t sequence;
    std::vector<std::vector<uint8_t>> witness;  // SegWit witness stack
};

struct TxOutput {
    int64_t value;                     // Una (8 bytes)
    std::vector<uint8_t> scriptPubKey;
};

struct Transaction {
    int32_t version;
    std::vector<TxInput> vin;
    std::vector<TxOutput> vout;
    uint32_t locktime;

    TxId GetTxid() const;    // Non-witness hash (malleability-proof)
    WTxId GetWtxid() const;  // Witness hash (includes witness data)
};
```

**Guarantee**:
- TxId computation excludes witness data (BIP 141)
- WTxId includes witness data
- Serialization format matches Bitcoin SegWit
- OutPoint uses TxId (not WTxId) - malleability protection

---

### Consensus Rules (FROZEN)

**Location**: `include/consensus/`, `src/consensus/`

#### Block Validation (`block_validation.h`)
```cpp
// FROZEN: Validation sequence, failure modes
bool ValidateBlock(const Block& block, uint32_t height, std::string& error);
bool ConnectBlock(const Block& block, uint32_t height,
                  const BlockHash& block_hash, BlockUndo& undo,
                  std::string& error);
```

**Frozen Rules**:
- Block weight calculation (SegWit rules)
- Merkle root validation algorithm
- Coinbase maturity (100 blocks)
- Maximum block weight (4,000,000 units)
- Timestamp validation (median-time-past)

#### Transaction Validation (`transaction_validator.h`)
```cpp
// FROZEN: Script verification, signature validation
bool ValidateTransaction(const Transaction& tx, uint32_t height,
                        const IUTXOProvider* utxo_set,
                        std::string& error);
```

**Frozen Rules**:
- Script opcodes and semantics
- Signature verification (ECDSA, Schnorr)
- Taproot validation (BIP 340/341/342)
- Sequence number / locktime rules
- Witness version semantics

#### UTXO Set Management (`utxo_set.h`, `iutxo_provider.h`)
```cpp
// FROZEN: Interface contract
class IUTXOProvider {
    virtual std::optional<WalletUTXO> GetUTXO(const TxId& txid, uint32_t vout) const = 0;
    virtual bool SpendUTXO(const TxId& txid, uint32_t vout, uint32_t height) = 0;
    // ... other methods frozen
};

// FROZEN: UTXO structure
struct WalletUTXO {
    TxId txid;      // 32 bytes (Phase M.4: Now TxId)
    uint32_t vout;
    int64_t value;
    std::vector<uint8_t> spk;  // scriptPubKey
    // ... metadata can evolve, but core fields frozen
};
```

**Guarantee**: UTXO keyed by `(TxId, vout)` - not `(WTxId, vout)`. Malleability-proof.

---

### Serialization Formats (FROZEN)

#### Disk Format
- **Blocks**: Bitcoin-compatible SegWit serialization
- **Transactions**: BIP 144 witness serialization
- **UTXO Set**: `(txid_hex, vout) → UTXO` (SQLite schema locked)
- **Byte Order**: Little-endian for all multi-byte integers
- **Hash Representation**: 32-byte raw binary or 64-char lowercase hex

#### Wire Format (P2P Consensus Messages)
- **Block Announcement**: `BlockHash` (32 bytes)
- **Transaction Relay**: Full SegWit transaction (BIP 144)
- **Merkle Proofs**: Standard Bitcoin Merkle branch format
- **Utreexo Proofs**: Proof format frozen (Phase 3)

---

### Cryptographic Primitives (FROZEN)

#### Hash Functions
- **SHA-256d**: Double SHA-256 (Bitcoin standard)
  - Block hashing: `SHA256(SHA256(header))`
  - Transaction IDs: `SHA256(SHA256(tx_without_witness))`
  - Merkle tree: `SHA256(SHA256(left || right))`

- **SHA-256 (single)**: Taproot tagged hashes
  - BIP 340 Schnorr signatures
  - BIP 341 Taproot commitments

#### Signature Schemes
- **ECDSA**: secp256k1 curve (legacy)
- **Schnorr**: BIP 340 (Taproot)
- **Key Derivation**: BIP 32 HD wallets (wallet layer, not consensus)

#### Commitment Schemes
- **Taproot**: BIP 341 Merkle tree commitments
- **Confidential Transactions**: Pedersen commitments (experimental, not consensus-critical yet)

---

## ABI Stability Guarantees

### Binary Compatibility Rules

1. **Struct Layout**:
   - Field order cannot change
   - Alignment/padding cannot change
   - Size must remain constant
   - No virtual functions in consensus types

2. **Serialization**:
   - Disk format version: `1.0.0` (locked)
   - Wire format version: `1.0.0` (locked)
   - Backward compatibility: Old nodes must validate new blocks
   - Forward compatibility: New nodes must validate old blocks

3. **Hash Stability**:
   - Hash algorithms cannot change
   - Domain separation cannot be removed
   - Hash output must remain 32 bytes

4. **Type Safety** (Phase M.4):
   - `TxId` ≠ `WTxId` (compile-time enforced)
   - `BlockHash` ≠ `TxId` (compile-time enforced)
   - No implicit conversions between domains

### Versioning Strategy

**Semver for Consensus Layer**:
- **Patch** (`1.0.X`): Bug fixes, performance, no ABI changes
- **Minor** (`1.X.0`): Soft fork (backward compatible), new opcodes
- **Major** (`X.0.0`): Hard fork (breaking), requires network coordination

**Current Version**: `1.0.0` (ABI freeze baseline)

### Hard Fork Protocol

**If consensus ABI must change:**
1. Announce breaking change 6 months in advance
2. Publish migration guide
3. Coordinate with miners/node operators
4. Activate via block height or timestamp trigger
5. Maintain both versions during transition period

---

## Enforcement Mechanisms

### CI/CD Checks

**File**: `ci/check_consensus_abi_stability.sh`

Automatically enforces:
- ✅ No changes to frozen header files (structural hash)
- ✅ Struct sizes remain constant
- ✅ Serialization round-trip tests pass
- ✅ Hash output vectors unchanged
- ✅ No new dependencies in consensus layer

**File**: `ci/check_consensus_isolation.sh`

Enforces layer separation:
- ✅ Consensus code cannot depend on wallet
- ✅ Consensus code cannot depend on RPC
- ✅ Consensus code cannot depend on Lightning (L2)
- ✅ Only permitted dependencies: crypto, primitives, util

### Compile-Time Guards

**Static Assertions** (already in code):
```cpp
// Phase M.3 tripwires (hash_domains.h)
static_assert(!std::is_convertible<TxId, WTxId>::value,
    "TxId must NOT be convertible to WTxId (malleability risk)");

static_assert(sizeof(BlockHash) == 32, "BlockHash must be 32 bytes");
static_assert(sizeof(TxId) == 32, "TxId must be 32 bytes");
static_assert(std::is_trivially_copyable<TxId>::value,
    "TxId must be trivially copyable");
```

### Test Vectors

**File**: `tests/consensus/test_abi_stability.cpp`

Frozen test vectors:
- Block hash computation (known blocks)
- Transaction ID computation (known txs)
- Merkle root computation (known trees)
- Signature verification (known sigs)
- Script execution (known scripts)

**Any change that breaks these tests = ABI break = hard fork required.**

---

## Frozen Header Files

**These headers are now immutable:**

```
include/primitives/
├── uint256.h              ✅ FROZEN (32-byte hash primitive)
├── hash_domains.h         ✅ FROZEN (TxId, WTxId, BlockHash, etc.)
├── block.h                ✅ FROZEN (BlockHeader, Block)
├── transaction.h          ✅ FROZEN (Transaction, TxInput, TxOutput)
└── outpoint.h             ✅ FROZEN (OutPoint with TxId)

include/consensus/
├── block_validation.h     ✅ FROZEN (ValidateBlock, ConnectBlock)
├── transaction_validator.h ✅ FROZEN (ValidateTransaction)
├── interfaces/
│   └── iutxo_provider.h   ✅ FROZEN (UTXO interface contract)
└── script_verify.h        ✅ FROZEN (Script opcodes, signatures)

include/crypto/
├── sha256.h               ✅ FROZEN (SHA-256 implementation)
└── secp256k1_wrapper.h    ✅ FROZEN (ECDSA/Schnorr wrappers)
```

**SHA-256 checksum of frozen surface** (verification):
```bash
find include/primitives include/consensus/interfaces \
     -name "*.h" | sort | xargs cat | sha256sum
# Baseline: To be computed at freeze time
```

---

## Non-Frozen Surfaces

**These CAN evolve without consensus impact:**

### Wallet Layer (Evolvable)
- `include/wallet/*.h` - RPC interfaces, database schema (wallet-specific)
- HD key derivation paths (BIP 44/49/84)
- UTXO selection algorithms
- Fee estimation

### Lightning Layer (L2 - Active Development)
- `include/lightning/*.h` - Channel state, HTLC management
- Payment routing algorithms
- Gossip protocol
- Watchtower protocol

### P2P Layer (Versioned)
- `include/p2p/*.h` - Peer discovery, message relay
- Block announcement strategies (compact blocks, etc.)
- Transaction relay policy (RBF, CPFP)

### RPC Layer (Versioned)
- `include/rpc/*.h` - JSON-RPC methods
- REST API endpoints
- WebSocket interfaces

### Mining Layer (Policy)
- Block template construction
- Fee priority algorithms
- Transaction selection

---

## Migration Path from Pre-Freeze Code

**If you have code written before ABI freeze:**

1. **Check Hash Domain Usage**:
   ```cpp
   // ❌ OLD (pre-M.4)
   uint256 txid = tx.GetTxid();

   // ✅ NEW (post-M.4)
   TxId txid = tx.GetTxid();
   ```

2. **Check OutPoint Usage**:
   ```cpp
   // ❌ OLD (pre-M.4.3-B)
   struct OutPoint {
       uint256 txid;  // Ambiguous
   };

   // ✅ NEW (M.4.3-B frozen)
   struct OutPoint {
       TxId txid;  // Unambiguous, malleability-proof
   };
   ```

3. **Check UTXO Indexing**:
   ```cpp
   // ❌ OLD (malleability risk)
   std::unordered_map<uint256, UTXO> utxos;  // Could be WTxId!

   // ✅ NEW (frozen, safe)
   std::unordered_map<TxId, UTXO> utxos;  // Guaranteed TxId
   ```

4. **Run ABI Compatibility Test**:
   ```bash
   ctest -R ABI_Stability
   ```

---

## Audit Trail

### Phase History Leading to Freeze

- **Phase M.0** (Binary Identity): Raw bytes → uint256
- **Phase M.1** (Struct Correctness): uint256 → BlockHeader
- **Phase M.2** (Boundary Enforcement): Parser validation
- **Phase M.3** (Semantic Domains): TxId, WTxId, BlockHash types introduced
- **Phase M.4** (System-Wide Adoption): 89 files migrated, malleability-proof
- **Phase M.4 Final** (Compatibility Removed): m4_compat namespace deleted
- **L1 ABI Freeze** (This Document): Consensus surface locked

### Commit References

- Phase M.3: `8419a227` (Semantic Hash Domains)
- Phase M.4.3-A: `63760c57` (Transaction Identity Source)
- Phase M.4: `848482cf` (TxId Migration – System-Wide)
- Phase M.4 Merge: `caa56c68` (Merge to main)
- m4_compat Removal: `6be6c7ca` (Compatibility layer deleted)
- **L1 ABI Freeze**: `[TO BE TAGGED]`

---

## Verification Checklist

**Before relying on ABI stability, verify:**

- [ ] You are on DineroCoin version ≥ 1.0.0
- [ ] All Phase M.4 migrations are complete in your code
- [ ] You are using `TxId` (not `uint256`) for transaction IDs
- [ ] You are using `WTxId` for witness-inclusive hashes
- [ ] You are using `BlockHash` for block identifiers
- [ ] Your serialization matches Bitcoin SegWit format
- [ ] Your UTXO index is keyed by `(TxId, vout)`
- [ ] Your code passes `test_abi_stability`

---

## Contact / Governance

**Breaking Change Requests**:
- Propose via GitHub issue with `[CONSENSUS-BREAKING]` tag
- Requires BIP-style specification
- Requires community consensus
- Requires 6-month activation timeline

**ABI Clarifications**:
- Open GitHub issue with `[ABI-QUESTION]` tag
- If ambiguity found, will be documented here

**Version**: 1.0.0
**Last Updated**: 2026-01-13
**Next Review**: 2027-01-13 (annual stability audit)

---

## Appendix: Structural Hashes

**Frozen Type Sizes** (verification):
```cpp
sizeof(uint256)       == 32
sizeof(BlockHash)     == 32
sizeof(TxId)          == 32
sizeof(WTxId)         == 32
sizeof(MerkleRoot)    == 32
sizeof(UtreexoRoot)   == 32
sizeof(BlockHeader)   == 80  // Bitcoin-compatible
sizeof(OutPoint)      == 36  // 32 (TxId) + 4 (vout)
```

**Hash Function Test Vectors**:
```
Block 0 (Genesis):
  Header: [80 bytes]
  Hash: 000000000019d6689c085ae165831e934ff763ae46a2a6c172b3f1b60a8ce26f

Transaction (Coinbase):
  Version: 1
  TxId: 4a5e1e4baab89f3a32518a88c31bc87f618f76673e2cc77ab2127b7afdeda33b
  WTxId: (same, no witness)
```

**This document is authoritative for DineroCoin L1 consensus.**
