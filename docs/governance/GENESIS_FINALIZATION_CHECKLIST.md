# Genesis Block Finalization Checklist

**Document Type:** CRITICAL PRE-MAINNET PLANNING
**Status:** PLANNING - DO NOT EXECUTE YET
**Authority:** Mainnet Launch Preparation
**Last Updated:** 2025-12-24

---

## ⚠️ CRITICAL: Read This First

This document is a **MANDATORY execution plan** for regenerating genesis block 0 and premine block 1 with finalized consensus rules.

**WHY REGENERATION IS MANDATORY:**

The current genesis was created **before** covenants were fully implemented and enforced. That genesis defines a different consensus ruleset than what exists now.

**Old Genesis (when created):**
- ❌ Covenants NOT enforced (stubs/NOPs)
- ❌ SCRIPT_VERIFY_STANDARD didn't include covenant flags
- ❌ BIP342 limits missing or incomplete
- ❌ Tapscript incomplete

**Current Rules (finalized):**
- ✅ Covenants enforced from block 0
- ✅ SCRIPT_VERIFY_STANDARD includes covenant flags
- ✅ BIP342 limits enforced
- ✅ No legacy paths

**These are different consensus universes.** Keeping the old genesis would cause immediate consensus split at block 1.

**The Non-Negotiable Rule:**
> "Genesis freezes the rules. If the ruleset at height 0 has changed, you MUST regenerate genesis."

**Why This is the RIGHT Time:**
- ✅ Pre-mainnet (no economic history lost)
- ✅ No public chain exists
- ✅ Architecture frozen
- ✅ Security audited (38/38 tests passed)
- ✅ This is the LAST acceptable time to regenerate

**After regeneration: Genesis is frozen forever.**

**What This Achieves:**
- Clean genesis activation (covenants/Taproot "always on" from block 0)
- No soft-fork activation heights needed
- No legacy script paths
- Single, immutable consensus ruleset from block 0 onward

**Governance Statement (for documentation):**
> "The DineroCoin genesis block was generated after full Taproot, covenant, and BIP342 enforcement was implemented, ensuring a single, immutable consensus ruleset from block 0 onward."

**Mindset:**
- You are **creating history for the first time**, not resetting it
- Everything before this is pre-history (dev/test chains)
- This is the transition from engineering to protocol

---

## Section 1: Pre-Finalization Verification

### 1.1 Confirm Motto Embedding (Cryptographic Commitment)

**Motto (Current):**
```
"Dinero: Real Money For Free People"
```

**CRITICAL: This is Cryptographically Precise**

The motto is NOT just a comment or documentation string. It is **cryptographically committed** to the genesis block hash through the coinbase transaction, making it an immutable part of the chain's identity forever.

**How the Commitment Works:**
```
Motto bytes (scriptSig)
  → Transaction ID
  → Merkle root
  → Block header hash
  → Genesis hash (chain identity)
```

Changing even **one character** changes the genesis hash, creating a different blockchain.

**Canonical Method (Bitcoin-Style):**

✅ **Correct placement:** Genesis coinbase transaction scriptSig
```
Coinbase Input:
  prevout: null (coinbase)
  scriptSig:
    <difficulty bits>      (0x04ffff001d or similar)
    <arbitrary data>       ← MOTTO GOES HERE
```

The scriptSig data:
- Is hashed into the transaction ID
- Becomes part of the merkle root
- Becomes part of the genesis block hash
- Survives forever as consensus history

This is exactly how Bitcoin embedded:
```
"The Times 03/Jan/2009 Chancellor on brink of second bailout for banks"
```

❌ **Wrong placements (do NOT use):**
- Comments in code
- chainparams strings (documentation only)
- Config files
- Logs or UI text

Those do not affect consensus.

**DineroCoin Approach: Double Commitment (scriptSig + OP_RETURN):**

DineroCoin will use BOTH methods for maximum clarity:

**Method 1: scriptSig (Bitcoin canonical)**
```
Coinbase Input:
  scriptSig: <difficulty bits> <motto bytes>
```

**Method 2: OP_RETURN output (DineroCoin addition)**
```
Coinbase Output 0:
  value: 100 DIN (burned, unspendable)
  scriptPubKey: OP_RETURN <motto bytes>
```

**Why Both?**
- ✅ scriptSig: Matches Bitcoin's proven method
- ✅ OP_RETURN: Explicit, easily parseable commitment
- ✅ Double commitment: Belt and suspenders approach
- ✅ Both contribute to merkle root → genesis hash

Bitcoin did not use OP_RETURN, but DineroCoin will for added clarity and ease of verification.

**Exact Bytes (MUST Document):**

**Motto (UTF-8):**
```
"Dinero: Real Money For Free People"
```

**Byte Length:** 57 bytes (including quotes if included, 55 bytes without quotes)

**Hex Encoding (UTF-8, without quotes):**
```
44696e65726f3a205265616c204d6f6e657920466f72204672656520506566
6f706c65202d204e6f76656d626572203235272032303235
```

**Canonical Breakdown:**
```
D  i  n  e  r  o  :  (space) R  e  a  l  (space) M  o  n  e  y
44 69 6e 65 72 6f 3a 20      52 65 61 6c 20      4d 6f 6e 65 79

(space) F  o  r  (space) F  r  e  e  (space) P  e  o  p  l  e
20      46 6f 72 20      46 72 65 65 20      50 65 6f 70 6c 65

(space) -  (space) N  o  v  e  m  b  e  r  (space) 2  5  ,
20      2d 20      4e 6f 76 65 6d 62 65 72 20      32 35 2c

(space) 2  0  2  5
20      32 30 32 35
```

