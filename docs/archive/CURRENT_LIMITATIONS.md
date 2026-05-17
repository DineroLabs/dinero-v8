# DineroCoin Daemon - Current Limitations & Known Issues

**Last Updated:** 2025-10-19
**Status:** Development / Testing Phase
**⚠️ DO NOT USE FOR REAL FUNDS ⚠️**

---

## 🔴 CRITICAL SECURITY LIMITATIONS

### ✅ 1. **SECURE HD Wallet Private Key Derivation** ✅
**Files:**
- `include/crypto/hd_keychain.h` - BIP32/BIP84 HD keychain implementation
- `src/crypto/hd_keychain.cpp` - Full BIP32 key derivation
- `src/wallet/wallet_manager.cpp:2613-2694` - HD key integration

**Status:** ✅ **COMPLETE** - Proper BIP32/BIP44 HD wallet implemented!

**Implementation Details:**
- ✅ BIP32 hierarchical deterministic key derivation
- ✅ BIP84 (P2WPKH) address derivation paths
- ✅ Master seed encrypted with PBKDF2 + AES-256-GCM
- ✅ Derivation paths tracked in database (`address_derivation_paths` table)
- ✅ Private keys cached in memory (encrypted)
- ✅ Secure key clearance on wallet lock
- ✅ INSECURE test code removed from codebase

**Security Features:**
- Master seed stored encrypted with user passphrase
- PBKDF2 with 100,000 iterations for key derivation
- AES-256-GCM for seed encryption
- Private key cache cleared on lock
- Full BIP32 hardened derivation support

---

## 🟡 HIGH PRIORITY ISSUES

### 2. **Missing Transaction Parser**
**File:** `src/daemon/rpc/spend_rpc_handlers.cpp:228`

**Issue:** Cannot parse raw hex transactions
```cpp
// TODO: Implement ParseFromHex method
```

**Impact:** `sendrawtransaction` RPC doesn't work with raw hex
**Workaround:** Use PSBT format instead
**Status:** ⏳ PENDING

### 3. **Incomplete Mempool Validation**
**Files:**
- `src/daemon/mempool.cpp:262` - Dependency tracking
- `src/daemon/mempool.cpp:458` - UTXO input lookups
- `src/daemon/mempool.cpp:474` - Dependency updates

**Issue:** Mempool accepts transactions without full validation

**Impact:**
- Invalid transactions may enter mempool
- Double-spend detection incomplete
- Chain reorganization handling incomplete

**Status:** ⏳ PENDING

### 4. **No WebSocket Notifications**
**Files:**
- `src/daemon/block_acceptor.cpp:940` - New block notifications
- `src/daemon/miner/miner.cpp:41` - Block found notifications
- `src/daemon/ws/ws_session.cpp:85` - Auth headers

**Impact:** Miners and clients must poll for updates
**Status:** ⏳ PENDING

---

## 🟢 MEDIUM PRIORITY ISSUES

### 5. **Wallet Encryption Stubbed**
**File:** `src/wallet/descriptor_wallet.cpp:402-446`

**Issue:** Wallet encryption not implemented
```cpp
// TODO: Implement actual encryption
// TODO: Verify passphrase and decrypt keys
// TODO: Implement auto-lock timer
```

**Impact:** Wallets stored in plaintext
**Status:** ⏳ PENDING

### 6. **HD Wallet Incomplete**
**Files:**
- `src/wallet/hd_wallet.cpp:496, 537` - Confirmation calculations
- `src/wallet/hd_wallet.cpp:572` - Coin selection
- `src/wallet/sqlite_wallet.cpp:1447, 1522` - HD interface updates

**Impact:** Some HD wallet features may not work
**Status:** ⏳ PENDING

### 7. **Hardware Wallet Support Missing**
**Files:**
- `src/wallet/ledger_wallet.cpp:479` - Ledger APDU stub
- `src/wallet/trezor_wallet.cpp:505` - Trezor communication stub

**Impact:** Cannot use Ledger/Trezor devices
**Status:** ❌ NOT PLANNED (low priority)

---

## 🔵 LOW PRIORITY / NICE TO HAVE

