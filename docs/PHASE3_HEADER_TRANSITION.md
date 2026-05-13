# Phase 3: BlockHeader v1 Transition - MANDATORY

**Status:** LOCKED - Effective at Block 1
**Header Size:** 128 bytes (IMMUTABLE)
**Difficulty Field:** `difficulty` (not `bits`)
**Timestamp Field:** `timestamp` (64-bit, not 32-bit `time`)

---

## The Clean Rule (Invariant)

```
Genesis:     Constructed (hard-coded, no PoW, special handling)
Block 1+:    Mined (128-byte BlockHeader v1, full PoW validation)
```

**There is no mixed mode once you launch.**

---

## What MUST Use 128-Byte BlockHeader v1

### Starting at Height 1 (First Mined Block):

#### ✅ External Miner

**Requirements:**
- MUST assemble a 128-byte header
- MUST hash exactly those 128 bytes
- MUST increment nonce inside that 128-byte structure
- MUST zero `reserved[12]` (for now)
- MUST use `difficulty` field (not `bits`)
- MUST use 64-bit `timestamp` (not 32-bit `time`)

**Implementation:**
```cpp
struct BlockHeader {
    uint32_t version;           // 4 bytes, offset 0x00
    uint256 prev_block_hash;    // 32 bytes, offset 0x04
    uint256 merkle_root;        // 32 bytes, offset 0x24
    uint256 utreexo_root;       // 32 bytes, offset 0x44
    uint64_t timestamp;         // 8 bytes, offset 0x64
    uint32_t difficulty;        // 4 bytes, offset 0x6C
    uint32_t nonce;             // 4 bytes, offset 0x70
    uint8_t reserved[12];       // 12 bytes, offset 0x74
};

static_assert(sizeof(BlockHeader) == 128);
```

---

#### ✅ Internal Mining Engine

**Requirements:**
- Same rules as external miner
- No legacy 80-byte or 112-byte paths
- No transitional structs
- One canonical header layout only

**File:** `src/mining/miner_engine.cpp`

**Must verify:**
```cpp
// Before mining loop
assert(sizeof(BlockHeader) == 128 &&
       "Mining engine must use 128-byte BlockHeader v1");

// Before hashing
std::array<uint8_t, 128> header_bytes = header.SerializeForHash();
assert(header_bytes.size() == 128 &&
       "Header must serialize to exactly 128 bytes");

// Hash all 128 bytes
uint256 hash = DoubleShapeTypeError256(header_bytes.data(), 128);
```

---

#### ✅ getblocktemplate / Mining RPC

**Requirements:**
- MUST return 128-byte header fields
- MUST return `difficulty` field (compact target)
- MUST return 64-bit `curtime` (not 32-bit)
- NO `bits` field (legacy)
- NO 32-bit `time` field (legacy)

**File:** `src/rpc/methods_mining_template.cpp`

**Template format:**
```json
{
  "version": 1,
  "previousblockhash": "...",
  "transactions": [...],
  "coinbasevalue": 10000000000,
  "target": "00000000ffff0000...",  // Full 256-bit target
  "difficulty": "0x1d00ffff",        // Compact bits (NEW name)
  "curtime": 1772496000,             // 64-bit timestamp
  "mintime": 1764028200,
  "height": 1,
  "reserved": "000000000000000000000000"  // 12 zero bytes
}
```

**❌ DO NOT return:**
- `bits` (use `difficulty` instead)
- `time` (use `curtime` instead)

---

#### ✅ Consensus Validation

**Requirements:**
- MUST hash 128 bytes
- MUST check PoW against `difficulty` field
- MUST apply ASERT from anchor
- MUST reject blocks that don't serialize to exactly 128 bytes

**File:** `src/consensus/block_validation.cpp`

