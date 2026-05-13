# DineroCoin Consensus Rules (Frozen)

**Status**: LOCKED for Phase G (P2P Networking)
**Last Updated**: December 29, 2025
**Version**: 1.0 (Pre-P2P Lockdown)

---

## 🔒 Purpose

This document defines DineroCoin's consensus rules that are **frozen** before P2P networking begins. Once nodes communicate, changing these rules requires a hard fork.

**Audience**: Protocol developers, node operators, auditors

---

## 1. Script Validation Model

### 1.1 Consensus Path (Block Validation)

**Authority**: `consensus::ValidateSpend()` in `src/consensus/script_validation.cpp`

**Design**: Explicit validators per script type (NO opcode VM)

**Supported Script Types**:
- ✅ **P2PKH** (Pay-to-PubKey-Hash) - Legacy ECDSA validation
- ✅ **P2TR** (Pay-to-Taproot) - Key-path Schnorr validation (BIP340)
- ⏸️ **P2WPKH** - Phase G (future)
- ⏸️ **Tapscript** - Phase G (future)

**Validation Flow**:
```
BlockValidator::ValidateTransaction()
  └─> ValidateSpend(tx, input_index, utxo, height)
       └─> DetectScriptType(scriptPubKey)
            ├─> P2PKH  → ValidateLegacySpend()  → VerifyECDSASignature()
            ├─> P2TR   → ValidateTaprootSpend() → VerifySchnorrSignature()
            └─> UNKNOWN → UNSUPPORTED_SCRIPT (reject)
```

**VerifyScript Status**:
- ❌ **NOT used** in consensus block validation (deprecated for this purpose)
- ✅ **Still used** in mempool acceptance (policy layer)
- ✅ **Still used** in transaction validation (non-consensus paths)

**Rationale**: Explicit validation eliminates hidden state, flag dependencies, and non-deterministic behavior. Each script type has a dedicated validator with clear semantics.

**Frozen**: YES - Adding new script types requires network coordination

---

## 2. Chain Selection Rules

### 2.1 Fork Choice Algorithm

**Authority**: `ChainstateService::ActivateBestChain()` in `src/daemon/services/chainstate_service.cpp`

**Rule**: Most cumulative chainwork wins

**Algorithm**:
```cpp
int work_cmp = chainwork::CompareWork(
    best_candidate->chainwork,
    active_tip_->chainwork
);

if (work_cmp > 0) {
    // Candidate has more work → activate
} else if (work_cmp == 0 && best_candidate->hash < active_tip_->hash) {
    // Equal work → lower hash wins (tie-breaker)
}
```

**Tie-Breaker**: Lexicographically smaller block hash (deterministic)

**Chainwork Definition**: Sum of difficulty from genesis to tip

**NOT Used**: Block height (incorrect - leads to longest-chain attacks)

**Frozen**: YES - Changing fork choice is a hard fork

---

### 2.2 Reorg Semantics

**Authority**:
- `ChainstateService::DisconnectTip()` - UTXO rollback
- `ChainstateService::ConnectTip()` - UTXO mutation

**Atomicity**: All-or-nothing (disconnect → connect sequence)

**UTXO Mutation**:
- **DisconnectBlock**: Restores spent UTXOs from `BlockUndo` data
- **ConnectBlock**: Applies new UTXOs, creates `BlockUndo` for future reorgs

**Undo Data**:
- **Storage**: ChainDB (`UndoRecord` → RocksDB)
- **Format**: `BlockUndo` → `UndoRecord` conversion
- **Persistence**: Written during `ConnectTip`, read during `DisconnectTip`
- **Reusability**: Undo data is NOT consumed (can be replayed multiple times)

**Wallet Notifications**: Happen AFTER UTXO state is consistent

**Frozen**: YES - Reorg semantics must be deterministic

---

## 3. Coinbase Maturity

### 3.1 Maturity Rules

