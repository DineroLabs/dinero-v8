# CONSENSUS LOCK - DO NOT MODIFY

**⚠️ WARNING: This document lists consensus-critical constants that are LOCKED FOREVER.**

Any modification to these values requires a new genesis (hard fork), which means:
- All existing nodes will reject your chain
- All balances will be reset to zero
- The premine will become unspendable
- Years of blockchain history will be lost

**If you're even thinking about changing these, STOP. You probably want to change something else.**

---

## 🔒 NEVER TOUCH THESE

### Genesis Block (Height 0)

```cpp
// File: src/consensus/chainparams_impl.cpp

.genesis_hash = "00000018a388c95d48c7141bb86f131c3538954d57acd07806d1f62bcfa9fd74"
.genesis.nTime = 1772496000       // 2026-03-03 00:00:00 UTC
.genesis.nBits = 0x1d31ffce       // Unified difficulty (50x easier than Bitcoin)
.genesis.nNonce = 2954325912
.genesis.merkleRootHex = "c997af69be7140ede59c8114ae24f36944c4302b4664fdd82b45473f95a6fba1"
```

**Why locked:** The genesis hash is the root of trust for the entire blockchain. Changing any genesis parameter changes the hash, making your chain incompatible with all existing nodes.

---

### Block 1 Premine

```cpp
// File: src/consensus/premine_block_mainnet.hpp

static constexpr const char* PREMINE_SCRIPTPUBKEY = 
    "5120c2a63bf0587d7be826218adea70e91759f85b87ca0aa2adaa8e541e601fa0aa0";

static constexpr const char* PREMINE_ADDRESS = 
    "din1pc2nrhuzc04a7sf3p3t02wr53wk0ctwru5z4z4k4gu4q7vq06p2sqyrrk3s";

static constexpr uint64_t PREMINE_AMOUNT_DIN = 2627900ULL;
static constexpr uint64_t PREMINE_AMOUNT_UNA = 262790000000000ULL;

static constexpr const char* BLOCK_HASH_BE =
    "0000002bd3fa677b43cc4efcb82078db53131f191ad72afb9d463ddd33c7f79a";
```

**Why locked:** The premine scriptPubKey is hardcoded in consensus validation. Changing it makes the premine unspendable. The private key that controls this address exists outside the codebase and cannot be regenerated.

**Critical:** This is a **Taproot (P2TR)** output, not P2WPKH. The scriptPubKey starts with `5120` (OP_1 PUSH32), which is witness version 1. This is architecturally mandated by "Taproot from genesis."

---

### Subsidy Schedule

```cpp
// File: include/consensus/subsidy.h

namespace dinero::consensus {
    constexpr uint32_t FIRST_HALVING_HEIGHT = 2102400;      // ~8 years at 2min blocks
    constexpr uint32_t SUBSIDY_HALVING_INTERVAL = 2102400;  // ~8 years
    constexpr uint64_t INITIAL_SUBSIDY = 10000000000ULL;    // 100 DIN
    constexpr uint64_t PREMINE_UNA = 262790000000000ULL;   // 2,627,900 DIN
}

uint64_t GetBlockSubsidy(uint32_t height, const ChainParams& params);
```

**Why locked:** The subsidy schedule determines the total supply and inflation rate. Changing this breaks the economic model and violates expectations of all stakeholders.

**Economic impact:**
- Block 1: 2,627,900 DIN (premine, one-time)
- Block 2+: 100 DIN per block initially
- First halving at height 2,102,400 → 50 DIN per block
- Continues halving every 2,102,400 blocks
- Max supply: ~21 million DIN (asymptotic)

---

### Network Parameters

```cpp
// File: src/consensus/chainparams_impl.cpp

.network_id = "main"                // Network identifier
.target_spacing = 120               // 2 minutes per block
.pow_limit_bits = 0x1d31ffce        // Unified difficulty (50x easier than Bitcoin)
```

**Why locked:** These parameters define the network identity and are embedded in every block header. Changing them creates an incompatible fork.

---

### Script Semantics (Taproot)

**Architectural mandate:** DineroCoin is "Taproot from genesis."

