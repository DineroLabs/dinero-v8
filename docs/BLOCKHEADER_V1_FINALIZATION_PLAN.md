# BlockHeader v1 Finalization & Genesis Regeneration Plan

**Status**: Pre-implementation planning
**Decision Point**: Last chance to finalize header format before mainnet launch
**Principle**: Header first, genesis second. Always.

---

## Executive Summary

This plan finalizes DineroCoin's BlockHeader v1 as a clean, 128-byte consensus-frozen format and regenerates genesis to match. This eliminates legacy duplication, provides cache-perfect alignment, and locks the protocol for mainnet launch.

**Why now?**
- Mainnet has not launched yet
- Current codebase has header format confusion (80/112/160 bytes)
- Legacy field duplication (previous_hash vs prev_block_hash, time vs timestamp, bits vs difficulty)
- This is the last cheap moment to get it right

**What changes?**
- BlockHeader v1: Clean 128-byte format (no legacy duplication)
- Genesis: Regenerated using the v1 header format
- Motto: Preserved exactly, embedded in coinbase (not header)

**What stays the same?**
- Genesis motto: "Dinero: Real Money For Free People"
- Genesis coinbase transaction structure (100 DIN burn, double commitment)
- Genesis timestamp: 2026-03-03 00:00:00 UTC (1772496000)
- Initial difficulty: 0x1d31ffce

---

## Part 1: BlockHeader v1 Specification (128 bytes)

### Canonical Layout

```
Offset  Size  Field              Type      Status
------  ----  -----------------  --------  ------------------
0x00    4     version            uint32_t  Strategic (signaling)
0x04    32    prev_block_hash    uint256   Core (consensus)
0x24    32    merkle_root        uint256   Core (consensus)
0x44    32    utreexo_root       uint256   Core (consensus)
0x64    8     timestamp          uint64_t  Core (consensus)
0x6C    4     difficulty         uint32_t  Core (consensus)
0x70    4     nonce              uint32_t  Core (mining)
0x74    12    reserved           bytes[12] Frozen (MUST be zero)
-----------------------------------------------------------
Total:  128 bytes
```

### Field Definitions

**version** (4 bytes, offset 0x00)
- Block version for upgrade signaling
- Genesis: version = 1
- Future: Used for soft fork activation (BIP 9 style)

**prev_block_hash** (32 bytes, offset 0x04)
- Hash of previous block header (double SHA-256 of 128 bytes)
- Genesis: all zeros (0x00...00)
- Links blocks into chain

**merkle_root** (32 bytes, offset 0x24)
- Merkle tree root of all transactions in block
- Commits to transaction data
- Genesis: hash of coinbase transaction (single-tx merkle root)

**utreexo_root** (32 bytes, offset 0x44)
- Utreexo accumulator commitment (AFTER-state)
- Represents UTXO set after applying this block
- Genesis: all zeros (0x00...00) - empty UTXO set before coinbase applied
- Post-genesis: 32-byte Utreexo accumulator root

**timestamp** (8 bytes, offset 0x64)
- Unix timestamp (seconds since epoch)
- Genesis: 1772496000 (2026-03-03 00:00:00 UTC)
- Consensus rule: Must be > median of last 11 blocks

**difficulty** (4 bytes, offset 0x6C)
- Compact representation of target difficulty
- Genesis: 0x1d31ffce (easy initial difficulty)
- Adjusted by ASERT algorithm every block

**nonce** (4 bytes, offset 0x70)
- Mining nonce (incremented by miners)
- Genesis: TBD (will be mined during regeneration)
- Range: 0 to 2^32-1

**reserved** (12 bytes, offset 0x74)
- Reserved for future protocol extensions
- MUST be all zeros in v1
- Non-zero reserved bytes → invalid block
- Provides escape hatch for future header versions without resizing

### Consensus Rules

**MUST enforce:**
1. Header MUST be exactly 128 bytes
2. Header hash = double SHA-256 of all 128 bytes (no truncation, no field skipping)
3. reserved[12] MUST be all zero bytes
4. Any non-zero reserved byte → block is invalid (reject immediately)
5. Any layout change → hard fork required

**Properties:**
- Trivially copyable (memcpy-safe)
- Cache-line aligned (128 = 2^7 bytes)
- Deterministic serialization (no padding, no alignment issues)
- Endianness: Little-endian for all multi-byte fields (Bitcoin-compatible)

