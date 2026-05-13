# Dinero Wallet Architecture Gap Analysis
**Date:** 2025-12-21
**Critical Issue:** Wallet restore broken - premine and UTXOs not visible after restart

---

## Executive Summary

After studying canonical Bitcoin wallet architecture (BIP32/39/84, Bitcoin Core source), the root cause of Dinero's wallet restore bug is **clear and fixable**:

> **Bitcoin Core Invariant:** A wallet does not own addresses. It owns **scripts, persisted in a database**, and scanned against the chain.

Dinero violates this invariant. The `restoreWallet()` function:
1. ✅ Restores seed correctly (BIP39/32)
2. ✅ Derives HD keys correctly (BIP84)
3. ❌ **Does NOT persist any scriptPubKeys to database**
4. ❌ **Does NOT trigger rescan after restore**
5. ❌ **Returns immediately without registering scripts for scanning**

Result: After wallet restart, **no scripts exist in the database**, so the wallet cannot identify which outputs belong to it. The premine transaction exists on-chain but the wallet doesn't know to claim it.

---

## The Bitcoin Core Model (Gold Standard)

### 1. Key Architecture Principles

From Bitcoin Core wallet documentation:

| Component | Purpose | Persisted? | Derived? |
|-----------|---------|------------|----------|
| **scriptPubKey** | Ownership authority | ✅ YES (database) | ❌ NO |
| **Address** | User-facing representation | ❌ NO | ✅ YES (from script) |
| **Private Key** | Signing authority | ✅ YES (encrypted) | ✅ YES (HD wallets) |

**Critical insight:** Scripts are **first-class citizens**. Addresses are **derived views**.

### 2. Bitcoin Core Restore Flow

```
restorewallet(mnemonic):
  1. Decode mnemonic → seed
  2. Derive master xprv
  3. Persist seed to database (encrypted)
  4. Generate FIRST N scripts (gap limit = 20, keypool = 1000)
  5. Persist scripts to database:
     - Table: "scripts" → scriptPubKey
     - Table: "keys" → private key (encrypted)
     - Table: "keymeta" → derivation path
     - Table: "watchonly" → scripts to scan
  6. Register scripts with chainstate
  7. **Rescan from wallet birthday height**
  8. Return ONLY after DB commit + rescan complete
```

**Atomicity guarantee:** Scripts are persisted BEFORE rescan. Database commit BEFORE RPC returns.

### 3. Bitcoin Core Rescan Logic

From `src/wallet/wallet.cpp::ScanForWalletTransactions()`:

```cpp
// Wallet does NOT scan without persisted scripts
// Wallet does NOT trust returned RPC values
// Wallet loads scripts → registers with chainstate → scans

for (each block from birthday to tip) {
    for (each transaction in block) {
        for (each output in transaction) {
            if (IsMine(output.scriptPubKey)) {
                // scriptPubKey exists in wallet database
                AddToWallet(transaction);
            }
        }
    }
}
```

**IsMine contract:** Returns true if scriptPubKey exists in persisted set. No dynamic generation during scan.

### 4. Bitcoin Core Database Tables

From `src/wallet/walletdb.cpp`:

| Table Key | Value | Purpose |
|-----------|-------|---------|
| `"key" + pubkey` | `(privkey, hash)` | Unencrypted private keys |
| `"ckey" + pubkey` | `encrypted_secret` | Encrypted private keys |
| `"keymeta" + pubkey` | `CKeyMetadata` | Derivation path + birthday |
| `"hdchain"` | `CHDChain` | HD seed + counters |
| `"hdpubkey" + pubkey` | `CHDPubKey` | Extended public key info |
| `"scripts" + scripthash` | `scriptPubKey` | **AUTHORITATIVE ownership** |
| `"watchs" + script` | `'1'` | Watch-only scripts |
| `"tx" + hash` | `CWalletTx` | Wallet transactions |
| `"pool" + index` | `CKeyPool` | Keypool entries |

**Gold Standard:** `"scripts"` table is the **source of truth** for wallet ownership.

---

## Dinero's Current Architecture (Broken)

### 1. HDWalletManager Structure

**File:** `src/daemon/hd_wallet_manager.cpp`

**Storage:** JSON file `wallet.hd` with:
```json
{
  "schema": 1,
  "meta": {
    "coin_type": 1,
    "account": 0,
    "gap_limit": 20,
    "next_index_recv": 0,
    "next_index_change": 0
  },
  "crypto": {
    "cipher": "aes-256-gcm",
    "data": "<base64-encrypted-mnemonic>"
  }
}
```