All outputs MUST be:
- Witness version 1 (Taproot)
- P2TR format: `OP_1 <32-byte x-only pubkey>`
- scriptPubKey: `5120<pubkey>`

**DO NOT:**
- Add P2WPKH (witness v0) support
- Add P2PKH/P2SH legacy formats
- Change witness version semantics
- Modify BIP341 Taproot validation

**Why locked:** This is architectural. Allowing non-Taproot outputs would violate the core design principle and break covenant verification logic.

---

### Utreexo Commitment (128-byte Header)

**Architectural mandate:** Dinero commits the UTXO set via an Utreexo accumulator root embedded in every block header. This field is consensus-critical and immutable.

```cpp
// File: include/mining/header_layout.h

#define DINERO_HEADER_SIZE_BYTES             128
#define DINERO_HEADER_UTREEXO_OFFSET         68
#define DINERO_HEADER_TIMESTAMP_OFFSET       100
#define DINERO_HEADER_DIFFICULTY_OFFSET      108
#define DINERO_HEADER_NONCE_OFFSET           112
#define DINERO_HEADER_RESERVED_OFFSET        116
```

**Header Structure (128 bytes):**

| Field           | Offset | Size | End Byte | Notes |
|-----------------|--------|------|----------|-------|
| version         | 0      | 4    | 3        | little-endian uint32 |
| prev_block_hash | 4      | 32   | 35       | 32-byte hash |
| merkle_root     | 36     | 32   | 67       | 32-byte hash |
| utreexo_root    | 68     | 32   | 99       | 32-byte hash |
| timestamp       | 100    | 8    | 107      | little-endian uint64 |
| difficulty      | 108    | 4    | 111      | compact bits (uint32) |
| nonce           | 112    | 4    | 115      | little-endian uint32 |
| reserved        | 116    | 12   | 127      | MUST be zero |

**Consensus Enforcement:** `src/consensus/block_validation.cpp:148-231`
- Every block's Utreexo commitment is validated against the computed UTXO set state
- Blocks with incorrect commitments are rejected with "bad-utreexo-root"
- This is enforced in `BlockValidator::ConnectBlock()` - same layer as UTXO validation

**Why locked:**
- The Utreexo commitment is part of the block hash (changes block PoW)
- It's embedded at offset 68 in every header mined since genesis
- Mining software (CPU, GPU, Stratum pools) all hash the full 128 bytes
- Removing or relocating this field would invalidate all historical blocks
- This is the cryptographic proof of UTXO set integrity

**DO NOT:**
- Change header size from 128 bytes
- Move Utreexo commitment to a different offset
- Make Utreexo commitment optional or conditional
- Hash only the first 80 bytes (legacy Bitcoin mode)

**Mining Impact:**
- All miners (CPU, GPU, pool) MUST hash 128 bytes
- SHA-256 input is 1024 bits (not 640 bits like Bitcoin)
- Legacy 80-byte mining code will produce invalid blocks
- See: `docs/GPU_MINING_REALITY_CHECK.md` for GPU miner rebuild requirements

---

## ✅ What You CAN Change

These are **safe** to modify without breaking consensus:

### RPC API
- Add new RPC methods
- Change RPC response formats (with versioning)
- Add new parameters to existing RPCs

