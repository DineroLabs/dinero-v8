# Coin Type 1447 Fix - November 7, 2025

## 🚨 Critical Bug Fixed

**Problem**: Dinero wallet was using Bitcoin testnet coin type (**1**) instead of Dinero's official SLIP-0044 coin type (**1447**).

**Impact**: Generated completely different addresses than expected.

---

## ✅ Fix Applied

### Changed Files
- `src/daemon/rpc/WalletHandlers.cpp`

### Code Changes

**Before**:
```cpp
HDWallet::Open(wallet_datadir_, 1);          // ❌ Bitcoin testnet
HDWallet::CreateNew(wallet_datadir_, 1, mnemonic);
HDWallet::Restore(wallet_datadir_, 1, mnemonic, passphrase);
```

**After**:
```cpp
HDWallet::Open(wallet_datadir_, 1447);       // ✅ Dinero SLIP-0044
HDWallet::CreateNew(wallet_datadir_, 1447, mnemonic);
HDWallet::Restore(wallet_datadir_, 1447, mnemonic, passphrase);
```

---

## 📋 Derivation Paths

### Before (WRONG)
```
Path: m/84'/1'/0'/0/0
Network: Bitcoin testnet
Coin type: 1
```

### After (CORRECT)
```
Path: m/84'/1447'/0'/0/0
Network: Dinero mainnet
Coin type: 1447 (SLIP-0044 official)
```

---

## ⚠️ Breaking Change

**This fix generates DIFFERENT addresses!**

If you have an existing wallet with coin_type=1:
1. ❌ **Old addresses will NOT work** with new code
2. ✅ **You MUST re-create wallet** with coin_type=1447
3. ✅ **Your seed phrase is still valid** (just different derivation)

---

## 🔍 Verification Required

### For Premine Address

**Address**: `din1q7gs8mgsnzmw3ur4wtt7snknhedzz5rx5xdvn94`

**Verify**:
1. Use Ian Coleman BIP39 tool (offline)
2. Enter your 12-word seed
3. Set coin type: **1447**
4. Set path: BIP84 (`m/84'/1447'/0'/0/0`)
5. Set HRP: **din**
6. Confirm address matches: `din1q7gs8mgsnzmw3ur4wtt7snknhedzz5rx5xdvn94`

**Security Check**:
```bash
# Check block 1 on mainnet
curl --user dinero:$COOKIE \
  -X POST http://127.0.0.1:20997 \
  -H 'Content-Type: application/json' \
  -d '{"method":"getblock","params":[1, 2],"id":1}'

# Look for premine output address in coinbase transaction
```

---

## 📊 Impact on Mainnet

### Genesis (Block 0)
- ✅ Unaffected (no derivation, hardcoded)
- ✅ Already in RocksDB

### Premine (Block 1)
- ⚠️ **Address MUST be verified**
- Amount: 2,627,900 DIN
- If address derived with coin_type=1, it's **WRONG**
- Must re-derive with coin_type=1447

---

## 🎯 Action Items

### For Users with Existing Wallets

1. **Back up your seed phrase** (12 or 24 words)
2. **Delete old wallet.conf** (uses coin_type=1)
3. **Re-create wallet** (will use coin_type=1447)
4. **Verify addresses** match expected derivation

### For Premine Holder

1. **Verify seed** derives to `din1q7gs8mgsnzmw3ur4wtt7snknhedzz5rx5xdvn94`
2. **Check block 1** contains that address
3. **Test Bech32 decoder** accepts the address
4. **DO NOT proceed** until all 3 verified ✅

---

## 🔧 Technical Details

### SLIP-0044 Registration
- **Coin**: Dinero
- **Symbol**: DIN
- **Coin type**: 1447
- **Path prefix**: m/44'/1447'/ (BIP44) or m/84'/1447'/ (BIP84)

### Bech32 Format
- **HRP**: din
- **Version**: 0 (witness v0)
- **Length**: 43 characters for P2WPKH
- **Example**: `din1q...` (lowercase only)

---

## 📝 Migration Path

### Testnet Wallets
- If you used testnet with coin_type=1, that's fine
- For mainnet, must use coin_type=1447

### Mainnet Wallets
- All mainnet wallets **must** use coin_type=1447
- Old wallets with coin_type=1 are **invalid** for Dinero mainnet

---

## ✅ Verification

Build successful:
```bash
cmake --build build --target dinerod -j8
# [100%] Built target dinerod
```

Committed:
```
commit: [hash]
date: November 7, 2025
```

---

## 🚀 Next Steps

1. Test wallet creation with coin_type=1447
2. Verify address derivation
3. Check premine block 1 on mainnet
4. Ensure Bech32 decoder works
5. Update all documentation

---

**Status**: ✅ FIXED  
**Severity**: CRITICAL  
**Impact**: All wallet addresses  
**Required Action**: Re-create wallets with coin_type=1447

