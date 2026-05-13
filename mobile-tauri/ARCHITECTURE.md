# Dinero Mobile Wallet - Architecture & Implementation Plan

## 🎯 Project Status

**Created:** Initial project structure ✅  
**FFI Bridge:** Skeleton complete ✅  
**C++ FFI Wrapper:** TODO (next step)  
**UI Components:** Basic skeleton ✅  

## 📁 Project Structure

```
DineroCoin/
├── mobile-tauri/              # NEW: Mobile wallet app
│   ├── src-tauri/            # Rust backend
│   │   ├── src/
│   │   │   ├── main.rs       # ✅ Tauri app entry
│   │   │   ├── wallet.rs     # ✅ FFI bridge skeleton
│   │   │   └── commands.rs   # ✅ Tauri commands
│   │   ├── build.rs          # ✅ Links to libdinero_wallet.a
│   │   └── Cargo.toml        # ✅ Dependencies
│   └── src/                  # React UI
│       ├── App.tsx           # ✅ Basic UI skeleton
│       ├── hooks/
│       │   └── useWallet.ts  # ✅ Wallet hook
│       └── components/       # TODO: UI components
│
└── wallet-core/              # TODO: Extract shared library
    └── ffi/                  # TODO: C API wrapper
```

## 🔗 FFI Architecture

### Current Implementation

**Rust Side (`mobile-tauri/src-tauri/src/wallet.rs`):**
- ✅ FFI function declarations
- ✅ Safe Rust wrappers
- ✅ Error handling

**C++ Side (TODO):**
- Need to create `wallet-core/ffi/wallet_ffi.h` and `wallet_ffi.cpp`
- Expose C API functions matching Rust declarations
- Wrap existing `HDWallet` and `WalletManager` classes

### FFI Functions Needed

```c
// wallet-core/ffi/wallet_ffi.h
extern "C" {
    int dinero_wallet_init(const char* datadir);
    int dinero_wallet_create(char** mnemonic_out);
    int dinero_wallet_restore(const char* mnemonic, const char* passphrase);
    int dinero_wallet_encrypt(const char* password);
    int dinero_wallet_unlock(const char* password, int timeout_seconds);
    int dinero_wallet_lock();
    bool dinero_wallet_is_encrypted();
    bool dinero_wallet_is_locked();
    WalletBalance dinero_wallet_get_balance();
    int dinero_wallet_get_new_address(const char* label, char** address_out);
    int dinero_wallet_send_transaction(
        const char* to, double amount, double fee_rate,
        const char* note, char** txid_out
    );
    // ... more functions
}
```

## 🛠️ Next Steps

### 1. Create C++ FFI Wrapper (Priority 1)

**Location:** `wallet-core/ffi/`

**Files to create:**
- `wallet_ffi.h` - C API header
- `wallet_ffi.cpp` - C API implementation
- `CMakeLists.txt` - Build libdinero_wallet.a

**Implementation approach:**
- Wrap `HDWallet` class in C functions
- Use `extern "C"` for C linkage
- Return error codes (0 = success, non-zero = error)
- Use `char**` for string outputs (caller allocates)

### 2. Update CMakeLists.txt

**Add to main `CMakeLists.txt`:**
```cmake
# Build wallet core library for FFI
add_subdirectory(wallet-core)
```

**Create `wallet-core/CMakeLists.txt`:**
```cmake
add_library(dinero_wallet STATIC
    ffi/wallet_ffi.cpp
    # Link to existing wallet sources
)
target_link_libraries(dinero_wallet
    dinero_common
    secp256k1
    jsoncpp
)
```

### 3. Complete FFI Bridge

**Update `mobile-tauri/src-tauri/src/wallet.rs`:**
- Implement all Rust wrapper functions
- Add proper error handling
- Test FFI calls

### 4. Build UI Components

**Create React components:**
- `components/QRScanner.tsx` - Camera QR scanner
- `components/QRGenerator.tsx` - QR code display
- `screens/Home.tsx` - Balance, transactions
- `screens/Send.tsx` - Send transaction form
- `screens/Receive.tsx` - Receive address + QR
- `screens/Settings.tsx` - Settings, auto-lock

### 5. Add Platform Features

**Tauri plugins:**
- ✅ `tauri-plugin-camera` - QR scanning
- ✅ `tauri-plugin-biometric` - Touch ID / Face ID
- ✅ `tauri-plugin-notification` - Push notifications
- ✅ `tauri-plugin-secure-store` - Secure storage

## 🧪 Testing Strategy

1. **Unit tests** - Rust FFI wrappers
2. **Integration tests** - C++ → Rust → React flow
3. **Device tests** - iOS simulator, Android emulator
4. **Security audit** - Private key handling

## 📝 Notes

- **FFI Safety:** All FFI functions use `unsafe` blocks in Rust
- **Memory Management:** C++ allocates strings, Rust frees them
- **Error Handling:** C functions return error codes, Rust converts to `Result`
- **Thread Safety:** Wallet operations run on Tauri's async runtime

## 🔒 Security Considerations

- Private keys never exposed to JavaScript
- All crypto operations in Rust/C++ layer
- Encrypted storage via platform Keychain/Keystore
- Biometric unlock before wallet operations
- Auto-lock timer in Rust background thread

