# 🧱 DineroCoin Wallet Subsystem - Implementation Roadmap

**Last Updated:** November 1, 2025  
**Status:** ~85% Complete - Core Wallet Production-Ready ✅

---

## ✅ VERIFIED COMPLETE FEATURES

### 🔐 Wallet Security & Creation

| Feature | Status | File | Notes |
|---------|--------|------|-------|
| **createhdwallet** | ✅ Complete | `src/daemon/main.cpp:2988` | BIP-39 mnemonic (12/24 words) |
| **restorewallet** | ✅ Complete | `src/daemon/main.cpp:3059` | BIP-39 restore with passphrase |
| **encryptwallet** | ✅ Complete | `src/wallet/hd_wallet.cpp:1206` | AES-256-GCM + PBKDF2-HMAC-SHA512 |
| **walletunlock** | ✅ Complete | `src/daemon/main.cpp:3130` | Password verification + unlock |
| **walletlock** | ✅ Complete | `src/daemon/main.cpp` | Manual lock command |
| **walletpassphrasechange** | ✅ Complete | `src/daemon/main.cpp:3361` | Change encryption password |

### 💰 Transaction & Balance

| Feature | Status | File | Notes |
|---------|--------|------|-------|
| **getbalance** | ✅ Complete | `src/daemon/main.cpp` | Real balance from UTXO tracking |
| **sendtoaddress** | ✅ Complete | `src/daemon/main.cpp:3686` | Create and sign transactions |
| **Change address logic** | ✅ **VERIFIED** | `src/wallet/hd_wallet.cpp:900` | Uses `DeriveNextChangeAddress()` |
| **listtransactions** | ✅ Complete | `src/daemon/main.cpp:2937` | Transaction history |
| **listunspent** | ✅ Complete | `src/daemon/main.cpp:3599` | Real UTXO data |

### 🔑 HD Derivation

| Feature | Status | File | Notes |
|---------|--------|------|-------|
| **Receive addresses** | ✅ Complete | `src/wallet/hd_wallet.cpp` | `m/84'/1447'/0'/0/index` |
| **Change addresses** | ✅ Complete | `src/wallet/hd_wallet.cpp:292` | `m/84'/1447'/0'/1/index` |
| **Mining addresses** | ✅ Complete | `src/wallet/hd_wallet.cpp` | `m/84'/1447'/0'/2/index` |
| **Index persistence** | ✅ Complete | `src/wallet/hd_wallet.cpp` | Stored in wallet.conf |

---

## ⚠️ CRITICAL MISSING FEATURES

### 🔥 Priority 1: Auto-Lock Timeout (SECURITY)

**Status:** ❌ Not Implemented  
**Why Critical:** Wallet stays unlocked until manual lock or daemon restart

**Implementation Plan:**

1. **Add timer thread in `HDWallet` class**
   - File: `src/wallet/hd_wallet.cpp` + `include/wallet/hd_wallet.h`
   - Add `std::thread autolock_thread_`
   - Add `std::atomic<bool> autolock_running_`
   - Add `std::atomic<int64_t> unlock_time_`
   - Add `int autolock_seconds_` (default: 900 = 15 min)

2. **Reset timer on RPC activity**
   - In `walletunlock` handler: `ResetAutoLockTimer()`
   - In `sendtoaddress`, `getnewaddress`, etc.: `ResetAutoLockTimer()`

3. **Auto-lock thread**
   ```cpp
   void HDWallet::AutoLockThread() {
       while (autolock_running_) {
           std::this_thread::sleep_for(std::chrono::seconds(10));
           if (IsUnlocked() && (std::time(nullptr) - unlock_time_ > autolock_seconds_)) {
               Lock();
               std::cout << "🔒 Wallet auto-locked after " << autolock_seconds_ << " seconds" << std::endl;
           }
       }
   }
   ```

4. **Config option**
   - Add `wallet_autolock_secs` to config (default: 900)

**Estimated Effort:** 2-3 hours  
**Files to Modify:**
- `include/wallet/hd_wallet.h` - Add timer member variables
- `src/wallet/hd_wallet.cpp` - Implement auto-lock thread
- `src/daemon/main.cpp` - Reset timer on RPC calls

---

### ⚡ Priority 2: Fee Estimation (UX)

**Status:** ❌ Not Implemented  
**Why Important:** Users must manually specify fees, leading to stuck transactions

**Implementation Plan:**

1. **Add `estimatefee` RPC handler**
   - File: `src/daemon/main.cpp`
   - Calculate median fee rate from last 10 blocks
   - Return fee rate per vbyte (una)

2. **Fee calculation logic**
   ```cpp
   uint64_t EstimateFeeRate(uint32_t blocks = 10) {
       // Get last N blocks from ChainDB
       // Extract fee rate from each block
       // Calculate median fee rate
       // Return una per vbyte
   }
   ```

3. **RPC Handler**
   ```cpp
   rpc_server->register_method("estimatefee", [](const Json::Value& params) {
       Json::Value result;
       uint32_t blocks = 10;
       if (!params.empty() && params[0].isInt()) {
           blocks = params[0].asUInt();
       }
       
       uint64_t fee_rate = EstimateFeeRate(blocks);
       result["fee_rate"] = static_cast<int>(fee_rate);
       result["fee_per_kb"] = fee_rate * 1000;
       result["blocks"] = blocks;
       return result;
   });
   ```