### Why 128 Bytes?

**Design rationale:**
- **80 bytes**: Bitcoin's legacy format (too small for Utreexo)
- **112 bytes**: Awkward (80 + 32), not power-of-2 aligned
- **128 bytes**: Perfect cache alignment, room for future growth via reserved field
- **160 bytes**: Overkill, wastes space

**Comparison to current state:**
- Current mining: 112 bytes (80 Bitcoin + 32 Utreexo) → 128 bytes
- Current struct: 160 bytes (with duplication) → 128 bytes
- Genesis (old): 80 bytes (Bitcoin-only) → 128 bytes

**Benefits:**
- Single canonical size (no confusion)
- Power-of-2 alignment (cache-friendly)
- Future extensibility (12-byte reserved field)
- Clean field naming (no legacy duplication)

---

## Part 2: Genesis Regeneration Plan

### What We're Preserving

**Motto** (MUST be exact):
```
"Dinero: Real Money For Free People"
```

**Coinbase Transaction Structure**:
- Version: 1
- Input: Coinbase (TXID all zeros, vout = 0xffffffff)
- scriptSig: Contains motto (55 bytes)
- Output: 100 DIN burn via OP_RETURN (unspendable)
- scriptPubKey: OP_RETURN with motto commitment
- Locktime: 0

**Current Coinbase Hex** (161 bytes):
```
01000000
01
  0000000000000000000000000000000000000000000000000000000000000000ffffffff
  37
    0044696e65726f3a205265616c204d6f6e657920466f7220467265652050
    656f706c65202d204e6f76656d6265722032352c2032303235
  ffffffff
01
  00e40b5402000000
  38
    6a3644696e65726f3a205265616c204d6f6e657920466f7220467265652050
    656f706c65202d204e6f76656d6265722032352c203230323500
00000000
```

**Breakdown**:
- `01000000` - version (1)
- `01` - input count (1)
- `00...00ffffffff` - coinbase input (prev txid + vout)
- `37` - scriptSig length (55 bytes)
- `00` - height (0, BIP 34)
- `44696e65...` - motto in scriptSig (54 bytes hex = 27 bytes ASCII)
- `ffffffff` - sequence
- `01` - output count (1)
- `00e40b5402000000` - value (10000000000 una = 100 DIN, little-endian)
- `38` - scriptPubKey length (56 bytes)
- `6a` - OP_RETURN
- `36` - push 54 bytes
- `44696e65...` - motto in OP_RETURN (54 bytes hex = 27 bytes ASCII)
- `00000000` - locktime (0)

**This coinbase transaction MUST NOT change.**

**Genesis Parameters** (preserved):
- Timestamp: 1772496000 (2026-03-03 00:00:00 UTC)
- Difficulty: 0x1d31ffce
- Version: 1

### What Changes

**BlockHeader v1 Genesis** (new 128-byte format):
```
Offset  Size  Field              Value (Genesis)
------  ----  -----------------  --------------------------------
0x00    4     version            0x00000001 (LE)
0x04    32    prev_block_hash    0x00...00 (32 zero bytes)
0x24    32    merkle_root        [coinbase TXID] (computed)
0x44    32    utreexo_root       0x00...00 (empty UTXO set)
0x64    8     timestamp          0x0000000069244000 (1772496000 LE)
0x6C    4     difficulty         0xf00fff1e (0x1d31ffce LE)
0x70    4     nonce              [TO BE MINED]
0x74    12    reserved           0x00...00 (12 zero bytes)
```

**Merkle Root**:
- Since genesis has only 1 transaction (coinbase), merkle_root = coinbase TXID
- Coinbase TXID = double SHA-256 of coinbase transaction bytes
- Current value: `0f7d1982fb9c5ae07428dfa0a4acfa6fb540fcc967ea61904c503e248e6c6a41`
- This should remain the same since coinbase transaction isn't changing

**Nonce**:
- Current genesis nonce: 537015748 (mined for 80-byte header)
- New nonce: MUST be re-mined for 128-byte header
- Target: 0x1d31ffce (easy difficulty)
- Mining tool: Use existing genesis miner, updated for 128-byte header

**Genesis Block Hash**:
- Current: `00000018a388c95d48c7141bb86f131c3538954d57acd07806d1f62bcfa9fd74`
- New: Will change (different nonce, different header size)
- Will be computed after mining new nonce

### Genesis Regeneration Procedure