**Maturity Depth**: 100 blocks (Bitcoin-compatible)

**Rule**: Coinbase outputs cannot be spent until 100 confirmations

**Enforcement Points**:
1. **Block Validation**: `BlockValidator::ValidateTransaction()` checks maturity
2. **Mempool Acceptance**: Rejects transactions spending immature coinbase
3. **Mining**: `CreateNewBlock()` excludes immature spends

**Height Calculation**:
```cpp
uint32_t confirmations = current_height - utxo.height;
bool is_mature = confirmations >= COINBASE_MATURITY;  // 100
```

**Edge Case**: Genesis coinbase is immediately mature (height 0 special case)

**Frozen**: YES - Changing maturity depth is a hard fork

---

## 4. Block Structure

### 4.1 Block Header

**Size**: 128 bytes (80-byte Bitcoin header + 32-byte Utreexo root + 4 extra bytes for 64-bit timestamp + 12-byte reserved)

**Fields**:
```cpp
struct BlockHeader {
    uint32_t version;          // 4 bytes
    uint256 prev_block_hash;   // 32 bytes
    uint256 merkle_root;       // 32 bytes
    uint256 utreexo_root;      // 32 bytes (AFTER-state accumulator)
    uint64_t timestamp;        // 8 bytes (Unix timestamp, 64-bit)
    uint32_t difficulty;       // 4 bytes (difficulty target)
    uint32_t nonce;            // 4 bytes
    uint8_t  reserved[12];     // 12 bytes (MUST be zero)
};
```

**Byte Layout (explicit ranges)**:

| Field           | Offset | Size | End Byte |
|-----------------|--------|------|----------|
| version         | 0      | 4    | 3        |
| prev_block_hash | 4      | 32   | 35       |
| merkle_root     | 36     | 32   | 67       |
| utreexo_root    | 68     | 32   | 99       |
| timestamp       | 100    | 8    | 107      |
| difficulty      | 108    | 4    | 111      |
| nonce           | 112    | 4    | 115      |
| reserved        | 116    | 12   | 127      |

**Hash Calculation**: SHA256d(all 128 bytes) - Utreexo root included in PoW, reserved must be zero

**Frozen**: YES - Changing header format is a hard fork

---

### 4.2 Transaction Structure

**Format**: Bitcoin-compatible with witness data

**Fields**:
```cpp
struct Transaction {
    int32_t version;
    std::vector<TxInput> vin;
    std::vector<TxOutput> vout;
    std::vector<std::vector<uint8_t>> witness;  // Per-input witness
    uint32_t locktime;
};
```

**Txid Calculation**: SHA256d(version || vin || vout || locktime)
**Witness NOT included** in txid (SegWit-compatible)

**Frozen**: YES - Changing transaction format is a hard fork

---

### 4.3 Utreexo Accumulator (Mandatory)

**Authority**: `consensus/utreexo_activation.h`, `consensus/features.h`

**Status**: **MANDATORY** - Utreexo is a consensus component activated at height 2 as part of genesis-era rules. No alternative validation mode exists.

**Activation Heights**:
| Network | Height | Rationale |
|---------|--------|-----------|
| Regtest | 0 | Immediate for testing |
| Testnet | 2 | Match mainnet for realistic testing |
| Mainnet | 2 | First normal block (post-premine) |

**Enforcement**:
- Block 0 (genesis): No enforcement
- Block 1 (premine): No enforcement
- **Block 2+**: Utreexo root REQUIRED in header, proofs REQUIRED for UTXO spends

**Design Rationale**:
- New chain with no legacy compatibility requirements
- Avoids future soft-fork coordination complexity
- Phase 4 delta-based undo verified via audit (2026-01-20)
- O(log n) UTXO verification from genesis

**Implementation Requirements**:
- `CONSENSUS_UTREEXO_PHASE4_DELTA_UNDO = true` (required, enforced by static_assert)
- Blocks with UTXO spends MUST include `BlockUtreexoData`
- `utreexo_root` field in header MUST match computed AFTER-state

