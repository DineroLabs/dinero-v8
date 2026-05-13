# iOS Library Build Status

## Current Issue

RocksDB fails to compile for iOS due to missing byte order macros (`__BYTE_ORDER`, `__LITTLE_ENDIAN`).

## Problem

- FFI library depends on `dinero_wallet`
- `dinero_wallet` depends on `dinero_consensus`
- `dinero_consensus` depends on RocksDB
- RocksDB has iOS compatibility issues

## Solutions

### Option 1: Fix RocksDB iOS Compatibility (Recommended)

Add iOS-specific defines to RocksDB build:

```cmake
# In CMakeLists.txt, add iOS-specific flags
if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
  target_compile_definitions(rocksdb PRIVATE
    __BYTE_ORDER=__LITTLE_ENDIAN
    __LITTLE_ENDIAN=1234
    __BIG_ENDIAN=4321
  )
endif()
```

### Option 2: Use Pre-built RocksDB

Use a pre-built RocksDB library for iOS instead of compiling from source.

### Option 3: Build Wallet-Only FFI (Limited)

Build FFI library without consensus/blockchain features:
- Wallet operations only
- No blockchain sync
- No UTXO tracking from chain

### Option 4: Use macOS Build (Not Recommended)

Build on macOS and convert architecture (architecture mismatch issues).

## Recommended: Fix RocksDB

The best approach is to fix RocksDB iOS compatibility. This requires:
1. Adding iOS-specific compile definitions
2. Possibly patching RocksDB source for iOS
3. Ensuring all dependencies compile for iOS

## Current Status

- ✅ Xcode project generates successfully
- ✅ dinero-miner issue fixed
- ❌ RocksDB compilation fails
- ⏳ FFI library build blocked

## Next Steps

1. Fix RocksDB iOS compatibility
2. Or: Create wallet-only FFI build (without RocksDB)
3. Or: Use alternative build approach

**For now, the placeholder library allows the iOS app to compile and test UI.**