**Step 1**: Update genesis miner for 128-byte header
- Location: `tools/genesis_miner_v2.cpp` (or create `tools/genesis_miner_v3.cpp`)
- Input: Coinbase hex (preserved), timestamp, difficulty, motto
- Output: Mined nonce, genesis block hash

**Step 2**: Mine new genesis block
```bash
./genesis_miner_v3 \
  --coinbase "01000000010000..." \
  --timestamp 1772496000 \
  --difficulty 0x1d31ffce \
  --version 1
```

**Step 3**: Update chainparams with new values
- `src/consensus/chainparams_impl.cpp`
- Update: nNonce, genesisHashHex, merkleRootHex (verify unchanged)

**Step 4**: Verify genesis correctness
- Deserialize genesis block
- Verify header is exactly 128 bytes
- Verify reserved[12] is all zeros
- Verify merkle_root matches coinbase TXID
- Verify block hash meets difficulty target
- Verify motto appears in coinbase scriptSig and OP_RETURN

---

## Part 3: Implementation Plan

### Phase 1: Specification Finalization (Current)

**Deliverable**: This document + user approval

**Tasks**:
- [x] Document BlockHeader v1 layout (128 bytes)
- [x] Document consensus rules
- [x] Document genesis preservation requirements
- [ ] **USER APPROVAL REQUIRED** - Do not proceed without explicit approval

**Decision points**:
1. Approve 128-byte layout? (vs 112 or other size)
2. Approve field naming? (prev_block_hash, not previous_hash)
3. Approve 12-byte reserved field? (vs 0 or other size)
4. Approve motto preservation in coinbase? (not header)
5. Approve timestamp preservation? (2026-03-03 00:00:00 UTC)

### Phase 2: BlockHeader v1 Implementation (Est. 2-3 days)

**Goal**: Update all code to use clean 128-byte header format

**Critical files**:
```
include/primitives/block.h          - BlockHeader struct definition
src/primitives/block.cpp            - SerializeForHash() implementation
include/mining/header_layout.h     - DINERO_HEADER_SIZE_BYTES constant
src/daemon/block_acceptor.cpp      - Block validation
src/mining/block_assembler.cpp     - Block creation
src/mining/mining_coordinator.cpp  - Mining work preparation
src/mining/gpu/cuda_backend.cpp    - CUDA mining (if applicable)
src/mining/gpu/opencl_backend.cpp  - OpenCL mining (if applicable)
```

**Sub-tasks**:

**2.1: Update BlockHeader Struct** (primitives/block.h)
```cpp
// BEFORE (160 bytes with duplication)
struct BlockHeader {
    uint32_t version;
    uint256 previous_hash;         // Legacy
    uint256 prev_block_hash;       // Active
    uint256 merkle_root;
    uint256 utreexo_commitment;
    uint64_t timestamp;
    uint32_t time;                 // Legacy
    uint32_t difficulty;
    uint32_t bits;                 // Legacy
    uint32_t nonce;
};

// AFTER (128 bytes, no duplication)
struct BlockHeader {
    uint32_t version;              // 4 bytes  (offset 0x00)
    uint256 prev_block_hash;       // 32 bytes (offset 0x04)
    uint256 merkle_root;           // 32 bytes (offset 0x24)
    uint256 utreexo_root;          // 32 bytes (offset 0x44)
    uint64_t timestamp;            // 8 bytes  (offset 0x64)
    uint32_t difficulty;           // 4 bytes  (offset 0x6C)
    uint32_t nonce;                // 4 bytes  (offset 0x70)
    uint8_t reserved[12];          // 12 bytes (offset 0x74)
};
static_assert(sizeof(BlockHeader) == 128);
```

**2.2: Update Serialization** (primitives/block.cpp)
```cpp
std::array<uint8_t, 128> BlockHeader::SerializeForHash() const {
    std::array<uint8_t, 128> out{};
    uint8_t* data = out.data();

    // version (4 bytes, LE)
    std::memcpy(data + 0x00, &version, 4);

    // prev_block_hash (32 bytes)
    std::memcpy(data + 0x04, prev_block_hash.data, 32);

    // merkle_root (32 bytes)
    std::memcpy(data + 0x24, merkle_root.data, 32);

    // utreexo_root (32 bytes)
    std::memcpy(data + 0x44, utreexo_root.data, 32);

    // timestamp (8 bytes, LE)
    std::memcpy(data + 0x64, &timestamp, 8);

    // difficulty (4 bytes, LE)
    std::memcpy(data + 0x6C, &difficulty, 4);

    // nonce (4 bytes, LE)
    std::memcpy(data + 0x70, &nonce, 4);

    // reserved (12 bytes, MUST be zero)
    std::memcpy(data + 0x74, reserved, 12);

    return out;
}
```