**What's persisted:**
- ✅ Mnemonic (encrypted)
- ✅ Account indices
- ✅ Coin type

**What's NOT persisted:**
- ❌ No scriptPubKeys
- ❌ No addresses
- ❌ No derivation path mapping
- ❌ No transaction history
- ❌ No UTXO set

### 2. Dinero's Broken Restore Flow

**File:** `src/daemon/hd_wallet_manager.cpp:80-112`

```cpp
bool HDWalletManager::restoreWallet(const std::string& mnemonic,
                                    const std::string& passphrase) {
    // Validate mnemonic
    auto words = dinero::bip39::split(mnemonic);
    if (words.size() != 12 && words.size() != 24) {
        return false;
    }

    mnemonic_ = mnemonic;
    passphrase_ = passphrase;

    // Derive seed
    uint8_t seed[64];
    dinero::bip39::mnemonic_to_seed(mnemonic_, passphrase_, seed);

    // Create master key
    std::vector<uint8_t> seed_vec(seed, seed + 64);
    auto master = crypto::HDKeychain::fromSeed(seed_vec);

    // Derive account key: m/84'/1'/0'
    account_key_ = std::make_unique<crypto::HDKeychain::ExtendedKey>(
        crypto::HDKeychain::getBIP84Account(master, coin_type_, account_)
    );

    // Create address generator
    address_gen_ = std::make_unique<crypto::BIP84AddressGenerator>(*account_key_, "din");

    // Save to JSON file (ONLY saves seed!)
    save();

    printf("✅ Restored HD wallet from mnemonic\n");
    return true;  // ❌ RETURNS WITHOUT PERSISTING SCRIPTS OR RESCANNING
}
```

**Bugs identified:**

1. **No Script Generation:** Restore does NOT call `generateAddress()` to create initial scripts
2. **No Script Persistence:** Even if addresses were generated, no scriptPubKeys saved to database
3. **No Rescan Trigger:** No blockchain scan for historical transactions
4. **Premature Return:** RPC returns before any scripts exist in persistent storage

### 3. Dinero's Address Generation (Also Broken)

**File:** `src/daemon/hd_wallet_manager.cpp:114-128`

```cpp
std::string HDWalletManager::generateAddress(const std::string& label) {
    if (!address_gen_) {
        throw std::runtime_error("Wallet not initialized");
    }

    // Generate next BIP-84 address
    std::string address = address_gen_->getNextAddress();

    // SimpleWallet integration DISABLED (commented out)
    // simple_wallet_->add_address(address, label);  // DISABLED

    save();  // Only saves index counter to JSON

    return address;  // ❌ RETURNS ADDRESS WITHOUT PERSISTING SCRIPT
}
```

**Critical bug:** Address is generated in-memory and returned to caller, but:
- ❌ scriptPubKey is NOT written to any persistent database
- ❌ Wallet relies on caller to "remember" the address
- ❌ After restart, wallet has no record this address was ever generated

### 4. Dinero's Database (Exists But Unused)

**File:** `src/wallet/sqlite_wallet.h`

Dinero **HAS** a proper SQLite wallet schema with all the right tables:

```cpp
class SQLiteWallet {
    // ✅ Has addAddress() - persists scriptPubKey
    bool addAddress(int desc_id, int idx, const std::string& address,
                   const std::vector<uint8_t>& scriptpubkey, const std::string& label = "");

    // ✅ Has addUTXO() - tracks ownership
    bool addUTXO(const std::vector<uint8_t>& outpoint, int64_t value_sats,
                 const std::vector<uint8_t>& scriptpubkey, int desc_id, int idx, int height);

    // ✅ Has getUTXOs() - retrieves owned outputs
    std::vector<WalletUTXO> getUTXOs(int desc_id = -1);
};
```

**But HDWalletManager never uses it!**

The comments show `SimpleWallet` was removed but never replaced with `SQLiteWallet` integration.

---

## Architectural Gap Summary