**Estimated Effort:** 2-3 hours  
**Files to Modify:**
- `src/daemon/main.cpp` - Add estimatefee RPC handler
- `src/core/blockchain/` - Add fee rate extraction from blocks

---

### ⚡ Priority 3: Wallet Backup File Export

**Status:** ⚠️ Partial (mnemonic only)  
**Current:** `backupwallet` only returns mnemonic  
**Needed:** Full wallet.dat export

**Implementation Plan:**

1. **Enhance `backupwallet` RPC**
   - File: `src/daemon/main.cpp:3325`
   - Add optional filepath parameter
   - Export wallet.conf + metadata
   - Create backup.tar.gz with wallet files

2. **Backup format**
   ```
   backup.tar.gz
   ├── wallet.conf (encrypted if wallet is encrypted)
   ├── wallet_metadata.json (fingerprint, addresses, etc.)
   └── README.txt (restore instructions)
   ```

**Estimated Effort:** 2-3 hours

---

### ⚡ Priority 4: Wallet Rescan

**Status:** ❌ Not Implemented  
**Why Important:** If wallet is restored, UTXOs may be missing

**Implementation Plan:**

1. **Add `walletrescan` RPC handler**
   - File: `src/daemon/main.cpp`
   - Scan blockchain from genesis or restore height
   - Rebuild `wallet_utxos` table
   - Register all addresses in UTXOIndex

2. **Rescan logic**
   ```cpp
   bool WalletRescan(uint32_t start_height = 0) {
       // Get all addresses from wallet
       // For each block from start_height:
       //   - Check if any outputs belong to wallet
       //   - Add to wallet_utxos table
       //   - Update balance
   }
   ```

**Estimated Effort:** 3-4 hours

---

## 🧩 OPTIONAL FEATURES (Post-Mainnet)

### PSBT & Multisig
- `walletcreatefundedpsbt` - Create PSBT for hardware wallets
- `walletprocesspsbt` - Sign PSBT
- `walletsend` - Send from PSBT

### Advanced Address Management
- `getaddressesbylabel` - List addresses by label
- `listaddressgroupings` - Group addresses by account
- `importxpub` - Watch-only wallets

---

## 📊 IMPLEMENTATION PRIORITY

### Phase A: Critical Security (Before Mainnet) 🔥

1. **Auto-lock timeout** (2-3 hours) ⚠️ **START HERE**
   - Timer thread in `HDWallet`
   - Reset on RPC activity
   - Config option

### Phase B: UX Improvements (Before Mainnet) ⚡

2. **Fee estimation** (2-3 hours)
   - `estimatefee` RPC handler
   - Median calculation from recent blocks

3. **Wallet backup file** (2-3 hours)
   - Export full wallet.dat
   - Verify restore works

### Phase C: Post-Mainnet ⚙️

4. **Wallet rescan** (3-4 hours)
5. **PSBT support** (optional)
6. **Advanced address management** (optional)

---

## ✅ VERIFICATION CHECKLIST

- [x] Wallet encryption working (AES-256-GCM)
- [x] Wallet unlock/lock working
- [x] BIP-39 restore working
- [x] Address derivation working (receive + change + mining)
- [x] Change address logic verified ✅ **USES DeriveNextChangeAddress()**
- [x] Transaction sending working
- [x] Balance query working
- [x] UTXO listing working
- [ ] Auto-lock timeout implemented ❌
- [ ] Fee estimation implemented ❌
- [ ] Wallet backup file export working ⚠️

---

## 🚀 NEXT STEPS

**Immediate Actions:**
1. ✅ Verify change address logic - **CONFIRMED WORKING**
2. 🔥 Implement auto-lock timeout (Critical for security)
3. ⚡ Add fee estimation (Better UX)

**Current Status:** ~85% complete - Core wallet functionality is production-ready. Critical security feature (auto-lock) needed before mainnet.

---

## 📝 Implementation Notes

### Change Address Logic ✅ VERIFIED

**Location:** `src/wallet/hd_wallet.cpp:898-906`

```cpp
// Add change output if needed (using BIP84 change chain)
if (coin_result.change_amount > dinero::CoinSelector::DUST_THRESHOLD) {
  std::string change_addr = DeriveNextChangeAddress();  // ✅ CORRECT
  auto change_script = AddressToScriptPubKey(change_addr);
  dinero::TxOutput change_output(coin_result.change_amount, change_script);
  tx.vout.push_back(change_output);
}
```

**Status:** ✅ **CORRECTLY IMPLEMENTED** - Change addresses use `DeriveNextChangeAddress()` from change chain.

---

## 🎯 Mainnet Readiness

**Blockers:**
- ❌ Auto-lock timeout (security risk if wallet left unlocked)

**Recommended:**
- ⚡ Fee estimation (better UX)
- ⚡ Wallet backup file (user convenience)

**Optional:**
- Wallet rescan (nice to have)
- PSBT support (future feature)

**Current Assessment:** **~85% Mainnet Ready** - Core wallet is production-quality. Auto-lock is the only critical blocker.