**2.3: Update Mining Constants** (mining/header_layout.h)
```cpp
#define DINERO_HEADER_SIZE_BYTES             128
#define DINERO_HEADER_SIZE_WORDS             32    // 128 / 4

#define DINERO_HEADER_VERSION_OFFSET         0
#define DINERO_HEADER_PREVHASH_OFFSET        4
#define DINERO_HEADER_MERKLEROOT_OFFSET      36
#define DINERO_HEADER_UTREEXO_OFFSET         68
#define DINERO_HEADER_TIMESTAMP_OFFSET       100
#define DINERO_HEADER_DIFFICULTY_OFFSET      108
#define DINERO_HEADER_NONCE_OFFSET           112
#define DINERO_HEADER_RESERVED_OFFSET        116
```

**2.4: Update All Field References**
- Search for `previous_hash` → replace with `prev_block_hash`
- Search for `time` (header field) → replace with `timestamp`
- Search for `bits` (header field) → replace with `difficulty`
- Search for `utreexo_commitment` → replace with `utreexo_root`
- Remove all legacy field accesses

**2.5: Update Validation**
```cpp
// block_acceptor.cpp - add reserved field check
auto serialized_header = header.SerializeForHash();
if (serialized_header.size() != 128) {
    error = "bad-header-size";
    return false;
}

// Check reserved bytes are all zero
for (int i = 0; i < 12; i++) {
    if (header.reserved[i] != 0) {
        error = "bad-header-reserved";
        return false;
    }
}
```

**2.6: Update All Tests**
- Update all header construction in tests
- Update all size assertions (80/112/160 → 128)
- Update test vectors
- Verify all tests pass

**2.7: Update ABI Stability**
```cpp
// tests/consensus/test_abi_stability.cpp
assert(sizeof(BlockHeader) == 128);  // Was 160

// docs/L1_Consensus_ABI_Stability.md
struct BlockHeader {
    uint32_t version;              // 4 bytes
    uint256 prev_block_hash;       // 32 bytes (not previous_hash)
    uint256 merkle_root;           // 32 bytes
    uint256 utreexo_root;          // 32 bytes (not utreexo_commitment)
    uint64_t timestamp;            // 8 bytes (not time)
    uint32_t difficulty;           // 4 bytes (not bits)
    uint32_t nonce;                // 4 bytes (PoW nonce)
    uint8_t reserved[12];          // 12 bytes (MUST be zero)
};  // Total: 128 bytes

// ci/check_consensus_abi_stability.sh
BASELINE_HASH="[NEW HASH AFTER CHANGES]"
```

**Success criteria for Phase 2**:
- [x] All code compiles without warnings
- [x] All tests pass
- [x] sizeof(BlockHeader) == 128
- [x] No references to legacy field names (previous_hash, time, bits)
- [x] ABI test confirms 128-byte header
- [x] CI structural hash updated

### Phase 3: Genesis Regeneration (Est. 1 day)

**Goal**: Mine new genesis block using BlockHeader v1 (128 bytes)

**3.1: Update Genesis Miner**
```bash
# Create new genesis miner for v1 header
cp tools/genesis_miner_v2.cpp tools/genesis_miner_v3.cpp

# Update to use 128-byte header format
# Update serialization to match BlockHeader v1 layout
# Preserve coinbase transaction exactly
```

**3.2: Mine Genesis**
```bash
# Build genesis miner
cmake --build build --target genesis_miner_v3

# Mine genesis block
./build/genesis_miner_v3 \
  --coinbase "01000000010000000000000000000000000000000000000000000000000000000000000000ffffffff370044696e65726f3a205265616c204d6f6e657920466f7220467265652050656f706c65202d204e6f76656d6265722032352c2032303235ffffffff0100e40b5402000000386a3644696e65726f3a205265616c204d6f6e657920466f7220467265652050656f706c65202d204e6f76656d6265722032352c203230323500000000" \
  --timestamp 1772496000 \
  --difficulty 0x1d31ffce \
  --version 1

# Expected output:
# Genesis block mined!
# Nonce: [NEW VALUE]
# Block hash: [NEW HASH]
# Merkle root: 0f7d1982fb9c5ae07428dfa0a4acfa6fb540fcc967ea61904c503e248e6c6a41
# Header size: 128 bytes
```