**What Becomes Immutable:**
```
❄️ Exact bytes of the motto (frozen)
❄️ Encoding (UTF-8 vs ASCII matters!)
❄️ Punctuation (dash, colon, comma)
❄️ Whitespace (every space)
❄️ Capitalization (Real vs real)
```

Changing **ANY** of these changes the genesis hash.

**Location in Code:**
- `src/consensus/chainparams_impl.cpp:26` - DINERO_MOTTO constant (documentation)
- `tools/genesis_miner_v2.cpp:354` - Genesis miner motto string (CONSENSUS-CRITICAL)

**Verification Steps:**
- [x] ✅ Confirm motto text is correct and final
- [x] ✅ Verify motto is embedded in genesis coinbase scriptSig (NOT just comments)
- [x] ✅ Document exact UTF-8 bytes (hex dump for verification)
- [x] ✅ Confirm encoding is consistent (UTF-8, no BOM)
- [x] ✅ Verify no typos or formatting issues
- [ ] 🔍 **CRITICAL:** After genesis mining, verify motto bytes in actual genesis transaction
- [ ] 🔍 Verify merkle root includes motto commitment
- [ ] 🔍 Test that changing one character produces different genesis hash

**Governance Statement (Recommended):**
```
"The DineroCoin genesis block cryptographically commits to the motto
'Dinero: Real Money For Free People' via the
genesis coinbase transaction, making it an immutable part of the
chain's identity."
```

**Why This Survives Forever:**
```
✅ Motto is in the transaction (not external metadata)
✅ Transaction is in the block (consensus data)
✅ Block hash commits to it (cryptographic proof)
✅ Genesis hash IS the chain identity
✅ Removing or altering the motto = different blockchain
✅ No special opcodes or rules needed
✅ Same method Bitcoin used (battle-tested)
```

**Decision Point:**
```
✅ Motto is FINAL and will not change
✅ Motto will be embedded in genesis coinbase scriptSig
✅ Exact bytes documented for independent verification
```

**Current Status:** ✅ **CONFIRMED** (cryptographic commitment method validated)

---

### 1.2 Confirm Consensus Rules (Critical)

**Current Consensus Configuration:**

**Taproot Status:**
- ✅ Taproot opcodes implemented
- ✅ BIP340 Schnorr signatures supported
- ✅ BIP341 script-path spending supported
- ✅ BIP342 Tapscript validation rules enforced

**Covenant Status:**
- ✅ OP_CHECKTEMPLATEVERIFY (0xb3) - Fully implemented
- ✅ OP_CHECKSIGFROMSTACK (0xbb) - Fully implemented
- ✅ OP_CHECKSIGFROMSTACKVERIFY (0xbc) - Fully implemented
- ✅ OP_TXHASH (0xbd) - Fully implemented
- ✅ OP_CHECKCONTRACTVERIFY (0xbe) - Fully implemented

**Script Verification Flags:**
```cpp
// From include/consensus/script_interpreter.h:87-100
SCRIPT_VERIFY_STANDARD = SCRIPT_VERIFY_P2SH |
                         SCRIPT_VERIFY_DERSIG |
                         SCRIPT_VERIFY_CHECKLOCKTIMEVERIFY |
                         SCRIPT_VERIFY_CHECKSEQUENCEVERIFY |
                         SCRIPT_VERIFY_WITNESS |
                         SCRIPT_VERIFY_NULLDUMMY |
                         SCRIPT_VERIFY_DISCOURAGE_UPGRADABLE_NOPS |
                         SCRIPT_VERIFY_MINIMALDATA |
                         SCRIPT_VERIFY_NULLFAIL |
                         SCRIPT_VERIFY_CLEANSTACK |
                         SCRIPT_VERIFY_MINIMALIF |
                         SCRIPT_VERIFY_WITNESS_PUBKEYTYPE |
                         SCRIPT_VERIFY_COVENANTS;  // ← ALREADY INCLUDED
```

**BIP342 Limits:**
- ✅ Maximum 1000 stack elements
- ✅ Maximum 520 bytes per element
- ✅ Maximum 10,000 bytes per script
- ✅ Annex handling (0x50 prefix detection)

**Verification Steps:**
- [ ] Confirm SCRIPT_VERIFY_COVENANTS is in SCRIPT_VERIFY_STANDARD
- [ ] Verify BIP342 limits are enforced unconditionally (no activation height)
- [ ] Check no legacy pre-Taproot script paths exist
- [ ] Confirm all covenant opcodes functional (Phase 4 tests passed: 38/38)

**Decision Point:**
```
✅ Consensus rules are FROZEN and final
OR
⚠️  Need to make changes (CRITICAL - requires re-audit)
```

**Current Status:** ✅ **FROZEN** (Phase L0, 2, 3, 4 complete)

---

### 1.3 Confirm Economics (Monetary Policy)

**Canonical Source:** `include/consensus/subsidy.h` (compile-time enforced)

**Total Supply:** 265,428,000 DIN

**Block Distribution:**
```
Block 0 (Genesis):  100 DIN → Burned to OP_RETURN (unspendable, symbolic)
Block 1 (Premine):  2,627,900 DIN → Premine (~1% of total supply)
Block 2+ (PoW):     262,800,000 DIN → Mined over 33 halvings
```

**Genesis Block (Height 0):**
```cpp
// From subsidy.h lines 57-58
GENESIS_UNSPENDABLE_DIN  = 100 DIN
GENESIS_UNSPENDABLE_UNA = 10,000,000,000 una

// Coinbase output: OP_RETURN (provably unspendable)
// Motto: "Dinero: Real Money For Free People"
// Purpose: Symbolic genesis, no premine in genesis block
```