**Validation logic:**
```cpp
// Deserialize header
BlockHeader header = ...; // from network or disk

// Phase 3 hard fork: Header MUST be 128 bytes
static_assert(sizeof(BlockHeader) == 128,
    "Consensus validation requires 128-byte headers");

// Serialize for hashing
std::array<uint8_t, 128> header_bytes = header.SerializeForHash();
if (header_bytes.size() != 128) {
    return false; // Reject: invalid header size
}

// Compute hash (double SHA-256)
uint256 hash = DoubleS HA256(header_bytes.data(), 128);

// Check PoW
uint256 target = CompactToTarget(header.difficulty);  // Use difficulty field
if (hash >= target) {
    return false; // Reject: insufficient PoW
}

// ASERT difficulty check (Block 1+)
if (height >= 1) {
    uint32_t required_difficulty = ComputeASERTTarget(
        prev_header,
        header,
        ASERTConsensus::ASERT_ANCHOR_BITS,
        ASERTConsensus::ASERT_ANCHOR_HEIGHT
    );
    if (header.difficulty != required_difficulty) {
        return false; // Reject: wrong difficulty
    }
}
```

---

## Difficulty: New Field, Immediately Active

### What Changed:

**Old (Legacy):**
```cpp
struct BlockHeader {
    uint32_t time;  // 32-bit
    uint32_t bits;  // Compact target
};
```

**New (BlockHeader v1):**
```cpp
struct BlockHeader {
    uint64_t timestamp;  // 64-bit (renamed from 'time')
    uint32_t difficulty; // Compact target (renamed from 'bits')
};
```

### When It Takes Effect:

| Block | Difficulty Source | PoW Validated? |
|-------|-------------------|----------------|
| Block 0 (Genesis) | Hard-coded (0x1d00ffff) | ❌ NO (genesis bypass) |
| Block 1 (Premine) | Hard-coded (0x1d00ffff) | ✅ YES (PoW enforced) |
| Block 2+ | ASERT computed | ✅ YES (PoW enforced) |

### ASERT Anchor:

```cpp
// From include/consensus/asert_params.h
static constexpr uint32_t ASERT_ANCHOR_HEIGHT = 1;
static constexpr uint32_t ASERT_ANCHOR_BITS = 0x1d00ffff;
static constexpr uint64_t ASERT_ANCHOR_TIME = <block_1_timestamp>;
```

**Block 1 becomes the ASERT anchor:**
- Difficulty = 0x1d00ffff (Bitcoin genesis standard)
- All future blocks adjust from this anchor

---

## What to Explicitly Delete/Forbid

### Before Launch, Remove These:

#### ❌ Legacy Header Size References

**File:** `include/consensus/header_consensus.h`

```cpp
// DELETE THIS:
static_assert(CURRENT_BLOCK_HEADER_SIZE == 112,
    "CONSENSUS VIOLATION: Block headers must be 112 bytes...");

// REPLACE WITH:
static_assert(CURRENT_BLOCK_HEADER_SIZE == 128,
    "CONSENSUS VIOLATION: Block headers must be 128 bytes (BlockHeader v1)");
```

---

#### ❌ Legacy Field References

**Search and destroy:**
```bash
# Find any remaining .bits references
grep -rn "\.bits\b" src/ include/ | grep -v "ASERT.*BITS" | grep -v "nBits"

# Find any remaining .time references (that aren't .timestamp)
grep -rn "\.time\b" src/ include/ | grep -v "timestamp" | grep -v "uptime"
```

**Replace:**
- `header.bits` → `header.difficulty`
- `header.time` → `header.timestamp`
- `header->bits` → `header->difficulty`
- `header->time` → `header->timestamp`

---

#### ❌ 80/112-Byte Header Paths

**File:** `src/consensus/genesis_canonical.cpp` (line 49)

```cpp
// REVIEW THIS:
if (header.size() == 80) {
    std::copy(header.begin(), header.end(), out.headerLE.begin());
}

// If this is for OLD genesis data, mark as legacy:
if (header.size() == 80) {
    // LEGACY: Old genesis format (pre-BlockHeader v1)
    // This path should never execute after Phase 3
    std::copy(header.begin(), header.end(), out.headerLE.begin());
}

// If this is for NEW genesis, DELETE this check:
// Genesis is 128 bytes now, not 80
```

---

### Add Mandatory Assertions

#### Runtime Assertions (Temporary, Phase 3 Only)

**File:** `src/mining/miner_engine.cpp`

