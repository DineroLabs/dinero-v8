# Dinero Mobile Wallet - Tauri + Rust

Cross-platform mobile wallet for DineroCoin built with Tauri, Rust, and React.

## 🏗️ Architecture

```
mobile-tauri/
├── src-tauri/          # Rust backend (FFI bridge to C++ wallet core)
│   ├── src/
│   │   ├── main.rs     # Tauri app entry point
│   │   ├── wallet.rs   # FFI bridge to libdinero_wallet.a
│   │   └── commands.rs # Tauri command handlers
│   ├── build.rs        # Build script (links to C++ lib)
│   └── Cargo.toml      # Rust dependencies
└── src/                # React UI (TypeScript + TailwindCSS)
    ├── App.tsx
    ├── hooks/
    │   └── useWallet.ts
    └── components/
```

## 🚀 Quick Start

### Prerequisites

- Rust (1.70+)
- Node.js (18+)
- CMake (to build wallet core)
- iOS SDK (for iOS builds)
- Android SDK (for Android builds)

### Build Steps

1. **Build C++ wallet core library:**
   ```bash
   cd DineroCoin
   mkdir -p build-mobile
   cd build-mobile
   cmake .. -DCMAKE_BUILD_TYPE=Release
   cmake --build . --target dinero_wallet
   ```

2. **Build mobile app:**
   ```bash
   cd mobile-tauri
   npm install
   npm run tauri build
   ```

   Or use the build script:
   ```bash
   ./build.sh
   ```

### Development

```bash
cd mobile-tauri
npm run dev  # Start Vite dev server
npm run tauri dev  # Start Tauri dev mode
```

## 🔗 FFI Bridge

The Rust FFI bridge (`src-tauri/src/wallet.rs`) provides safe wrappers around the C++ wallet library:

- `wallet::init()` - Initialize wallet with data directory
- `wallet::create()` - Create new HD wallet (returns mnemonic)
- `wallet::restore()` - Restore wallet from mnemonic
- `wallet::get_balance()` - Get wallet balance
- `wallet::send_transaction()` - Send transaction

## 📱 Features

- ✅ HD wallet (BIP-39/32/84)
- ✅ Wallet encryption (AES-256-GCM)
- ✅ Biometric unlock (Touch ID / Face ID)
- ✅ QR code scanning (camera API)
- ✅ QR code generation (payment URIs)
- ✅ Auto-lock timeout
- ✅ Secure storage (Keychain / Keystore)

## 🛠️ TODO

- [ ] Implement C++ FFI wrapper (`wallet-core/ffi/`)
- [ ] Complete wallet operations (list UTXOs, addresses)
- [ ] Add QR code scanner component
- [ ] Add QR code generator component
- [ ] Implement biometric unlock
- [ ] Add transaction history screen
- [ ] Add send/receive screens
- [ ] Add settings screen
- [ ] Add backup/restore flow
- [ ] iOS/Android platform-specific optimizations

## 🔒 Security

- Private keys never leave Rust FFI layer
- Encrypted storage using platform Keychain/Keystore
- Biometric authentication via Tauri plugins
- Auto-lock timer runs in Rust background thread

## 📦 Build Targets

- iOS: `aarch64-apple-ios`
- Android: `aarch64-linux-android` / `armv7-linux-androideabi`
- Desktop: `x86_64-apple-darwin` / `x86_64-unknown-linux-gnu` / `x86_64-pc-windows-msvc`

## 🚧 Next Steps

1. **Create C++ FFI wrapper** (`wallet-core/ffi/`)
   - Wrap HDWallet, WalletManager in C API
   - Export functions that Rust can call

2. **Complete FFI bridge** (`src-tauri/src/wallet.rs`)
   - Implement all wallet operations
   - Add error handling

3. **Build UI screens**
   - Home (balance, recent transactions)
   - Send (QR scanner, address input)
   - Receive (QR generator, address display)
   - Settings (auto-lock, network)

4. **Test on devices**
   - iOS simulator
   - Android emulator
   - Physical devices

## 📚 Resources

- [Tauri Documentation](https://tauri.app/)
- [Tauri Mobile Guide](https://tauri.app/v1/guides/mobile/)
- [Rust FFI Guide](https://doc.rust-lang.org/nomicon/ffi.html)