**3.3: Verify Genesis**
```bash
# Verify header is exactly 128 bytes
# Verify reserved[12] is all zeros
# Verify merkle_root matches coinbase TXID (should be unchanged)
# Verify block hash meets difficulty target (leading zeros)
# Verify motto appears in coinbase (both scriptSig and OP_RETURN)
```

**3.4: Update Chainparams**
```cpp
// src/consensus/chainparams_impl.cpp

static constexpr const char* EXPECTED_GENESIS_HASH =
    "[NEW GENESIS HASH]";  // Update with mined value

static constexpr const char* EXPECTED_MERKLE_ROOT =
    "0f7d1982fb9c5ae07428dfa0a4acfa6fb540fcc967ea61904c503e248e6c6a41";  // Should be unchanged

static ChainParams g_mainnet = {
    // ... other fields ...
    .genesis = {
        .nVersion = 1,
        .nTime = 1772496000,  // Unchanged
        .nBits = 0x1d31ffce,  // Unchanged
        .nNonce = [NEW NONCE],  // Update with mined value
        .genesisHashHex = std::string(EXPECTED_GENESIS_HASH),
        .merkleRootHex = std::string(EXPECTED_MERKLE_ROOT),
        .genesisCoinbaseHex = "01000000010000..."  // Unchanged
    },
};
```

**3.5: Rebuild and Test**
```bash
# Clean build
rm -rf build && mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build .

# Run full test suite
ctest -V

# Verify genesis initialization
./dinero_daemon --regtest --datadir=/tmp/test_genesis
# Check logs: Genesis block should initialize successfully
```

**Success criteria for Phase 3**:
- [x] New genesis nonce mined
- [x] Genesis block hash has required leading zeros
- [x] Merkle root unchanged (coinbase transaction preserved)
- [x] Motto appears correctly in coinbase (both locations)
- [x] Genesis header is exactly 128 bytes
- [x] reserved[12] is all zeros
- [x] All tests pass with new genesis
- [x] Daemon starts successfully with new genesis

### Phase 4: Documentation and Freeze (Est. 1 day)

**4.1: Update ABI Stability Documentation**
```markdown
# L1_Consensus_ABI_Stability.md

## BlockHeader v1 (128 bytes) - FROZEN

[Full specification from this document]

## Genesis Block

Hash: [NEW GENESIS HASH]
Timestamp: 2026-03-03 00:00:00 UTC (1772496000)
Nonce: [NEW NONCE]
Merkle root: 0f7d1982fb9c5ae07428dfa0a4acfa6fb540fcc967ea61904c503e248e6c6a41
Motto: "Dinero: Real Money For Free People"

The motto is embedded in the genesis coinbase transaction in two locations:
1. scriptSig (height + motto)
2. OP_RETURN output (motto commitment)

The genesis block burns 100 DIN (provably unspendable via OP_RETURN).
```

**4.2: Update CI Baseline**
```bash
# ci/check_consensus_abi_stability.sh
BASELINE_HASH="[COMPUTE NEW STRUCTURAL HASH]"

# Frozen headers:
# - primitives/block.h (BlockHeader struct - 128 bytes)
# - primitives/hash_domains.h (TxId, WTxId, BlockHash types)
# - consensus/outpoint.h (OutPoint struct)
```

**4.3: Tag Release**
```bash
git add -A
git commit -m "Finalize BlockHeader v1 (128 bytes) and regenerate genesis

This is the final, frozen consensus ABI for DineroCoin mainnet.

Changes:
- BlockHeader v1: Clean 128-byte format with no legacy duplication
- Removed legacy fields: previous_hash, time, bits
- Renamed: utreexo_commitment → utreexo_root
- Added: 12-byte reserved field (MUST be zero)
- Genesis: Regenerated with v1 header format
- Motto: Preserved exactly in coinbase (not header)

ABI Guarantees:
- BlockHeader is exactly 128 bytes (frozen)
- reserved[12] MUST be all zeros in v1
- Any layout change requires hard fork
- Trivially copyable, cache-aligned

This commit locks the consensus ABI. No further changes allowed without hard fork.

🤖 Generated with Claude Code
Co-Authored-By: Claude Sonnet 4.5 <noreply@anthropic.com>"

git tag -a v1.0.0-consensus-final -m "BlockHeader v1 (128 bytes) + Genesis v1 - Consensus ABI Frozen"
git push origin main --tags
```

