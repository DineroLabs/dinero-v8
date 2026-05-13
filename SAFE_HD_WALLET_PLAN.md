# 🔐 Safe HD Wallet Implementation Plan

**Goal:** Add production-grade HD wallet WITHOUT breaking existing functionality

**Status:** Mining running, 2,300+ DIN safe in Simple Wallet

---

## ✅ **Safety Guarantees:**

1. **Simple Wallet stays intact** - Your existing `wallet.json` untouched
2. **Mining continues** - No interruption to block production
3. **Backward compatible** - Can use both wallet types
4. **Gradual rollout** - Test before switching
5. **Migration tool** - Safe transfer of coins

---

## 📊 **Implementation Steps:**

### **Step 1: Backend - HD Wallet Core** ⏳
**File:** `src/daemon/hd_wallet_backend.cpp/.h`
- [ ] Create `HDWalletBackend` class (separate from Simple Wallet)
- [ ] Integrate with existing `crypto/bip39.cpp`
- [ ] Integrate with existing `crypto/hd_keychain.cpp`
- [ ] Add wallet encryption (Argon2id + AES-GCM)
- [ ] Save to separate file: `hd_wallet.db` (not `wallet.json`)

**Why Safe:** New files, doesn't touch existing wallet code

---

### **Step 2: RPC Methods** ⏳
**File:** `src/daemon/rpc_handler.cpp`
- [ ] `createhdwallet(seed_words, passphrase, password)` → encrypted HD wallet
- [ ] `restorehdwallet(mnemonic, passphrase, password)` → restore from seed
- [ ] `gethdaddress(account, change, index)` → derive address
- [ ] `gethdbalance()` → check HD wallet balance
- [ ] `hdwalletlock()` / `hdwalletunlock(password, timeout)` → lock/unlock

**Why Safe:** New RPC methods, existing RPCs (`getnewaddress`, etc.) still work

---

### **Step 3: GUI Integration** ⏳
**Files:** `gui/src/walletwizard.cpp`
- [ ] Wire wizard to `createhdwallet` RPC
- [ ] Wire wizard to `restorehdwallet` RPC  
- [ ] Update completion page to show real fingerprint
- [ ] Add "Use HD Wallet" toggle in settings

**Why Safe:** Wizard is optional, doesn't affect current address generation

---

### **Step 4: Testing** ⏳
- [ ] Create test HD wallet
- [ ] Generate 10 addresses
- [ ] Send test transaction (0.01 DIN)
- [ ] Restore from seed phrase
- [ ] Verify addresses match
- [ ] Test encryption/decryption

**Why Safe:** Test with small amounts first

---

### **Step 5: Migration Tool** ⏳
**File:** `tools/migrate_to_hd.cpp`
- [ ] Read existing `wallet.json`
- [ ] Extract all private keys
- [ ] Create new HD wallet
- [ ] Send all coins to first HD address
- [ ] Backup old wallet
- [ ] Switch to HD wallet

**Why Safe:** User-initiated, manual process, backs up old wallet

---

### **Step 6: Production Rollout** ⏳
- [ ] Update GUI to prefer HD wallet by default
- [ ] Show migration prompt for Simple Wallet users
- [ ] Keep Simple Wallet as fallback option
- [ ] Document migration process

**Why Safe:** Gradual adoption, both wallets work

---

## 🔧 **Technical Architecture:**

```
Current (Simple Wallet):
data-main/wallet/wallet.json → Simple addresses (still works!)

New (HD Wallet):
data-main/wallet/hd_wallet.db → Encrypted HD wallet (BIP-39/32/84)

Both can coexist!
```

---

## 📅 **Timeline:**

**Today (Next 4-6 hours):**
- ✅ Step 1: HD wallet backend core
- ✅ Step 2: RPC methods
- ✅ Step 3: GUI integration

**Tomorrow:**
- ✅ Step 4: Testing
- ✅ Step 5: Migration tool
- ✅ Step 6: Documentation

**Result:** Production-ready HD wallet in ~1-2 days

---

## 🛡️ **Rollback Plan:**

If anything goes wrong:
1. HD wallet files are separate
2. Simple Wallet still works
3. Just don't use HD features
4. Delete `hd_wallet.db` and continue with Simple Wallet

**Your coins are ALWAYS safe!**

---

## ✅ **Current Status:**

- ✅ Mining: **ACTIVE** (don't stop!)
- ✅ Balance: **2,300+ DIN**
- ✅ Simple Wallet: **WORKING**
- ⏳ HD Wallet: **BUILDING NOW**

**Mining can continue while I build this!** 🚀