| Requirement | Bitcoin Core | Dinero Current | Status |
|-------------|--------------|----------------|--------|
| **Script Persistence** | scripts table | ❌ None | 🔴 BROKEN |
| **Script-first model** | YES | ❌ Address-first | 🔴 BROKEN |
| **Restore generates scripts** | YES (gap_limit × 2) | ❌ NO | 🔴 BROKEN |
| **Restore triggers rescan** | YES | ❌ NO | 🔴 BROKEN |
| **Address→Script mapping** | Persisted in DB | ❌ In-memory only | 🔴 BROKEN |
| **UTXO tracking** | Wallet DB | ❌ None | 🔴 BROKEN |
| **IsMine() check** | DB lookup | ❌ No implementation | 🔴 BROKEN |
| **Transaction history** | Wallet DB | ❌ None | 🔴 BROKEN |
| **Database atomicity** | YES | ❌ JSON file only | 🔴 BROKEN |
| **Rescan from birthday** | YES | ❌ Never called | 🔴 BROKEN |

---

## Why The Bug Manifests

### Scenario: Restore Wallet with Premine

```
User: restorewallet "word1 word2 ... word12"

1. Dinero derives seed                     ✅ Works
2. Dinero creates account key              ✅ Works
3. Dinero saves JSON file                  ✅ Works
4. Dinero returns success                  ✅ Returns

User: getbalance

5. Wallet checks for owned UTXOs           ❌ No scripts in DB
6. Wallet cannot identify premine output   ❌ No IsMine() check
7. Returns balance: 0.0                    ❌ WRONG

User: restart daemon

8. Wallet loads JSON file                  ✅ Loads seed
9. Wallet restores account key             ✅ Works
10. Wallet scans for UTXOs                 ❌ No scripts to match against
11. Wallet still shows balance: 0.0        ❌ STILL BROKEN
```

**Root cause:** Wallet has seed but no scripts. It's like having a master key but no locks installed.

---

## The One-Sentence Fix

> **Before returning from restore, generate and persist scripts, then rescan the blockchain.**

---

## Required Changes (High-Level)

### 1. Restore Flow (Critical)

**File:** `src/daemon/hd_wallet_manager.cpp::restoreWallet()`

```cpp
bool HDWalletManager::restoreWallet(const std::string& mnemonic,
                                    const std::string& passphrase) {
    // ... existing seed derivation code ...

    // ✅ NEW: Open wallet database
    if (!wallet_db_) {
        wallet_db_ = std::make_unique<SQLiteWallet>();
        wallet_db_->open(wallet_file_ + ".db");
    }

    // ✅ NEW: Begin atomic transaction
    wallet_db_->beginTransaction();

    // ✅ NEW: Generate initial scripts (gap_limit + lookahead)
    const int gap_limit = 20;
    const int lookahead = 1000;  // Bitcoin Core default keypool

    // External chain (receiving addresses)
    for (int i = 0; i < lookahead; ++i) {
        std::string addr = address_gen_->generateAddress(i, false);
        std::vector<uint8_t> script = deriveScriptPubKey(addr);
        wallet_db_->addAddress(0, i, addr, script, "");
    }

    // Internal chain (change addresses)
    for (int i = 0; i < lookahead; ++i) {
        std::string addr = address_gen_->generateAddress(i, true);
        std::vector<uint8_t> script = deriveScriptPubKey(addr);
        wallet_db_->addAddress(1, i, addr, script, "");
    }

    // ✅ NEW: Commit scripts to database
    wallet_db_->commitTransaction();

    // Save HD metadata
    save();

    // ✅ NEW: Trigger blockchain rescan
    rescanBlockchain(0);  // Scan from genesis

    printf("✅ Restored HD wallet with %d scripts, rescanning blockchain...\n", lookahead * 2);
    return true;
}
```

### 2. Script Derivation Helper

```cpp
std::vector<uint8_t> deriveScriptPubKey(const std::string& bech32_address) {
    // Decode Bech32 address
    auto decoded = bech32::decode(bech32_address);

    // For P2WPKH: OP_0 <20-byte-keyhash>
    std::vector<uint8_t> script;
    script.push_back(0x00);  // OP_0 (witness version 0)
    script.push_back(0x14);  // Push 20 bytes
    script.insert(script.end(), decoded.data.begin(), decoded.data.end());

    return script;
}
```

### 3. Rescan Integration

