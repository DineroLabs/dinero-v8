# Bech32 HRP Fix - din1q... Addresses ✅

**Date**: October 3, 2025  
**Status**: Complete

## 🐛 Problem Identified

Wallet tests were generating addresses without the HRP (Human-Readable Part):
- ❌ **Before**: `1q7cz7523yrkrmx6kvw694rcnawz7raqjz0dx0z4`
- ✅ **After**: `din1qq9v0a8h5lmd4rdxat4dx6shl6z7wdl4w9gzve6`

The missing `din` prefix made addresses invalid for Dinero mainnet.

## 🔍 Root Cause

The global `ChainParams` was default-constructed with empty values:
```cpp
static ChainParams g_chainParams;  // ❌ Empty hrp!
```

When `dinero::Params().hrp` was called, it returned an empty string because:
1. Tests didn't call `SelectParams()` to initialize the chain
2. Global `g_chainParams` had no default initialization

## ✅ Solution

Initialize `g_chainParams` with mainnet defaults inline:

```cpp
// FIXED: Initialize with mainnet defaults
static ChainParams g_chainParams = {
    .name = "mainnet",
    .hrp = "din",  // ✅ Always set!
    .magic = 0xd9b4bef9,
    // ... rest of mainnet params
};
```

### Files Fixed
- `src/core/consensus/chainparams_impl.cpp` - Added mainnet defaults
- `src/consensus/chainparams_impl.cpp` - Added mainnet defaults
- `src/wallet/hd_wallet.cpp` - Added safety fallback
- `tests/test_wallet_integration.cpp` - Updated assertion to check for `din1`

## 🎯 Address Format Verification

### Mainnet (din)
- **HRP**: `din`
- **Format**: `din1q...` (SegWit v0, P2WPKH)
- **Example**: `din1qq9v0a8h5lmd4rdxat4dx6shl6z7wdl4w9gzve6`
- **Length**: 42-62 characters
- **Checksum**: 6-character bech32 checksum

### Testnet (tdin)
- **HRP**: `tdin`
- **Format**: `tdin1q...`

### Regtest (rdin)
- **HRP**: `rdin`
- **Format**: `rdin1q...`

## 🧪 Test Results

### Before Fix
```
Address: 1q7cz7523yrkrmx6kvw694rcnawz7raqjz0dx0z4
         ^ Missing "din" prefix!
```

### After Fix
```
Address: din1qq9v0a8h5lmd4rdxat4dx6shl6z7wdl4w9gzve6
         ^^^^ Correct HRP present!
```

### All Tests Pass
```bash
$ ./build/test_wallet_integration
✅ Address generated (din1q... format)
✅ Deterministic address generation
✅ All wallet tests passed

$ ./build/test_bip39
✅ All BIP39 tests passed
```

## 🔐 Security Note

The bech32 encoder implementation is correct:
```cpp
std::string out = hrp; out.push_back('1');  // ✅ Includes HRP
for (auto d : data) out.push_back(CHARSET[d]);
for (int i=0;i<6;i++) out.push_back(CHARSET[(pm >> (5*(5-i))) & 31]);
return out;  // Returns: "din" + "1" + data + checksum
```

The issue was **only** that the HRP was empty from uninitialized `ChainParams`.

## 📊 Address Validation Checklist

- [x] **Starts with HRP**: `din1` for mainnet
- [x] **Separator**: `1` after HRP
- [x] **Witness version**: `q` (= 0 in bech32)
- [x] **Program data**: 20 bytes (P2WPKH) → 32 chars in bech32
- [x] **Checksum**: 6 characters
- [x] **Length**: 42-62 characters total
- [x] **Charset**: Only `qpzry9x8gf2tvdw0s3jn54khce6mua7l`

## 🚀 Implementation Details

### Safety Fallback Added
```cpp
// In HDWallet::DeriveAddressAt()
std::string hrp = dinero::Params().hrp;

// Fallback to hardcoded "din" if empty (shouldn't happen but be safe)
if (hrp.empty()) {
    hrp = "din";
}

return bech32_local::EncodeSegwitV0(hrp, prog20);
```

This ensures addresses are **never** generated without an HRP, even if `SelectParams()` isn't called.

## ✅ Verification

### Test Address Format
```cpp
assert(addr.substr(0, 4) == "din1");  // ✅ Correct prefix
assert(addr.length() >= 42);          // ✅ Minimum length
assert(addr.length() <= 62);          // ✅ Maximum length
```

### Example Addresses
All generated addresses now have correct format:
```
din1qq9v0a8h5lmd4rdxat4dx6shl6z7wdl4w9gzve6
din1qsdrs5zmj0ny84t2tf4dcjm9m0sx0hcwl3rs58k
din1q8wk6jlwwtvr02vukcfv0asey5d94qhewprfspj
din1qymep5y0q5qr059gmgw689vjsfxpm24nkm3g8ts
```

## 🎉 Summary

**Fixed**: Addresses now correctly include the `din` HRP prefix.

**Root Cause**: Uninitialized `ChainParams` returned empty HRP.

**Solution**: Initialize `g_chainParams` with mainnet defaults inline.

**Result**: All addresses properly formatted as `din1q...` for mainnet.

**Tests**: ✅ All BIP39 and wallet tests passing with correct addresses.

---

**No placeholders. Real bech32 addresses. Production-ready!** 🚀