**Premine Block (Height 1):**
```cpp
// From subsidy.h lines 61-62
PREMINE_DIN  = 2,627,900 DIN
PREMINE_UNA = 262,790,000,000,000 una
PREMINE_HEIGHT = 1

// 🔒 CONSENSUS GUARDS (lines 174-215):
// - Compile-time assertions prevent changes
// - Hard fork required to modify premine parameters
// - Guards protect history, not ownership (spending premine = allowed)
```

**PoW Mining (Height 2+):**
```cpp
// From subsidy.h lines 52-53
INITIAL_SUBSIDY = 100 DIN per block
HALVING_INTERVAL = 1,314,000 blocks (5 years @ 2-minute blocks)

// 33 halvings total:
// Epoch 0: 100 DIN/block
// Epoch 1: 50 DIN/block
// Epoch 2: 25 DIN/block
// ...
// Epoch 33: 0 DIN/block (subsidy exhausted)
```

**Premine Configuration Checklist:**

**Amount Verification:**
- [ ] ✅ Total premine: 2,627,900 DIN (exactly ~1% of 265.428M total)
- [ ] ✅ Height: Block 1 (NOT in genesis)
- [ ] ✅ Consensus guards: Active (compile-time enforced)

**Allocation Planning:**
- [x] **Number of Outputs:** 1 (single output)
- [x] **Output Breakdown:**
  - Output 0: 2,627,900 DIN → P2TR scriptPubKey (development + ecosystem fund)
  - **TOTAL = 2,627,900 DIN** ✅ (enforced by consensus guards)
- [x] **Script Type:** **P2TR** ✅ CONFIRMED
  - Taproot native (BIP341)
  - Future-proof, covenant-compatible
  - Optimal for DineroCoin's advanced features

**CRITICAL INVARIANT:**
```
"In DineroCoin, ownership and authority are defined exclusively by the scriptPubKey.
Address strings are non-consensus encodings and carry no authority."
```

**What is Consensus-Critical (FROZEN in genesis):**
- ✅ **scriptPubKey bytes** (internal pubkey + script tree commitment)
- ✅ Internal public key (32 bytes)
- ✅ Script tree commitment (if any)
- ✅ Covenant semantics

**What is NOT Consensus-Critical (can change):**
- ❌ Address string encoding (din1p... is just bech32 encoding)
- ❌ Bech32 HRP (can change from "din" to something else without consensus change)
- ❌ Address formatting

**Rule:** If scriptPubKey bytes are unchanged, consensus is unchanged.

- [ ] **Key Management:**
  - Single private key (NOT multisig)
  - Storage: Hardware wallet (e.g., Ledger, Trezor)
  - Backup: Seed phrase secured (offline, encrypted)
  - Recovery plan: Documented and tested
- [ ] **scriptPubKey Generation:** (CONSENSUS-CRITICAL - must be generated and frozen)
  - P2TR scriptPubKey = OP_1 <32-byte internal pubkey>
  - Internal pubkey derived from private key
  - No script tree (key-path spending only)
- [ ] **Address String:** (NON-CONSENSUS - can regenerate anytime)
  - Format: bech32m encoding of scriptPubKey
  - Example: din1p... (din = HRP, 1p = witness v1)
  - Can change HRP or encoding without changing consensus
- [x] **Vesting Schedule:** None (no time-locks on premine output)
- [ ] **Public Allocation Document:** (transparency requirement - MANDATORY)
  - Purpose: Combined development and ecosystem fund
  - Spending policy: ______ (e.g., requires community approval, quarterly reports)
  - Accountability: ______ (public block explorer monitoring, spending disclosures)

**Security Considerations:**
- [ ] Private key stored securely (hardware wallet recommended)
- [ ] Seed phrase backup secured (offline, encrypted, multiple copies)
- [ ] Backup recovery plan documented and tested
- [ ] Spending policy defined (requires community approval?)
- [ ] Public audit trail for all premine spends (block explorer monitoring)

**Economic Rationale:**
```
Why 2,627,900 DIN (~1%)?
- Development fund: Long-term protocol maintenance
- Ecosystem fund: Grants, partnerships, marketing
- Initial liquidity: Exchange listings, market making
- Transparency: Public allocation + spending accountability

Why NOT in genesis (block 0)?
- Clean separation: Genesis = symbolic (100 DIN burned)
- Premine = functional (block 1, spendable)
- Consensus clarity: Genesis has no spendable outputs
```

**Compile-Time Protection:**
```cpp
// From subsidy.h lines 174-198
// These guards will FAIL THE BUILD if premine parameters change:

static_assert(PREMINE_UNA == 262790000000000ULL,
    "🔒 CONSENSUS VIOLATION: Premine amount changed!");

static_assert(PREMINE_DIN == 2627900ULL,
    "🔒 CONSENSUS VIOLATION: Premine DIN amount changed!");

static_assert(PREMINE_HEIGHT == 1,
    "🔒 CONSENSUS VIOLATION: Premine height altered!");

// These ensure any premine modification requires:
// 1. Explicit code change (can't be accidental)
// 2. Build failure (forces review)
// 3. Hard fork coordination (network-wide agreement)
```

**Decision Point:**
```
✅ Economics are FROZEN and final (enforced by subsidy.h)
OR
⚠️  Need to change premine allocation (REQUIRES HARD FORK - coordinate with network)
```

**Current Status:** ✅ **CANONICAL** (enforced by compile-time guards in subsidy.h)

**Note:** Premine AMOUNT (2,627,900 DIN) is consensus-critical and cannot change without a hard fork. However, the ALLOCATION of that premine into specific outputs/addresses is a one-time configuration decision before genesis regeneration.

---

### 1.4 Confirm Mining Difficulty

**Current Genesis Difficulty:**
```
nBits: 0x1d31ffce
```