### Wallet Features
- Add new address types (as long as they're Taproot variants)
- Improve coin selection
- Add new signing methods
- Enhance transaction building

### P2P Protocol
- Add new message types (with version negotiation)
- Optimize block propagation
- Add new service flags

### Mining
- Optimize mining algorithms
- Add new mining RPC methods
- Improve block template building

### Logging and Metrics
- Add new log messages
- Add performance metrics
- Improve error reporting

### GUI/UX
- Change UI layout
- Add new features
- Improve user experience

---

## 🧪 Invariant Tests

Run these tests before **every** commit that touches consensus code:

```bash
# Build and run genesis invariant tests
cmake --build build --target test_genesis_invariants
./build/bin/test_genesis_invariants
```

**All tests MUST pass.** If any fail:
1. STOP immediately
2. Revert your changes
3. Figure out what you broke
4. Fix it without changing consensus constants

---

## 📋 Pre-Launch Checklist

Before mainnet launch, verify:

- [ ] Genesis hash matches: `00000018a388c95d...`
- [ ] Block 1 hash matches: `0000002bd3fa677b...`
- [ ] Premine scriptPubKey is Taproot (starts with `5120`)
- [ ] Premine amount is 2,627,900 DIN exactly
- [ ] Initial subsidy is 100 DIN per block
- [ ] Halving interval is 2,102,400 blocks
- [ ] Target spacing is 120 seconds
- [ ] Network ID is "main"
- [ ] All invariant tests pass
- [ ] Genesis tag exists: `git tag | grep v1.0.0-genesis`

---

## 🆘 What If I Need to Change These?

**Short answer:** You don't. You want to change something else.

**Common scenarios:**

### "I need to change the premine amount"
**Solution:** You can't. The premine is locked at block 1. If you need more funds:
- Mine more blocks (100 DIN per block)
- Request a transfer from the premine holder
- Propose a community fund mechanism (via governance)

### "I need to change the subsidy schedule"
**Solution:** You can't. The economic model is locked. If you want different inflation:
- This is a hard fork - launch a new chain with a different genesis
- Consider tail emission or other mechanisms that don't break existing blocks

### "I need to fix a bug in validation"
**Solution:** Probably OK, but be careful:
- If the bug affects consensus rules → hard fork required
- If the bug only affects non-consensus features → safe to fix
- **Test thoroughly** - run all invariant tests

### "I want to add a new feature"
**Solution:** Usually fine:
- Add new opcodes → soft fork (can be done carefully)
- Add new transaction types → must be Taproot-compatible
- Add new P2P messages → safe with version negotiation

---

## 📚 References

- **BIP 341 (Taproot)**: https://github.com/bitcoin/bips/blob/master/bip-0341.mediawiki
- **BIP 340 (Schnorr)**: https://github.com/bitcoin/bips/blob/master/bip-0340.mediawiki
- **Genesis Tag**: `git show v1.0.0-genesis`
- **Invariant Tests**: `tests/consensus/test_genesis_invariants.cpp`

---

## ⚖️ Legal Notice

This blockchain's consensus rules are defined by its code and genesis block. The constants listed in this document represent a **social contract** with all network participants. Changing them without overwhelming community consensus constitutes a contentious hard fork and may result in:

- Chain split
- Loss of network effect
- Reputational damage
- Legal liability (depending on jurisdiction)

**When in doubt, ask the community. When still in doubt, don't change it.**

---

## 🔮 Optional Future Upgrades

These are **not required now**, just notes for later maturity:

### Optional A — Pre-commit Hook (Local Protection)

Add a local git hook to catch consensus changes before CI:

```bash
# .git/hooks/pre-commit
#!/bin/bash
cd "$(git rev-parse --show-toplevel)"
ctest -R GenesisInvariants --output-on-failure || {
    echo "❌ CRITICAL: Genesis invariant tests failed!"
    echo "Your commit modifies consensus constants."
    echo "This requires explicit hard fork governance approval."
    exit 1
}
```

**Benefits:**
- Catches mistakes before they reach CI
- Instant feedback during development
- Prevents accidental consensus changes from being committed

**When to add:** After mainnet launch, when team discipline is established

---

### Optional B — Signed Governance Tag (Cryptographic Law Snapshot)

Cryptographically sign the consensus lock with GPG:

```bash
# Create signed tag for consensus snapshot
git tag -s consensus-v1.0 -m "Consensus lock: mainnet genesis parameters"

# Verify signature
git tag -v consensus-v1.0

# Push signed tag
git push origin consensus-v1.0
```

**Benefits:**
- Cryptographic proof of what consensus rules were at a given point
- Non-repudiation: can't deny what the rules were
- Audit trail for governance decisions
- Legal defense: "These were the immutable rules, signed by core devs"

**When to add:** Before mainnet launch or first governance vote

**Best practice:** Sign with multi-sig from core maintainers

---

**Last Updated:** 2025-12-25
**Genesis Lock Tag:** v1.0.0-genesis
**Chain Start Date:** 2026-03-03 00:00:00 UTC