```cpp
// At start of mining loop
assert(sizeof(BlockHeader) == 128 &&
       "FATAL: Mining requires 128-byte BlockHeader v1");

// Before hashing
auto header_bytes = header.SerializeForHash();
assert(header_bytes.size() == 128 &&
       "FATAL: Header must serialize to exactly 128 bytes");
```

**File:** `src/consensus/block_validation.cpp`

```cpp
// At start of validation
static_assert(sizeof(BlockHeader) == 128,
    "Consensus validation requires 128-byte BlockHeader v1");

// After deserialization
auto serialized = header.SerializeForHash();
if (serialized.size() != 128) {
    return reject("bad-header-size",
                  "BlockHeader v1 must be exactly 128 bytes");
}
```

---

#### Compile-Time Assertions (Permanent)

**File:** `include/primitives/block.h`

```cpp
// Verify size
static_assert(sizeof(BlockHeader) == 128,
    "BlockHeader v1 MUST be exactly 128 bytes");

// Verify trivially copyable (performance)
static_assert(std::is_trivially_copyable_v<BlockHeader>,
    "BlockHeader v1 MUST be trivially copyable (memcpy-safe)");

// Verify field offsets
static_assert(offsetof(BlockHeader, version) == 0x00);
static_assert(offsetof(BlockHeader, prev_block_hash) == 0x04);
static_assert(offsetof(BlockHeader, merkle_root) == 0x24);
static_assert(offsetof(BlockHeader, utreexo_root) == 0x44);
static_assert(offsetof(BlockHeader, timestamp) == 0x64);
static_assert(offsetof(BlockHeader, difficulty) == 0x6C);
static_assert(offsetof(BlockHeader, nonce) == 0x70);
static_assert(offsetof(BlockHeader, reserved) == 0x74);
```

---

## The One Acceptable Exception

### Genesis (Block 0) Only:

```cpp
// Genesis is:
// - Hard-coded (not mined)
// - Not validated by PoW
// - Special-cased in InitializeGenesisAndPremine()

// Genesis CAN be 128 bytes without PoW checks
// This is the ONLY block that bypasses PoW
```

**File:** `src/daemon/genesis_init.cpp`

```cpp
static Block LoadGenesisBlock(const ChainParams& params) {
    Block genesis;
    genesis.header.version = params.genesis.nVersion;
    genesis.header.prev_block_hash = uint256(); // All zeros
    genesis.header.timestamp = params.genesis.nTime;
    genesis.header.difficulty = params.genesis.nBits; // 0x1d00ffff
    genesis.header.nonce = params.genesis.nNonce;  // Will be mined
    genesis.header.merkle_root = uint256::FromHexUnsafe(params.genesis.merkleRootHex);
    genesis.header.utreexo_root = uint256(); // All zeros
    genesis.header.ZeroReserved(); // Explicitly zero reserved field

    // Genesis is 128 bytes, but PoW validation is BYPASSED
    // during InitializeGenesisAndPremine()

    return genesis;
}
```

---

## Timeline: What Comes First

### ✅ Correct Order (Already Being Followed):

1. ✅ Finalize BlockHeader v1 (128 bytes) - **DONE**
2. ✅ Update all miners + engines + RPCs - **IN PROGRESS**
3. ✅ Regenerate genesis to match that header - **NEXT**
4. ✅ Hard-code genesis - **AFTER MINING**
5. ✅ Freeze L1 - **AFTER VERIFICATION**
6. 🚀 Launch

### ❌ Never Do This:

- ❌ Regenerate genesis after miners are live
- ❌ Change header layout post-launch
- ❌ Mix 80/112/128-byte headers
- ❌ Allow legacy `bits`/`time` fields after launch

---

## Pre-Genesis Checklist

**Before mining genesis, verify ALL of these:**

### Code Verification:

- [ ] `sizeof(BlockHeader) == 128` (verified in all critical paths)
- [ ] No remaining `.bits` references (except in ASERT constants)
- [ ] No remaining `.time` references (except `.timestamp`)
- [ ] No 80-byte or 112-byte header paths (except legacy docs)
- [ ] All static assertions present and passing
- [ ] getblocktemplate returns `difficulty` (not `bits`)
- [ ] getblocktemplate returns `curtime` (64-bit, not 32-bit `time`)
- [ ] Mining engine uses 128-byte header
- [ ] Block validation enforces 128-byte size
- [ ] Serialization produces exactly 128 bytes