**Conditional Logic**: PROHIBITED - There is no "legacy mode", "lite mode", or "optional Utreexo". All nodes MUST validate Utreexo proofs for blocks at height ≥ 2.

**Frozen**: YES - Utreexo is consensus-critical; changing activation is a hard fork

---

### 4.4 Witness Commitment (SegWit)

**Authority**: `consensus/witness_commitment.h`, `consensus/witness_commitment.cpp`

**Status**: **MANDATORY** - Witness commitment is enforced at height ≥ 2 for blocks containing witness transactions.

**Activation Heights**:
| Network | Height | Rationale |
|---------|--------|-----------|
| Regtest | 0 | Immediate for testing |
| Testnet | 2 | Match mainnet |
| Mainnet | 2 | First normal block |

**Commitment Format**:
- Magic: `0x444E5257` ("DINW" - Dinero Witness)
- Script: `OP_RETURN <37 bytes: magic + version + commitment_hash>`
- Formula: `commitment = SHA256(SHA256(witness_merkle_root || nonce))`

**DESIGN DECISION: Deterministic Witness Nonce**

DineroCoin uses a **deterministic** witness commitment nonce (32 zero bytes) instead of a coinbase witness stack like Bitcoin BIP-141.

| Aspect | Bitcoin BIP-141 | DineroCoin |
|--------|-----------------|------------|
| Nonce location | `coinbase.vin[0].witness[0]` | Hardcoded constant |
| Nonce value | Miner-chosen (typically zeros) | Always `0x00...00` (32 bytes) |
| Coinbase witness | Required | None (`witness_version = 0xFF`) |

**Why this divergence is INTENTIONAL**:
1. **DETERMINISM**: Same block always produces same commitment
2. **UTREEXO**: Stateless validation doesn't need coinbase witness lookup
3. **ZK PROOFS**: Deterministic inputs simplify circuit design
4. **SIMPLICITY**: No extra serialization complexity for coinbase

**Security properties PRESERVED**:
- Witness data is still committed via merkle root
- Commitment binds witness merkle to coinbase
- Tampering detection is identical to Bitcoin

**DO NOT** change this to match Bitcoin. This is a deliberate design choice.

**Frozen**: YES - Witness commitment format is consensus-critical

---

## 5. Difficulty Adjustment

### 5.1 Algorithm

**Type**: ASERT (Absolutely Scheduled Exponentially Rising Targets)

**Reference Implementation**: `src/consensus/asert.cpp`

**Parameters**:
- Target block time: 600 seconds (10 minutes)
- Half-life: 2 days (288 blocks)

**Frozen**: YES - Changing difficulty algorithm is a hard fork

---

## 6. Block Rewards

### 6.1 Subsidy Schedule

**Initial Reward**: 50 DIN per block

**Halving Interval**: 210,000 blocks (~4 years)

**Formula**: `subsidy = 50 * 100000000 >> (height / 210000)`

**Total Supply**: ~21 million DIN (asymptotic)

**Frozen**: YES - Changing subsidy is a hard fork

---

## 7. Genesis Block

### 7.1 Genesis Parameters

**Genesis Hash**: (From `dinero::Params().genesis_hash`)

**Genesis Timestamp**: November 25, 2025

**Genesis Motto**: "Dinero: Real Money For Free People"

**Premine**: Block 1 (not genesis)

**Frozen**: YES - Genesis cannot be changed without new chain

---

## 8. Network Parameters

### 8.1 Network Identity

**Network Magic**: (From `dinero::Params()`)

**Default Port**: (From chainparams)

**Protocol Version**: (From version.h)

**Service Bits**: NODE_NETWORK (1)

**Frozen**: YES - Changing these breaks P2P compatibility

---

## 9. Validation Flags