```cpp
void HDWalletManager::rescanBlockchain(uint32_t start_height) {
    // Call existing blockchain scanner
    // This should already exist in Dinero's codebase

    auto* blockchain = getBlockchainInterface();

    for (uint32_t height = start_height; height <= blockchain->getHeight(); ++height) {
        auto block = blockchain->getBlock(height);

        for (const auto& tx : block.transactions) {
            for (size_t vout = 0; vout < tx.outputs.size(); ++vout) {
                if (isMine(tx.outputs[vout].scriptPubKey)) {
                    wallet_db_->addUTXO(
                        tx.hash + ":" + std::to_string(vout),
                        tx.outputs[vout].value,
                        tx.outputs[vout].scriptPubKey,
                        0,  // descriptor_id
                        findScriptIndex(tx.outputs[vout].scriptPubKey),
                        height
                    );
                }
            }
        }
    }
}

bool HDWalletManager::isMine(const std::vector<uint8_t>& scriptPubKey) {
    // Check if scriptPubKey exists in wallet database
    auto addresses = wallet_db_->getAllAddresses();
    for (const auto& addr : addresses) {
        if (addr.scriptpubkey == scriptPubKey) {
            return true;
        }
    }
    return false;
}
```

### 4. Integration with SQLiteWallet

**Replace all commented-out SimpleWallet calls with SQLiteWallet:**

```cpp
class HDWalletManager {
private:
    std::unique_ptr<SQLiteWallet> wallet_db_;  // ✅ Add this
    // Remove: std::unique_ptr<SimpleWallet> simple_wallet_;
};
```

---

## Testing Plan

### Regression Test (Must Pass)

```bash
#!/bin/bash

# Test: restore → restart → verify premine visibility

echo "1. Creating new wallet..."
MNEMONIC=$(./dinerod createwallet test_wallet 12)

echo "2. Getting premine address..."
PREMINE_ADDR=$(./dinero-cli getnewaddress)

echo "3. Mining genesis with premine to address..."
./dinero-cli generatetoaddress 1 $PREMINE_ADDR

echo "4. Verifying balance (should show premine)..."
BALANCE1=$(./dinero-cli getbalance)
echo "Balance before restart: $BALANCE1"

echo "5. Stopping daemon..."
./dinero-cli stop

echo "6. Starting daemon..."
./dinerod -daemon

echo "7. Verifying balance after restart (should STILL show premine)..."
BALANCE2=$(./dinero-cli getbalance)
echo "Balance after restart: $BALANCE2"

if [ "$BALANCE1" != "$BALANCE2" ]; then
    echo "❌ FAIL: Balance changed after restart!"
    echo "Before: $BALANCE1, After: $BALANCE2"
    exit 1
fi

echo "✅ PASS: Balance persists after restart"
```

---

## Implementation Priority

1. **CRITICAL (Do First):**
   - Add `wallet_db_` member to HDWalletManager
   - Implement script generation in `restoreWallet()`
   - Implement script persistence to SQLiteWallet
   - Add `isMine()` implementation

2. **HIGH (Do Second):**
   - Implement `rescanBlockchain()`
   - Add UTXO tracking during rescan
   - Test restore → restart flow

3. **MEDIUM (Do Third):**
   - Add gap limit enforcement
   - Implement keypool refill
   - Add transaction history tracking

4. **LOW (Polish):**
   - Progress indicators during rescan
   - Wallet backup/export
   - Migration from old JSON format

---

## Estimated Complexity

- **Core Fix:** ~200 lines of code
- **Testing:** 1-2 hours
- **Total Time:** 4-6 hours for experienced developer

This is NOT a complex refactor. The infrastructure (SQLiteWallet) already exists. We just need to wire it up.

---

## References

1. **BIP32** - HD Wallets: https://github.com/bitcoin/bips/blob/master/bip-0032.mediawiki
2. **BIP39** - Mnemonic Seeds: https://github.com/bitcoin/bips/blob/master/bip-0039.mediawiki
3. **BIP84** - Native Segwit: https://github.com/bitcoin/bips/blob/master/bip-0084.mediawiki
4. **Bitcoin Core** - wallet.md: https://github.com/bitcoin/bitcoin/blob/master/doc/wallet.md
5. **Bitcoin Core** - wallet.cpp: https://github.com/bitcoin/bitcoin/blob/master/src/wallet/wallet.cpp

---

## Conclusion

The fix is **clear, well-defined, and achievable**. Dinero's wallet architecture is actually well-designed (SQLiteWallet is good), but HDWalletManager is bypassing it entirely.

By following Bitcoin Core's invariant - **scripts first, addresses second** - the restore bug will be completely eliminated.