### 8. **Reorg Wallet Notifications**
**File:** `src/daemon/block_acceptor.cpp:1297`
```cpp
// TODO: Notify wallet of reorg when wallet reorg handling is implemented
```

**Impact:** Wallet may show incorrect balance after chain reorg
**Status:** ⏳ PENDING

### 9. **P2P Connection Simulation**
**File:** `src/daemon/simple_p2p.cpp:186-187`

**Issue:** P2P connections simulated, not real TCP
**Impact:** Limited network connectivity
**Status:** ⏳ PENDING

### 10. **Transaction History Incomplete**
**File:** `src/wallet/descriptor_wallet.cpp:348`

**Impact:** Some wallet transaction queries may not work
**Status:** ⏳ PENDING

---

## ✅ RECENTLY FIXED / IMPLEMENTED

### ✅ Server-Side UTXO Calculations
**File:** `src/daemon/rpc/wallet_stage3_handlers.cpp:210-243`
**Fixed:** 2025-10-19
- Real UTXO amounts (was hardcoded to 0)
- Server-side maturity calculations
- Coinbase maturity remaining
- Verified status

### ✅ Multiple Transaction Outputs
**File:** `src/daemon/rpc/spend_rpc_handlers.cpp:669-684`
**Fixed:** 2025-10-19
- Change address generation
- Proper BIP84 change derivation
- Dust threshold handling (546 sats)

### ✅ Change Address Support
**File:** `src/wallet/wallet_manager.cpp:1963-2010`
**Fixed:** 2025-10-19
- BIP44 change chain support (m/84'/coin'/0'/1/index)
- Separate index counter for change addresses

### ✅ Complete Block Hash Calculation
**Files:** `src/daemon/blockchain.cpp:1004-1017, 1250-1267`
**Fixed:** 2025-10-19
- Block hash now includes full 80-byte header (was only 20 bytes)
- Added prev_hash (32 bytes) to hash calculation using `util::HexToBytes()`
- Added merkle_root (32 bytes) to hash calculation
- Blocks now properly linked via previous hash
- Transactions now cryptographically bound via merkle root
- Fixed in both `calculateBlockHash()` and `computeBlockHash()` functions

---

## 🎯 IMPLEMENTATION ROADMAP

### Phase 1: Security Critical (NOW - 3 days)
1. ✅ Document limitations (this file)
2. 🔨 Implement HD wallet private key derivation
3. 🔨 Implement BIP32/BIP44 key management
4. 🔨 Store HD seed securely in database
5. 🔨 Derivation path tracking per address

### Phase 2: Core Functionality (Week 2)
6. Transaction hex parser
7. Complete mempool validation
8. WebSocket notifications
9. Wallet encryption

### Phase 3: Enhancements (Week 3-4)
10. HD wallet coin selection
11. Reorg handling
12. Full P2P implementation

---

## 📊 CURRENT STATUS SUMMARY

| Category | Status | Count |
|----------|--------|-------|
| 🔴 Critical Security Issues | ✅ ALL RESOLVED! | 0 |
| 🟡 High Priority | Pending | 3 |
| 🟢 Medium Priority | Pending | 3 |
| 🔵 Low Priority | Pending | 3 |
| ✅ Recently Fixed | Complete | 5 |

**Total Known Issues:** 9 pending (no critical issues!)
**Recently Fixed:** 5 major improvements

---

## 🚀 FOR PRODUCTION DEPLOYMENT

**DO NOT deploy to production until:**
- [x] Server-side UTXO calculations implemented
- [x] Multiple outputs + change addresses working
- [x] **HD wallet private key derivation secure ✅**
- [ ] Wallet encryption implemented
- [ ] Mempool validation complete
- [ ] WebSocket notifications working
- [ ] Full testing on testnet for 30+ days
- [ ] External security audit completed

**Major Progress:** 3/8 critical items complete! HD wallet security implemented!

---

## 📞 REPORTING ISSUES

If you discover additional issues:
1. Check this document first
2. Add to GitHub Issues: https://github.com/DineroCoin/dinero/issues
3. Mark severity: 🔴 Critical, 🟡 High, 🟢 Medium, 🔵 Low

---

**Remember:** This is development software. Never use for real funds until security audit complete!