### 9.1 Block Validation Flags

**Used Flags**:
- `BLOCK_VALID_HEADER` - Header validation passed
- `BLOCK_HAVE_DATA` - Block data stored
- `BLOCK_CONNECTED` - Block applied to UTXO set
- `BLOCK_FAILED` - Validation failed permanently

**NOT Used** (from Bitcoin):
- Soft fork flags (WITNESS, TAPROOT, etc.) - DineroCoin uses explicit validators

**Frozen**: YES - Flag semantics must be stable

---

## 10. Mempool Policy (NOT Consensus)

### 10.1 Policy Rules (Phase F.12 Documentation)

**Purpose**: These rules control transaction acceptance into the mempool. They are NOT consensus rules and can be changed without a hard fork.

**Fee Estimation**: Dynamic (not consensus-critical)
- Fee rate calculated from recent blocks
- Used for prioritization, not validation
- Can be adjusted based on network conditions

**Replace-by-Fee (RBF)**: NOT SUPPORTED
- No transaction replacement allowed
- First-seen policy enforced
- Prevents mempool DoS via replacement storms
- **Status**: Phase G decision (may be reconsidered)

**Minimum Relay Fee**: Policy-only
- Default: 1000 una/kB
- Prevents spam transactions
- Nodes may have different minimums (not consensus)

**Child-Pays-For-Parent (CPFP)**: NOT SUPPORTED
- No ancestor fee rate calculation
- Each transaction evaluated independently
- **Status**: Phase G decision (may be added)

**Transaction Selection (Mining)**:
- Highest fee rate first (greedy algorithm)
- Coinbase maturity enforced (100 blocks)
- No ancestor/descendant limits (yet)

**Script Validation**:
- **Consensus Path**: Uses `ValidateSpend()` (explicit validators)
- **Mempool Path**: Uses `VerifyScript()` (policy layer)
- Difference: Mempool may be more restrictive but never less

**Determinism**:
- ✅ Fee rate calculation: Deterministic
- ✅ Maturity checks: Deterministic
- ⚠️ Mempool eviction: May vary by node (not consensus-critical)
- ⚠️ Transaction ordering in mempool: Not guaranteed

**These are NOT frozen** - Can be changed without hard fork

---

## 11. Auditing & Safety

### 11.1 Pre-P2P Verification

**No TODOs in consensus paths**: ✅ Verified
**No test-only code in production**: ✅ Verified
**No #ifdef DINERO_TESTING in consensus**: ✅ Verified

**Test Coverage**:
- Phase F tests: 36/36 passing ✅
- Consensus validation: Comprehensive ✅
- Reorg safety: Validated ✅

---

## 12. Change Policy

### 12.1 What Requires Hard Fork

- Adding new script types
- Changing chainwork calculation
- Changing maturity depth
- Changing block structure
- Changing difficulty algorithm
- Changing subsidy schedule
- Changing genesis parameters

### 12.2 What Can Change (Soft Fork or Policy)

- Mempool acceptance rules
- Fee estimation
- RBF policy
- Block relay strategy
- P2P protocol optimizations (compact blocks, etc.)

---

## 13. Frozen Signature

**Date Frozen**: December 29, 2025
**Git Commit**: `40e50431` (consensus: fix service-layer reorgs)
**Git Tag**: `phase-f-certified`
**Phase**: F.12 (Pre-P2P Lockdown)

**Attestation**: These rules are frozen for mainnet compatibility. Changes require explicit network coordination and versioning.

---

## References

- `src/consensus/script_validation.cpp` - Script validation
- `src/daemon/services/chainstate_service.cpp` - Chain selection & reorg
- `src/consensus/block_validation.cpp` - Block validation
- `src/consensus/asert.cpp` - Difficulty adjustment
- `include/consensus/chainparams.h` - Network parameters
- `docs/phases/PHASE_F11_SCOPE.md` - Reorg safety requirements