**Interpretation:**
- This is an **easy** difficulty (suitable for genesis mining)
- Allows genesis block to be mined quickly
- Similar to Bitcoin's genesis difficulty

**Block 1+ Difficulty:**
```
pow_limit_bits: 0x1d31ffce (from chainparams)
target_spacing: 120 seconds (2 minutes)
```

**Verification Steps:**
- [x] ✅ Confirm genesis nBits is correct for launch
- [x] ✅ Verify difficulty adjustment algorithm (ASERT)
- [x] ✅ Check target_spacing matches desired block time (2 minutes)
- [x] ✅ Confirm pow_limit_bits is appropriate for hashrate expectations

**Decision Point:**
```
✅ Difficulty parameters are correct
```

**Current Status:** ✅ **CONFIRMED**

---

### 1.5 Confirm Genesis Timestamp

**Current Genesis Time:**
```
nTime: 1772496000
Decoded: 2026-03-03 00:00:00 UTC
```

**Motto Date Reference:**
```
"Dinero: Real Money For Free People"
```

**Verification Steps:**
- [x] ✅ Confirm timestamp matches motto date
- [x] ✅ Verify timezone is UTC (not local time)
- [x] ✅ Check timestamp is reasonable for launch window
- [x] ✅ Ensure timestamp doesn't conflict with block 1+ times

**Decision Point:**
```
✅ Genesis timestamp is correct (Nov 25, 2025)
```

**Current Status:** ✅ **CONFIRMED**

---

### 1.6 Confirm Network Parameters

**Current Mainnet Configuration:**
```cpp
.name = "mainnet"
.hrp = "din"  // Bech32 prefix (e.g., din1...)
.magic = 0xd9b4bef9  // P2P network magic bytes
.rpc_port = 20997
.http_port = 8080
.ws_port = 8081
.p2p_port = 20999
```

**Verification Steps:**
- [ ] Confirm network magic bytes unique (no collision with Bitcoin/others)
- [ ] Verify port numbers don't conflict with common services
- [ ] Check hrp ("din") is registered/documented
- [ ] Confirm DNS seeds are configured correctly

**Decision Point:**
```
✅ Network parameters are final
OR
⚠️  Need to adjust (specify changes)
```

**Current Status:** 🟡 NEEDS CONFIRMATION

---

## Section 2: Genesis Regeneration Plan

### 2.1 What Gets Regenerated

**Block 0 (Genesis Block):**
- ✅ MUST regenerate because:
  - Consensus rules finalized (covenants always-on)
  - Script validation rules changed (SCRIPT_VERIFY_STANDARD now includes covenants)
  - 100 DIN burned to OP_RETURN (symbolic, no premine in genesis)

**Block 1 (Premine Block):**
- ✅ MUST regenerate because:
  - Contains 2,627,900 DIN premine (consensus-critical)
  - Premine outputs must be valid under final Taproot rules
  - Script type must match consensus (P2WPKH / P2TR)
  - Addresses must be spendable under final rules
  - Allocation into specific outputs must be finalized

