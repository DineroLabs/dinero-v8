# HD Wallet Private Key Derivation - Implementation Plan

**Goal:** Replace INSECURE test key derivation with proper BIP32/BIP44 HD wallet
**Timeline:** 2-3 days
**Priority:** 🔴 CRITICAL SECURITY

---

## 📋 TASK BREAKDOWN

### Phase 1: Database Schema (4 hours)
- [ ] Add `hd_seeds` table to wallet database
- [ ] Add `address_derivation_paths` table
- [ ] Store encrypted seed with salt
- [ ] Track derivation path for each address
- [ ] Migration script for existing wallets

**Files to modify:**
- `src/wallet/wallet_manager.cpp` - Database migration
- `include/wallet/wallet_manager.h` - New methods

---

### Phase 2: Key Derivation Implementation (8 hours)
- [ ] Implement BIP32 derivation from seed
- [ ] Implement BIP44 path generation (m/84'/coin'/account'/change/index)
- [ ] Derive private keys from master seed + path
- [ ] Derive public keys and addresses
- [ ] Cache derived keys in memory (encrypted)

**Files to create/modify:**
- `src/wallet/bip32_derivation.cpp` (NEW)
- `include/wallet/bip32_derivation.h` (NEW)
- `src/wallet/wallet_manager.cpp` - Integration

**External Dependencies:**
- `crypto/secp256k1` (already available)
- `crypto/sha256` (already available)
- `crypto/ripemd160` (already available)

---

### Phase 3: WalletManager Integration (6 hours)
- [ ] Implement `WalletManager::getPrivateKeyForAddress(address)`
- [ ] Lookup derivation path from database
- [ ] Decrypt master seed
- [ ] Derive key from seed + path
- [ ] Return private key bytes
- [ ] Add key caching for performance

**Files to modify:**
- `src/wallet/wallet_manager.cpp`
- `include/wallet/wallet_manager.h`

**New Methods:**
```cpp
class WalletManager {
public:
    // Get private key for spending
    std::optional<std::vector<uint8_t>> getPrivateKeyForAddress(const std::string& address);

private:
    // HD wallet internals
    bool storeMasterSeed(const std::vector<uint8_t>& seed, const std::string& passphrase);
    std::optional<std::vector<uint8_t>> loadMasterSeed(const std::string& passphrase);
    std::optional<std::string> getDerivationPath(const std::string& address);
    void cachePrivateKey(const std::string& address, const std::vector<uint8_t>& key);

    // Key cache (encrypted in memory)
    std::map<std::string, std::vector<uint8_t>> private_key_cache_;
};
```

---

### Phase 4: PSBT Signing Integration (4 hours)
- [ ] Replace INSECURE key derivation in `spend_rpc_handlers.cpp:336-345`
- [ ] Call `WalletManager::getPrivateKeyForAddress()`
- [ ] Handle errors (address not in wallet, wallet locked, etc.)
- [ ] Add proper error messages
- [ ] Test with real transactions

**Files to modify:**
- `src/daemon/rpc/spend_rpc_handlers.cpp:336-345`

**Before:**
```cpp
// INSECURE TEST CODE
std::string seed = "INSECURE_TEST_SEED_" + utxo.address;
private_key.resize(32);
std::copy_n(seed.begin(), std::min(seed.length(), size_t(32)), private_key.begin());
```

**After:**
```cpp
// Get private key from HD wallet
auto private_key_opt = wallet_manager->getPrivateKeyForAddress(utxo.address);
if (!private_key_opt) {
    Json::Value error;
    error["txid"] = utxo.txid;
    error["vout"] = utxo.vout;
    error["error"] = "Private key not available for address";
    errors.append(error);
    all_signed = false;
    continue;
}
std::vector<uint8_t> private_key = *private_key_opt;
```

---

### Phase 5: Wallet Creation/Import (6 hours)
- [ ] Update `createhdwallet` to generate BIP39 mnemonic
- [ ] Store encrypted seed in database
- [ ] Create initial receive address (m/84'/coin'/0'/0/0)
- [ ] Implement `importhdwallet` with mnemonic
- [ ] Implement wallet backup (export mnemonic)

**Files to modify:**
- `src/daemon/rpc/wallet_basic_handlers.cpp`
- `src/wallet/wallet_manager.cpp`

---

### Phase 6: Address Derivation Tracking (4 hours)
- [ ] Track derivation path when creating addresses
- [ ] Store in `address_derivation_paths` table
- [ ] Update `getNewAddress()` to use HD derivation
- [ ] Update `getNewChangeAddress()` to use HD derivation
- [ ] Ensure gap limit compliance (BIP44)

**Files to modify:**
- `src/wallet/wallet_manager.cpp:1963-2010` (getNewChangeAddress)
- `src/wallet/wallet_manager.cpp` (getNewAddress)

---

### Phase 7: Testing & Validation (8 hours)
- [ ] Unit tests for BIP32 derivation
- [ ] Test vector validation (BIP32 test vectors)
- [ ] Integration test: create wallet → derive address → sign transaction
- [ ] Test wallet recovery from mnemonic
- [ ] Test key caching performance
- [ ] Security review of key storage

**Test Files to Create:**
- `tests/test_bip32_derivation.cpp`
- `tests/test_hd_wallet_signing.cpp`
- `tests/test_wallet_recovery.cpp`

---

## 🗄️ DATABASE SCHEMA CHANGES

### New Table: `hd_seeds`
```sql
CREATE TABLE IF NOT EXISTS hd_seeds (
    wallet_id INTEGER PRIMARY KEY,
    encrypted_seed BLOB NOT NULL,           -- Encrypted master seed
    salt BLOB NOT NULL,                     -- Salt for encryption
    coin_type INTEGER NOT NULL,             -- BIP44 coin type
    created_at INTEGER NOT NULL,
    FOREIGN KEY (wallet_id) REFERENCES wallets(id) ON DELETE CASCADE
);
```

### New Table: `address_derivation_paths`
```sql
CREATE TABLE IF NOT EXISTS address_derivation_paths (
    address TEXT PRIMARY KEY,
    wallet_id INTEGER NOT NULL,
    derivation_path TEXT NOT NULL,  -- e.g., "m/84'/0'/0'/0/0"
    account INTEGER DEFAULT 0,
    change INTEGER DEFAULT 0,       -- 0 = receive, 1 = change
    address_index INTEGER NOT NULL,
    created_at INTEGER NOT NULL,
    FOREIGN KEY (wallet_id) REFERENCES wallets(id) ON DELETE CASCADE
);

CREATE INDEX IF NOT EXISTS idx_derivation_wallet ON address_derivation_paths(wallet_id);
CREATE INDEX IF NOT EXISTS idx_derivation_path ON address_derivation_paths(derivation_path);
```

---

## 🔐 ENCRYPTION STRATEGY

### Master Seed Storage
1. User provides passphrase when creating/unlocking wallet
2. Derive encryption key from passphrase using PBKDF2 (100,000 iterations)
3. Generate random salt (32 bytes)
4. Encrypt master seed with AES-256-GCM
5. Store encrypted seed + salt in database

### Key Caching
1. When wallet unlocked, decrypt master seed
2. Keep decrypted seed in memory (process memory only)
3. Derive keys on-demand from seed + derivation path
4. Cache derived private keys in encrypted memory map
5. Clear cache when wallet locked

---

## 🧪 BIP32 DERIVATION ALGORITHM

```
Master Seed (512 bits)
    ↓ HMAC-SHA512("Bitcoin seed")
Master Private Key (256 bits) + Chain Code (256 bits)
    ↓ BIP32 Child Key Derivation (hardened for account/coin)
m/84'/0'/0'  (Account Master)
    ↓ BIP32 Child Key Derivation (normal)
m/84'/0'/0'/0  (Receive Chain)
    ↓ BIP32 Child Key Derivation (normal)
m/84'/0'/0'/0/0  (First Receive Address)
```

**Key Properties:**
- Hardened derivation (') prevents parent key compromise from child
- Normal derivation allows public key derivation without private key
- Each address has unique private key
- Master seed can recover all keys

---

## 📦 DEPENDENCIES

### Already Available:
- ✅ `crypto/secp256k1` - ECDSA signing
- ✅ `crypto/sha256` - Hash function
- ✅ `crypto/ripemd160` - Hash function
- ✅ `crypto/hmac_sha512` - Key derivation
- ✅ SQLite database

### May Need to Add:
- BIP39 mnemonic wordlist (optional, for wallet creation)
- PBKDF2 implementation (for passphrase → key derivation)
- AES-256-GCM encryption (for seed storage)

---

## 🎯 DELEGATION TO ASSISTANT

If I had an assistant, I would assign them:

### Assistant Task 1: Database Migration (4 hours)
**Priority:** High
**Files:** `src/wallet/wallet_manager.cpp`

**Instructions:**
1. Add `hd_seeds` table schema to migration function
2. Add `address_derivation_paths` table schema
3. Create indexes for performance
4. Test migration on existing wallet database
5. Ensure backward compatibility (don't break existing wallets)

**Deliverable:** Pull request with database schema changes tested

---

### Assistant Task 2: BIP32 Implementation (8 hours)
**Priority:** Critical
**Files:** Create `src/wallet/bip32_derivation.cpp` + header

**Instructions:**
1. Implement BIP32 child key derivation (hardened + normal)
2. Use existing HMAC-SHA512 from crypto library
3. Follow BIP32 spec exactly: https://github.com/bitcoin/bips/blob/master/bip-0032.mediawiki
4. Add test vectors from BIP32 spec
5. Ensure constant-time operations (security)

**Deliverable:** Fully tested BIP32 implementation with test vectors passing

---

### Assistant Task 3: Unit Tests (6 hours)
**Priority:** High
**Files:** Create `tests/test_bip32_derivation.cpp`, `tests/test_hd_wallet_signing.cpp`

**Instructions:**
1. Test BIP32 derivation with official test vectors
2. Test key derivation from seed → address
3. Test transaction signing with HD keys
4. Test wallet recovery from mnemonic
5. Test edge cases (locked wallet, missing address, etc.)

**Deliverable:** Comprehensive test suite with >90% coverage

---

## ⏱️ TIME ESTIMATE BREAKDOWN

| Phase | Task | Hours | Who |
|-------|------|-------|-----|
| 1 | Database schema | 4 | Assistant |
| 2 | BIP32 implementation | 8 | Assistant |
| 3 | WalletManager integration | 6 | Me |
| 4 | PSBT signing integration | 4 | Me |
| 5 | Wallet creation/import | 6 | Me |
| 6 | Address derivation tracking | 4 | Me |
| 7 | Testing & validation | 8 | Assistant |
| **TOTAL** | | **40 hours** | **~5 days (8h/day)** |

**With assistant doing parallel work:** 3 days
**Solo:** 5 days

---

## 🚀 GETTING STARTED

1. ✅ Document current limitations (DONE - see CURRENT_LIMITATIONS.md)
2. 🔨 Start with database schema (Phase 1)
3. 🔨 Implement BIP32 core (Phase 2)
4. 🔨 Integrate with WalletManager (Phase 3)
5. 🔨 Replace insecure signing code (Phase 4)
6. ✅ Test thoroughly (Phase 7)

Let's begin!