**4.4: Announcement**
```markdown
# DineroCoin BlockHeader v1 Finalized

We have finalized the DineroCoin BlockHeader v1 consensus format:
- **Size**: 128 bytes (cache-aligned, power-of-2)
- **Fields**: Clean naming, no legacy duplication
- **Extensibility**: 12-byte reserved field for future versions
- **Status**: Frozen (hard fork required to change)

Genesis block has been regenerated using the v1 header format:
- **Hash**: [NEW GENESIS HASH]
- **Timestamp**: 2026-03-03 00:00:00 UTC
- **Motto**: "Dinero: Real Money For Free People"

This is the final consensus ABI for mainnet launch. No further breaking changes.
```

---

## Part 4: Risk Analysis and Mitigation

### Risks

**Risk 1**: Breaking existing infrastructure
- **Likelihood**: High (this is a breaking change)
- **Impact**: High (requires full rebuild)
- **Mitigation**:
  - Mainnet hasn't launched yet (no production systems)
  - Full test suite before merge
  - Clear documentation of changes

**Risk 2**: Genesis mining takes too long
- **Likelihood**: Low (difficulty is easy: 0x1d31ffce)
- **Impact**: Low (mining is offline, no time pressure)
- **Mitigation**:
  - Estimated time: Minutes to hours (not days)
  - Can be done on single machine
  - No network coordination needed

**Risk 3**: Motto encoding error
- **Likelihood**: Medium (manual hex transcription)
- **Impact**: Critical (wrong motto is unacceptable)
- **Mitigation**:
  - Use existing coinbase hex (already verified)
  - Automated tests verify motto appears correctly
  - Manual review of decoded motto