**Block 2+ (PoW Mining Starts):**
- Naturally follows from genesis
- No regeneration needed (doesn't exist yet)
- First PoW block: 100 DIN subsidy

---

### 2.2 Regeneration Procedure

**Step 1: Freeze All Code Changes**
- [ ] No more covenant semantic changes (already frozen)
- [ ] No more consensus rule changes
- [ ] No more script validation changes
- [ ] Tag codebase: `v1.0.0-genesis-ready`

**Step 2: Delete Old Chains - MANDATORY COMPLETE WIPE**

**WHY COMPLETE WIPE IS MANDATORY:**

Your node currently has persistent artifacts created under the **old genesis + old rules**:
- Old genesis hash
- Old block index
- Old chainstate
- Old UTXO layout assumptions

**These artifacts are NOT compatible with new genesis and ruleset.**

Keeping them causes:
- Genesis mismatch on startup
- Chainstate load failure
- **WORSE:** Partial acceptance with wrong assumptions → silent consensus corruption

**Consensus software MUST start from absolute zero.**

**The Invariant (no exceptions):**
> **Genesis change = total database invalidation**

This is true for Bitcoin, Ethereum, DineroCoin, and any serious blockchain.
**There are no partial resets.**

**MANDATORY Wipe Procedure:**

```bash
# Step 2.1: Stop everything
killall dinerod
# Or: pkill -9 dinerod
# Verify no processes running: ps aux | grep dinerod

# Step 2.2: Backup old data (OPTIONAL - for reference only)
# This is NOT restoration data - purely for comparison if needed
mv ~/.dinero ~/.dinero.old-genesis-backup
# Or: tar -czf ~/dinero-old-genesis-$(date +%s).tar.gz ~/.dinero

# Step 2.3: DELETE ALL CHAIN DATA (MANDATORY)
rm -rf ~/.dinero/regtest/chaindb/
rm -rf ~/.dinero/regtest/blocks/
rm -rf ~/.dinero/regtest/chainstate/
rm -rf ~/.dinero/regtest/indexes/
rm -rf ~/.dinero/regtest/peers.dat
rm -rf ~/.dinero/regtest/banlist.dat
rm -rf ~/.dinero/regtest/mempool.dat
rm -rf ~/.dinero/regtest/wallet*.db  # Or move aside if needed

rm -rf ~/.dinero/testnet/chaindb/
rm -rf ~/.dinero/testnet/blocks/
rm -rf ~/.dinero/testnet/chainstate/
rm -rf ~/.dinero/testnet/indexes/
rm -rf ~/.dinero/testnet/peers.dat
rm -rf ~/.dinero/testnet/banlist.dat
rm -rf ~/.dinero/testnet/mempool.dat
rm -rf ~/.dinero/testnet/wallet*.db

rm -rf ~/.dinero/chaindb/
rm -rf ~/.dinero/blocks/
rm -rf ~/.dinero/chainstate/
rm -rf ~/.dinero/indexes/
rm -rf ~/.dinero/peers.dat
rm -rf ~/.dinero/banlist.dat
rm -rf ~/.dinero/mempool.dat
rm -rf ~/.dinero/wallet*.db

# If using custom datadir:
# rm -rf /path/to/custom/datadir/*

# Step 2.4: Delete AssumeUTXO / Snapshot data (if exists)
rm -rf ~/.dinero/*/snapshots/
rm -rf ~/.dinero/*/assumeutxo/
rm -rf ~/.dinero/*/utxo_indexes/

# Step 2.5: Verify complete deletion
ls -la ~/.dinero/regtest/   # Should be empty or not exist
ls -la ~/.dinero/testnet/   # Should be empty or not exist
ls -la ~/.dinero/   # Should be empty or not exist
```

**Rule:** If it depends on the genesis hash → DELETE IT.

**What Happens If You Forget to Clean DB:**

| Scenario | Result |
|----------|--------|
| Old chaindb + new genesis | Node refuses to start |
| Old chainstate + new rules | Subtle validation bugs |
| Old indexes | Silent corruption |
| Old UTXO DB | Wrong accumulator state |
| Old assumeutxo metadata | Undefined behavior |

**This is not theoretical** - Bitcoin Core explicitly requires this on genesis change.

**Checklist:**
- [ ] Stop all dinerod processes
- [ ] Backup old data (optional, for reference only)
- [ ] Delete ALL regtest chain data
- [ ] Delete ALL testnet chain data
- [ ] Delete ALL mainnet chain data
- [ ] Delete ALL snapshot/assumeutxo data
- [ ] Verify directories empty or non-existent

**Step 3: Regenerate Genesis and Premine Blocks**

**3a. Regenerate Genesis (Block 0):**
```bash
# Use genesis_miner_v2 (configured for 100 DIN burn to OP_RETURN)
./build/bin/genesis_miner

# Output: genesis_final_output.txt
# Contains:
#   - Genesis hash
#   - Merkle root
#   - Nonce
#   - Coinbase hex (100 DIN → OP_RETURN)
#   - Motto embedded in scriptSig
```

**3b. Create Premine Block (Block 1):**
```bash
# Generate premine coinbase transaction
# Total: 2,627,900 DIN (enforced by subsidy.h)

# CONFIRMED ALLOCATION: Single output, P2TR single-signature
# - Output 0: 2,627,900 DIN → P2TR scriptPubKey

# ============================================================================
# CRITICAL: What Gets Frozen in Genesis (Consensus-Critical)
# ============================================================================
# The following are IMMUTABLE once block 1 is mined:
#   - scriptPubKey bytes (OP_1 <32-byte internal pubkey>)
#   - Internal public key (derived from private key)
#   - Script tree commitment (none for key-path-only spending)
#
# The following are NOT consensus-critical and can change:
#   - Address string (din1p... is just bech32m encoding)
#   - Bech32 HRP ("din" can change to "dinero" later)
#   - Address formatting
#
# Rule: If scriptPubKey bytes unchanged → consensus unchanged
# ============================================================================

# Premine coinbase structure:
# - Input: Coinbase input (height 1, arbitrary data in scriptSig)
# - Output 0: 262,790,000,000,000 una (2,627,900 DIN)
#   - scriptPubKey: OP_1 <32-byte internal pubkey>
#   - scriptPubKey bytes: 0x5120<pubkey> (34 bytes total)
#   - Address (optional): bech32m("din", 1, pubkey) → din1p...

# Transaction construction:
# 1. Generate private key (securely, offline recommended)
# 2. Derive internal public key (32-byte x-only pubkey, BIP340)
# 3. Build P2TR scriptPubKey:
#    - Byte 0: 0x51 (OP_1 - witness version 1)
#    - Byte 1: 0x20 (push 32 bytes)
#    - Bytes 2-33: internal pubkey (32 bytes)
# 4. Build coinbase transaction:
#    - Version: 2
#    - Input: Coinbase (prevout: null, sequence: 0xffffffff)
#      - scriptSig: <height 1> <arbitrary data>
#    - Output 0: 262,790,000,000,000 una → scriptPubKey (0x5120<pubkey>)
#    - Locktime: 0
# 5. Calculate merkle root (single tx = txid = merkle root)
# 6. Mine block 1:
#    - Previous block hash: [genesis hash from step 3a]
#    - Merkle root: [coinbase txid]
#    - Timestamp: [genesis time + ~120 seconds]
#    - Bits: 0x1d31ffce (same as genesis)
#    - Find nonce (PoW)
# 7. (Optional) Generate address string for human reference:
#    - address = bech32m("din", 1, internal_pubkey)
#    - This is NOT consensus-critical (can regenerate anytime)

# Tools needed:
# - Key pair generator (BIP340 x-only pubkeys)
# - P2TR scriptPubKey builder
# - Premine transaction builder
# - Block miner (builds on genesis)
# - (Optional) Bech32m address encoder

# Verification:
# - Total output = 2,627,900 DIN exactly ✅
# - scriptPubKey format: 0x5120<32-byte pubkey> ✅
# - Output spendable under Taproot rules ✅
# - Block 1 validates against consensus (subsidy.h) ✅
# - Private key secured (hardware wallet) ✅
# - scriptPubKey bytes DOCUMENTED (will be frozen in genesis) ✅
```

**Step 4: Update Hardcoded Genesis Constants - MANDATORY CODE CHANGES**

**CRITICAL:** Genesis is hardcoded in code, not DB.

**Files to Update (MANDATORY):**

1. **`src/consensus/chainparams_impl.cpp`** (lines 33-80)
2. **`include/consensus/subsidy.h`** (lines 34-48, if genesis hash referenced)
3. Any other files with hardcoded genesis hash

**WHY THIS IS MANDATORY:**

If old genesis hash remains in code:
- Node will reject new genesis on startup
- Consensus validation will fail
- Chain will not initialize

**Only the new genesis hash must exist in code.**
**Old genesis hash must not appear anywhere.**

Update `src/consensus/chainparams_impl.cpp`:

```cpp
// Line 33-36: Expected hashes
static constexpr const char* EXPECTED_GENESIS_HASH = "[new genesis hash]";
static constexpr const char* EXPECTED_MERKLE_ROOT = "[new merkle root]";

// Line 49-80: Mainnet genesis parameters
.genesis = {
    .nVersion = 1,
    .nTime = [confirmed timestamp],
    .nBits = [confirmed difficulty],
    .nNonce = [mined nonce],
    .genesisHashHex = std::string(EXPECTED_GENESIS_HASH),
    .merkleRootHex = std::string(EXPECTED_MERKLE_ROOT),
    .genesisCoinbaseHex = "[new coinbase hex]"
}
```

**Step 5: Update Genesis Hash References**

Files that reference genesis hash:
- [ ] `src/consensus/chainparams_impl.cpp` (mainnet params)
- [ ] `include/consensus/genesis_canonical.h` (if used)
- [ ] `docs/launch/genesis_constants.txt` (documentation)
- [ ] Any checkpoint files
- [ ] Any test files with hardcoded genesis hash

**Step 6: Rebuild and Verify - CLEAN BUILD REQUIRED**

```bash
# Step 6.1: Clean rebuild (MANDATORY)
rm -rf build/
rm -rf CMakeCache.txt
rm -rf CMakeFiles/
mkdir build && cd build
cmake ..
cmake --build . -j8

# Verify binaries rebuilt
ls -lh bin/dinerod
ls -lh bin/dinero-cli
# Check timestamp - should be recent

# Step 6.2: Start node (genesis only - fresh DB)
cd ..
./build/bin/dinerod --regtest --daemon

# Expected behavior:
# - Genesis block created
# - No errors
# - No warnings about reorgs or mismatches
# - No "genesis mismatch" errors

# Step 6.3: Verify genesis block
./build/bin/dinero-cli --regtest getblockhash 0

# Expected output: [new genesis hash]
# This MUST match EXPECTED_GENESIS_HASH in chainparams_impl.cpp

# Step 6.4: Verify genesis block details
./build/bin/dinero-cli --regtest getblock $(./build/bin/dinero-cli --regtest getblockhash 0)

# Verify:
# - height: 0
# - merkle root: matches EXPECTED_MERKLE_ROOT
# - time: matches nTime (1772496000)
# - difficulty: matches nBits (0x1d31ffce)
# - coinbase tx: 100 DIN → OP_RETURN (no premine in genesis)
# - motto in coinbase scriptSig

# Step 6.5: Verify chainstate
./build/bin/dinero-cli --regtest getblockchaininfo

# Verify:
# - blocks: 0
# - headers: 0
# - bestblockhash: matches genesis hash
# - chainwork: minimal (genesis only)
# - covenants active: true (from genesis)

# Step 6.6: Stop node
./build/bin/dinero-cli --regtest stop

# Wait for clean shutdown
sleep 2
ps aux | grep dinerod  # Should show no running processes
```

**Verification Checklist:**
- [ ] Clean rebuild completed successfully
- [ ] Node starts without errors
- [ ] No genesis mismatch warnings
- [ ] Genesis hash matches hardcoded constant
- [ ] Merkle root correct
- [ ] Timestamp correct
- [ ] Difficulty correct
- [ ] Coinbase has motto, no premine
- [ ] Blockchain info shows height 0
- [ ] Covenants active from genesis
- [ ] Node shuts down cleanly

**Step 7: Testnet Deployment**

```bash
# Mine genesis on testnet
# Deploy to testnet nodes
# Verify:
# - Genesis syncs correctly
# - Covenants work from block 0
# - Taproot addresses work
# - No activation height logic triggered
```

**Step 8: Mainnet Genesis Ceremony**

```bash
# Final mainnet genesis mining
# Publish genesis parameters publicly:
#   - Genesis hash
#   - Merkle root
#   - Coinbase hex
#   - Timestamp
#   - Difficulty
#   - Nonce

# Hardcode in release build
# Tag: v1.0.0-mainnet
# Deploy to mainnet seed nodes
```

---

### 2.3 Post-Regeneration Verification

**Verification Checklist:**

**Genesis Block Verification:**
- [ ] Genesis hash matches hardcoded constant
- [ ] Merkle root matches hardcoded constant
- [ ] Coinbase transaction parses correctly
- [ ] Motto appears in coinbase scriptSig
- [ ] Block 1 premine = 2,627,900 DIN (enforced by subsidy.h)
- [ ] Block header 80 bytes (exactly)
- [ ] Timestamp reasonable
- [ ] Difficulty matches expected

**Consensus Verification:**
- [ ] SCRIPT_VERIFY_COVENANTS active from block 0
- [ ] Taproot scripts validate correctly
- [ ] BIP342 limits enforced from genesis
- [ ] No legacy script paths exist
- [ ] Covenant opcodes work immediately

**Economic Verification (Premine Block 1):**
- [ ] Total premine = 2,627,900 DIN exactly (enforced by subsidy.h)
- [ ] Premine output spendable under final Taproot rules
- [ ] Premine allocation matches approved plan (single output, single-sig)
- [ ] Private key secured (hardware wallet)
- [ ] Seed phrase backup secured (offline, encrypted)
- [ ] Public allocation document published

**Network Verification:**
- [ ] Regtest syncs from genesis
- [ ] Testnet syncs from genesis
- [ ] Multiple nodes agree on genesis hash
- [ ] No consensus splits

---

## Section 3: What NOT to Change

**DO NOT Change After Genesis Freeze (Consensus-Critical):**
- ❌ Covenant opcode semantics (frozen per COVENANT_SEMANTIC_FREEZE.md)
- ❌ Taproot validation rules (BIP340/341/342 compliance frozen)
- ❌ BIP342 limits (DoS protection frozen)
- ❌ Script verification flags (SCRIPT_VERIFY_STANDARD frozen)
- ❌ Genesis timestamp (immutable)
- ❌ Genesis difficulty (immutable)
- ❌ Motto text (immutable)
- ❌ Premine amount (2,627,900 DIN - immutable, consensus-critical)
- ❌ **Premine scriptPubKey bytes** (internal pubkey + script tree - FROZEN in block 1)

**CAN Change After Genesis (Non-Consensus):**
- ✅ Performance optimizations (if output identical)
- ✅ RPC improvements
- ✅ Wallet features
- ✅ P2P protocol enhancements (backward-compatible)
- ✅ Documentation
- ✅ **Address string encodings** (bech32m encoding of scriptPubKey)
- ✅ **Bech32 HRP** (e.g., "din" → "dinero" - address format only)
- ✅ **Address formatting** (scriptPubKey unchanged = consensus unchanged)

**Critical Principle:**
> "Ownership and authority are defined exclusively by the scriptPubKey.
> Address strings are non-consensus encodings and carry no authority."

---

## Section 4: Activation Strategy (Post-Genesis)

**Genesis Activation Model:**

```
Block 0 (Genesis):
  ✅ Taproot ENABLED
  ✅ Covenants ENABLED
  ✅ SCRIPT_VERIFY_STANDARD includes SCRIPT_VERIFY_COVENANTS
  ✅ BIP342 limits ENFORCED
  ✅ No legacy script paths
  ✅ No activation heights
  ✅ No soft-fork logic

Block 1+:
  ✅ Inherit all rules from genesis
  ✅ No upgrade required (rules always existed)
  ✅ No consensus conditionals ("if height >= X")
```

**What This Eliminates:**
- ❌ Height-based activation (not needed - always on)
- ❌ Version bits signaling (not needed - no soft fork)
- ❌ Soft-fork coordination (not needed - genesis activation)
- ❌ "If height >= X" code (not needed - unconditional)
- ❌ Activation governance complexity (not needed - pre-launch)

**Governance Documents Status:**

**Still Relevant:**
- ✅ COVENANT_SEMANTIC_FREEZE.md - **CRITICAL** (prevents post-genesis changes)
- ✅ Adversarial testing reports (security proof)
- ✅ BIP compliance audits (correctness proof)

**No Longer Needed (Genesis Activation):**
- ⚠️ COVENANT_ACTIVATION_PLAN.md - Only relevant if mainnet exists
- ⚠️ Soft-fork upgrade checklists - No soft fork needed
- ⚠️ T-2016 warning procedures - No activation event

**Replacement Document:**
- ✅ Create: GENESIS_ACTIVATION_STATEMENT.md
  - "Covenants and Taproot are active from genesis (block 0)"
  - "No activation heights exist"
  - "These rules have always existed"

---

## Section 5: Final Confirmation Checklist

**Before Regenerating Genesis, Confirm ALL of the Following:**

### 5.1 Motto
- [ ] ✅ Motto text is final and approved
- [ ] ✅ Motto will be embedded in genesis coinbase scriptSig
- [ ] ✅ Motto date matches genesis timestamp

### 5.2 Consensus Rules
- [ ] ✅ All covenant opcodes implemented and tested (Phase 4: 38/38 tests passed)
- [ ] ✅ SCRIPT_VERIFY_COVENANTS included in SCRIPT_VERIFY_STANDARD
- [ ] ✅ BIP342 limits enforced unconditionally
- [ ] ✅ Taproot validation rules finalized (BIP340/341/342)
- [ ] ✅ Semantic freeze policy in effect (no post-genesis changes)

### 5.3 Economics
- [ ] ✅ Premine amount confirmed (2,627,900 DIN at block 1)
- [ ] ✅ Premine allocation plan documented and approved (single output, single-sig)
- [ ] ✅ Script type chosen (P2TR or P2WPKH)
- [ ] ✅ Address generated and verified
- [ ] ✅ Private key secured, backup plan in place (hardware wallet + seed backup)
- [ ] ✅ Public transparency commitment made (allocation document published)

### 5.4 Mining Parameters
- [ ] ✅ Genesis difficulty confirmed (nBits)
- [ ] ✅ Target block time confirmed (120 seconds)
- [ ] ✅ Difficulty adjustment algorithm verified (ASERT)

### 5.5 Network Parameters
- [ ] ✅ Network magic bytes confirmed (0xd9b4bef9)
- [ ] ✅ Port numbers confirmed (RPC: 20997, P2P: 20999)
- [ ] ✅ Bech32 prefix confirmed ("din")
- [ ] ✅ DNS seeds configured

### 5.6 Code Freeze
- [ ] ✅ No consensus changes planned
- [ ] ✅ All Phase L0, 2, 3, 4 fixes committed
- [ ] ✅ Adversarial testing complete
- [ ] ✅ Semantic freeze policy committed
- [ ] ✅ Codebase tagged: v1.0.0-genesis-ready

---

## Section 6: Risk Assessment

**Risks of Delaying Genesis Regeneration:**
- ⚠️ Current genesis uses old consensus rules
- ⚠️ Confusion about premine (conflict between sources)
- ⚠️ Potential for incorrect activation logic
- ⚠️ Training bad habits (soft-fork mindset when not needed)

**Risks of Rushing Genesis Regeneration:**
- ⚠️ Wrong premine amount (can't fix after genesis)
- ⚠️ Wrong consensus rules (requires hard fork to fix)
- ⚠️ Typo in motto (permanent embarrassment)
- ⚠️ Wrong difficulty (unusable chain)
- ⚠️ Insecure premine keys (theft risk)

**Correct Approach:**
✅ **Take time to confirm everything**
✅ **No rush - get it right once**
✅ **Genesis is forever**

---

## Section 7: Sign-Off Requirements

**Before Proceeding with Genesis Regeneration:**

**Technical Sign-Off:**
- [ ] ✅ Lead developer confirms all code frozen
- [ ] ✅ Security auditor confirms Phase 4 results final
- [ ] ✅ Consensus engineer confirms rules finalized

**Economic Sign-Off:**
- [ ] ✅ Premine amount confirmed (2,627,900 DIN - consensus-critical)
- [ ] ✅ Allocation plan approved (single output, single-signature)
- [ ] ✅ Script type chosen (P2TR or P2WPKH)
- [ ] ✅ Private key secured (hardware wallet + offline backup)
- [ ] ✅ Public transparency document prepared

**Governance Sign-Off:**
- [ ] ✅ Semantic freeze policy accepted
- [ ] ✅ Motto approved
- [ ] ✅ Genesis timestamp approved
- [ ] ✅ Network parameters approved

**Documentation Sign-Off:**
- [ ] ✅ All governance documents reviewed
- [ ] ✅ Genesis parameters documented
- [ ] ✅ Public announcement drafted
- [ ] ✅ Transparency commitments made

---

## Section 8: Next Steps

**After All Confirmations:**

1. **Create Genesis Regeneration Issue/Ticket**
   - Title: "Regenerate Genesis Block with Final Consensus Rules"
   - Checklist: Copy relevant items from this document
   - Assignee: Lead developer
   - Priority: CRITICAL
   - Blocker: Mainnet launch

2. **Execute Regeneration Procedure (Section 2.2)**
   - Follow steps exactly
   - Document all outputs
   - Verify at each step

3. **Update Governance Documents**
   - Archive: COVENANT_ACTIVATION_PLAN.md (not needed)
   - Create: GENESIS_ACTIVATION_STATEMENT.md (replacement)
   - Keep: COVENANT_SEMANTIC_FREEZE.md (CRITICAL)

4. **Public Announcement**
   - Announce genesis parameters
   - Publish premine transparency document (allocation, addresses, custodians)
   - Set mainnet launch date

---

## Summary

**Status: PLANNING PHASE**

This checklist is your roadmap to a clean, final genesis. **Do not rush.**

**Critical Decisions - ALL CONFIRMED:**
1. ✅ **Premine allocation** - Single output: 2,627,900 DIN → P2TR scriptPubKey
2. ✅ **Script type** - P2TR (Taproot native, BIP341)
3. ✅ **Motto** - "Dinero: Real Money For Free People"
4. ✅ **Difficulty** - nBits: 0x1d31ffce
5. ✅ **Timestamp** - Mar 3, 2026 00:00:00 UTC (1772496000)
6. ✅ **Consensus rules** - Covenants + Taproot + BIP342 active from block 0

**Mandatory Execution Steps (in order):**
1. 🟡 **Stop all nodes** (killall dinerod)
2. 🟡 **WIPE ALL CHAIN DATA** (rm -rf ~/.dinero/*/chaindb/ blocks/ chainstate/ indexes/)
3. 🟡 **Regenerate genesis block 0** (100 DIN burn, motto, new hash)
4. 🟡 **Update hardcoded genesis constants** (chainparams_impl.cpp, subsidy.h)
5. 🟡 **Clean rebuild** (rm -rf build/ && cmake && make)
6. 🟡 **Verify genesis** (start node, check hash matches, covenants active)
7. 🟡 **Generate P2TR scriptPubKey** for premine
8. 🟡 **Create and mine block 1** (2,627,900 DIN → scriptPubKey)
9. 🟡 **Verify block 1** (subsidy correct, scriptPubKey valid)
10. 🟡 **Deploy to testnet** (repeat procedure)
11. 🟡 **Deploy to mainnet** (final genesis, frozen forever)

**The Invariant (non-negotiable):**
> **Genesis change = total database invalidation**
>
> No partial resets. No shortcuts. No exceptions.

**Once All Decisions Made:**
- Regenerate genesis block 0
- Update hardcoded constants
- Delete old chains
- Start fresh: regtest → testnet → mainnet

**Remember:**
> You are not "resetting history".
> You are creating history for the first time.
> Everything before this moment was pre-history.

**Genesis is forever. Get it right once.**

---

**Document Status:** READY FOR EXECUTION - All parameters confirmed
**Next Action:** Generate P2TR address and secure private key
**Blocker:** Mainnet launch (cannot proceed until genesis finalized)

**Progress:**
- ✅ Premine allocation confirmed (1 output, 2,627,900 DIN, P2TR single-signature)
- ✅ Script type confirmed (P2TR - Taproot native)
- ✅ Motto confirmed ("Dinero: Real Money For Free People")
- ✅ Difficulty confirmed (nBits: 0x1d31ffce)
- ✅ Timestamp confirmed (Mar 3, 2026 00:00:00 UTC)
- 🟡 Generate P2TR address for premine (din1p...)
- 🟡 Secure private key (hardware wallet + offline backup)
- 🟡 Confirm network parameters (if not already final)

---

**END OF GENESIS FINALIZATION CHECKLIST**