### Miner Verification:

- [ ] External miner can assemble 128-byte header
- [ ] External miner hashes all 128 bytes
- [ ] External miner zeros `reserved[12]`
- [ ] Internal mining engine uses BlockHeader v1
- [ ] No legacy header format support

### RPC Verification:

- [ ] `getblocktemplate` returns correct fields
- [ ] `submitblock` accepts 128-byte headers
- [ ] No legacy field names in JSON output

---

## Post-Genesis Verification

**After Block 0 is mined:**

1. **Verify genesis header:**
   ```bash
   dinero-cli getblock 0 2
   # Check:
   # - Header fields correct
   # - Size = 128 bytes
   # - difficulty = 0x1d00ffff
   # - timestamp = 1772496000
   ```

2. **Mine Block 1 (test):**
   ```bash
   # Get block template
   dinero-cli getblocktemplate '{"address":"din1q..."}'

   # Verify template has:
   # - "difficulty": "0x1d00ffff"
   # - "curtime": <64-bit timestamp>
   # - NO "bits" field
   # - NO 32-bit "time" field
   ```

3. **Verify Block 1 validation:**
   ```bash
   # After Block 1 is mined
   dinero-cli getblock 1 2

   # Check:
   # - Header size = 128 bytes
   # - PoW validated
   # - Difficulty correct
   # - ASERT anchor set
   ```

---

## Final Clarity (One Sentence)

**Yes — your external miner, internal mining engine, templates, validation, and difficulty logic MUST ALL use the 128-byte BlockHeader v1 and final difficulty rules from Block 1 onward.**

---

## Critical Files to Update

### 1. Header Consensus Constants

**File:** `include/consensus/header_consensus.h`

**Change:**
```cpp
// OLD:
static constexpr size_t CURRENT_BLOCK_HEADER_SIZE = 112;

// NEW:
static constexpr size_t CURRENT_BLOCK_HEADER_SIZE = 128;

// OLD:
static_assert(CURRENT_BLOCK_HEADER_SIZE == 112, ...);

// NEW:
static_assert(CURRENT_BLOCK_HEADER_SIZE == 128,
    "BlockHeader v1 must be exactly 128 bytes");
```

---

### 2. Genesis Canonical

**File:** `src/consensus/genesis_canonical.cpp`

**Review line 49:**
```cpp
// If this is legacy genesis handling, mark as such:
if (header.size() == 80) {
    // LEGACY: Pre-BlockHeader v1 format
    // This should NOT execute after Phase 3
    LOG_WARNING("Legacy 80-byte header detected");
}

// For BlockHeader v1, expect 128 bytes:
if (header.size() == 128) {
    std::copy(header.begin(), header.end(), out.headerLE.begin());
}
```

---

### 3. getblocktemplate RPC

**File:** `src/rpc/methods_mining_template.cpp`

**Ensure returns:**
```cpp
result["difficulty"] = FormatCompactBits(template_data.difficulty); // Not "bits"
result["curtime"] = static_cast<uint64_t>(template_data.timestamp);  // Not "time"
result["reserved"] = "000000000000000000000000";  // 12 zero bytes
```

---

## Summary: Transition is Mandatory

**BlockHeader v1 (128 bytes) is active at Block 1.**

**All components must support it:**
- ✅ Miners (external and internal)
- ✅ RPCs (getblocktemplate, submitblock)
- ✅ Validation (consensus)
- ✅ Serialization (exactly 128 bytes)
- ✅ Difficulty (uses `difficulty` field, not `bits`)
- ✅ Timestamp (uses 64-bit `timestamp`, not 32-bit `time`)

**Legacy paths must be removed:**
- ❌ No 80-byte headers
- ❌ No 112-byte headers
- ❌ No `bits` field
- ❌ No 32-bit `time` field

**Once genesis is mined, this is permanent.**

🔒 **128-BYTE BLOCKHEADER V1 (IMMUTABLE FROM BLOCK 1)**