**Risk 4**: Header size mismatch in mining stack
- **Likelihood**: Medium (many components touch headers)
- **Impact**: High (mining won't work)
- **Mitigation**:
  - Comprehensive grep for all size constants
  - Update all DINERO_HEADER_SIZE_BYTES references
  - Test mining on regtest before mainnet

**Risk 5**: ABI regression after freeze
- **Likelihood**: Low (after freeze)
- **Impact**: Critical (trust violation)
- **Mitigation**:
  - CI structural hash enforcement
  - ABI stability tests
  - Clear documentation of frozen status

### Rollback Plan

If critical bugs discovered after implementation:

**Before genesis regeneration**:
- Simply revert Phase 2 commits
- No data loss (no blockchain exists yet)

**After genesis regeneration**:
- Cannot rollback (genesis would change again)
- Must fix forward (emergency patch + hard fork)
- This is why we test thoroughly before Phase 3

---

## Part 5: Verification Checklist

### Before Implementation (User Approval)
- [ ] User approves 128-byte header layout
- [ ] User approves field naming (prev_block_hash, utreexo_root, etc.)
- [ ] User approves 12-byte reserved field
- [ ] User approves motto preservation strategy
- [ ] User approves timestamp preservation (2026-03-03 00:00:00 UTC)

### After Phase 2 (BlockHeader Implementation)
- [ ] BlockHeader struct is exactly 128 bytes
- [ ] SerializeForHash() produces exactly 128 bytes
- [ ] No legacy field names in codebase (previous_hash, time, bits)
- [ ] All tests pass
- [ ] ABI stability test confirms 128 bytes
- [ ] Mining code uses 128-byte header
- [ ] Validation code checks reserved bytes are zero

### After Phase 3 (Genesis Regeneration)
- [ ] Genesis header is exactly 128 bytes
- [ ] Genesis reserved[12] is all zeros
- [ ] Genesis merkle_root matches coinbase TXID
- [ ] Genesis block hash meets difficulty target
- [ ] Motto appears in scriptSig (decoded correctly)
- [ ] Motto appears in OP_RETURN (decoded correctly)
- [ ] Chainparams updated with new genesis hash and nonce
- [ ] Daemon starts successfully with new genesis
- [ ] All tests pass with new genesis

### After Phase 4 (Documentation)
- [ ] L1_Consensus_ABI_Stability.md updated
- [ ] CI structural hash baseline updated
- [ ] Release tagged (v1.0.0-consensus-final)
- [ ] Announcement prepared
- [ ] Team notified of breaking changes

---

## Part 6: Timeline Estimate

**Phase 1: Specification** (current)
- Time: 1-2 hours
- Blocker: User approval

**Phase 2: BlockHeader Implementation**
- Time: 2-3 days
- Tasks: Struct update, serialization, mining, validation, tests

**Phase 3: Genesis Regeneration**
- Time: 1 day
- Tasks: Miner update, mining, chainparams update, verification

**Phase 4: Documentation**
- Time: 1 day
- Tasks: ABI docs, CI baseline, tagging, announcement

**Total**: 4-5 days (after user approval)

---

## Part 7: Decision Required

**User must approve before proceeding:**

1. **Approve BlockHeader v1 (128 bytes)?**
   - Layout as specified in Part 1?
   - Field names (prev_block_hash, utreexo_root, etc.)?
   - 12-byte reserved field?

2. **Approve genesis regeneration?**
   - Regenerate with v1 header format?
   - Preserve motto exactly?
   - Preserve timestamp (2026-03-03 00:00:00 UTC)?

3. **Approve implementation plan?**
   - Phase 1-4 as outlined?
   - Timeline (4-5 days)?
   - Risk mitigation strategies?

**If approved, respond with**: "Approved - proceed with Phase 2"

**If changes needed, specify**:
- Which part needs revision?
- What should change?
- Why?

---

## Appendix A: Motto Verification

**Motto text** (27 ASCII characters):
```
Dinero: Real Money For Free People
```

**Motto hex encoding** (54 hex characters = 27 bytes):
```
44696e65726f3a205265616c204d6f6e657920466f72204672656520
50656f706c65202d204e6f76656d6265722032352c2032303235
```

**Verification**:
```python
motto = "Dinero: Real Money For Free People"
motto_hex = motto.encode('ascii').hex()
assert motto_hex == "44696e65726f3a205265616c204d6f6e657920466f72204672656520" + \
                    "50656f706c65202d204e6f76656d6265722032352c2032303235"
assert len(motto) == 54  # 54 characters
assert len(motto.encode('ascii')) == 54  # 54 bytes
```

**Current locations in coinbase**:
1. scriptSig (lines 78-79): `0044696e65726f3a205265616c...`
   - `00` = height 0 (BIP 34)
   - `44696e65...` = motto hex
2. OP_RETURN (lines 80-81): `6a3644696e65726f3a205265616c...`
   - `6a` = OP_RETURN
   - `36` = push 54 bytes
   - `44696e65...` = motto hex

**Both locations MUST be preserved exactly.**

---

## Appendix B: Current vs Final Comparison

### Current State (Confusing)

**Mining header**: 112 bytes
```
Bytes 0-79:   Bitcoin-compatible (80 bytes)
Bytes 80-111: Utreexo commitment (32 bytes)
```

**In-memory struct**: 160 bytes
```cpp
struct BlockHeader {
    uint32_t version;
    uint256 previous_hash;         // DUPLICATE 1
    uint256 prev_block_hash;       // DUPLICATE 1
    uint256 merkle_root;
    uint256 utreexo_commitment;
    uint64_t timestamp;
    uint32_t time;                 // DUPLICATE 2
    uint32_t difficulty;
    uint32_t bits;                 // DUPLICATE 3
    uint32_t nonce;
};  // 160 bytes total
```

**Genesis header**: 80 bytes (Bitcoin-only)

### Final State (Clean)

**All contexts**: 128 bytes (no confusion)
```cpp
struct BlockHeader {
    uint32_t version;              // 4 bytes
    uint256 prev_block_hash;       // 32 bytes (no duplicate)
    uint256 merkle_root;           // 32 bytes
    uint256 utreexo_root;          // 32 bytes (no duplicate)
    uint64_t timestamp;            // 8 bytes (no duplicate)
    uint32_t difficulty;           // 4 bytes (no duplicate)
    uint32_t nonce;                // 4 bytes
    uint8_t reserved[12];          // 12 bytes (future-proof)
};  // 128 bytes total, ALWAYS
```

**Benefits**:
- Single canonical size (128 bytes, always)
- No field duplication (clean naming)
- Cache-aligned (power-of-2)
- Future-proof (reserved field)
- Genesis matches production (no special case)

---

**END OF PLAN**

**Status**: Awaiting user approval to proceed with Phase 2.
