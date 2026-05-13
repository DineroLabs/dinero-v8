# iOS Integration Guide: DineroCoin Wallet FFI

## 🎯 Goal
Integrate the DineroCoin Wallet FFI library (`libdinero_wallet_ffi.a`) into your native iOS app.

## 📋 Prerequisites

1. ✅ FFI library built for iOS (arm64)
2. ✅ Header files (`wallet_ffi.h`)
3. ✅ iOS app project

---

## 🔧 Step 1: Build FFI Library for iOS

### Option A: Cross-Compile from macOS

```bash
cd /path/to/dinero

# Create iOS build directory
mkdir -p build-ios

# Configure CMake for iOS
cmake -S . -B build-ios \
  -DCMAKE_SYSTEM_NAME=iOS \
  -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=13.0 \
  -DCMAKE_BUILD_TYPE=Release \
  -DENABLE_SANITIZERS=OFF \
  -DCMAKE_XCODE_ATTRIBUTE_ONLY_ACTIVE_ARCH=NO

# Build FFI library
cmake --build build-ios --target dinero_wallet_ffi
```

### Option B: Use Xcode Build (Recommended)

```bash
cd /path/to/dinero

# Generate Xcode project
cmake -S . -B build-xcode -G Xcode \
  -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=13.0

# Open in Xcode
open build-xcode/DineroCoin.xcodeproj

# Build target: dinero_wallet_ffi
# Select: Generic iOS Device or specific device
# Product → Build
```

**Output**: `build-ios/lib/libdinero_wallet_ffi.a` (or from Xcode)

---

## 📦 Step 2: Add FFI Library to iOS Project

### Method 1: Manual Integration (Recommended)

1. **Copy Library and Headers**:
   ```bash
   # Create FFI directory in iOS project
   mkdir -p /path/to/DineroiOS/Dinero/FFI
   
   # Copy library
   cp build-ios/lib/libdinero_wallet_ffi.a \
      /path/to/DineroiOS/Dinero/FFI/
   
   # Copy headers
   cp wallet-core/ffi/wallet_ffi.h \
      /path/to/DineroiOS/Dinero/FFI/
   cp wallet-core/ffi/kyc_provider.h \
      /path/to/DineroiOS/Dinero/FFI/
   cp wallet-core/ffi/kyc_provider_config.h \
      /path/to/DineroiOS/Dinero/FFI/
   ```

2. **Add to Xcode Project**:
   - Open `Dinero.xcodeproj` in Xcode
   - Right-click project → "Add Files to Dinero"
   - Select `FFI/` directory
   - Check "Copy items if needed"
   - Check "Create groups"

3. **Link Library**:
   - Select project → Build Settings
   - Search: "Other Linker Flags"
   - Add: `-ldinero_wallet_ffi`
   - Add: `-force_load $(SRCROOT)/Dinero/FFI/libdinero_wallet_ffi.a`

4. **Add Header Search Paths**:
   - Build Settings → "Header Search Paths"
   - Add: `$(SRCROOT)/Dinero/FFI`
   - Set: "Recursive" = YES

### Method 2: Swift Package Manager (If Supported)

```swift
// Package.swift or Xcode Package Dependencies
.package(
    url: "file:///path/to/dinero",
    from: "1.0.0"
)
```

---

## 🔗 Step 3: Create Swift Bridging Header

Create `Dinero-Bridging-Header.h`:

```objc
//
//  Dinero-Bridging-Header.h
//  Dinero
//

#ifndef Dinero_Bridging_Header_h
#define Dinero_Bridging_Header_h

// Import FFI C headers
#import "wallet_ffi.h"
#import "kyc_provider.h"

// Note: C++ headers need to be wrapped in extern "C" if needed
// For now, wallet_ffi.h should be C-compatible

#endif /* Dinero_Bridging_Header_h */
```

**In Xcode**:
- Build Settings → "Objective-C Bridging Header"
- Set to: `Dinero/Dinero-Bridging-Header.h`

---

## 📱 Step 4: Create Swift Wrapper Classes

Create `Dinero/FFI/WalletFFI.swift`:

```swift
//
//  WalletFFI.swift
//  Dinero
//

import Foundation

/// Swift wrapper for DineroCoin Wallet FFI
class WalletFFI {
    
    // MARK: - Initialization
    
    /// Initialize wallet with data directory
    static func initialize(datadir: String) -> Bool {
        return dinero_wallet_init(datadir) == 0
    }
    
    // MARK: - Wallet Creation
    
    /// Create new HD wallet
    static func createWallet(datadir: String, coinType: UInt32) -> (success: Bool, mnemonic: String?) {
        var mnemonicPtr: UnsafeMutablePointer<CChar>? = nil
        let result = dinero_wallet_create_hd_wallet(datadir, coinType, &mnemonicPtr)
        
        guard result == 0, let mnemonicPtr = mnemonicPtr else {
            return (false, nil)
        }
        
        let mnemonic = String(cString: mnemonicPtr)
        dinero_wallet_free_string(mnemonicPtr)
        
        return (true, mnemonic)
    }
    
    /// Restore wallet from mnemonic
    static func restoreWallet(datadir: String, coinType: UInt32, mnemonic: String, passphrase: String = "") -> Bool {
        return dinero_wallet_restore_hd_wallet(datadir, coinType, mnemonic, passphrase) == 0
    }
    
    // MARK: - Address Management
    
    /// Derive next receive address
    static func deriveNextAddress() -> String? {
        var addressPtr: UnsafeMutablePointer<CChar>? = nil
        let result = dinero_wallet_derive_next_address(&addressPtr)
        
        guard result == 0, let addressPtr = addressPtr else {
            return nil
        }
        
        let address = String(cString: addressPtr)
        dinero_wallet_free_string(addressPtr)
        
        return address
    }
    
    // MARK: - Balance
    
    /// Get wallet balance
    static func getBalance() -> (confirmed: UInt64, unconfirmed: UInt64) {
        var balance = FFI_WalletBalance()
        let result = dinero_wallet_get_balance(&balance)
        
        guard result == 0 else {
            return (0, 0)
        }
        
        return (balance.confirmed, balance.unconfirmed)
    }
    
    // MARK: - KYC Provider
    
    /// Initialize KYC provider
    static func initializeKYCProvider(providerType: String, config: String) -> Bool {
        return dinero_wallet_init_kyc_provider(providerType, config) == 0
    }
    
    /// Get KYC status
    static func getKYCStatus() -> KYCStatus? {
        var status = FFI_KYCStatus()
        let result = dinero_wallet_get_kyc_status(&status)
        
        guard result == 0 else {
            return nil
        }
        
        return KYCStatus(from: status)
    }
    
    /// Start KYC verification
    static func startKYCVerification(level: String, country: String) -> String? {
        var urlPtr: UnsafeMutablePointer<CChar>? = nil
        let result = dinero_wallet_start_kyc_verification(level, country, &urlPtr)
        
        guard result == 0, let urlPtr = urlPtr else {
            return nil
        }
        
        let url = String(cString: urlPtr)
        dinero_wallet_free_string(urlPtr)
        
        return url
    }
}

// MARK: - Helper Types

struct KYCStatus {
    let isVerified: Bool
    let verificationLevel: String
    let provider: String
    let verifiedAt: Int64
    let expiresAt: Int64
    let country: String
    
    init(from ffiStatus: FFI_KYCStatus) {
        self.isVerified = ffiStatus.is_verified
        self.verificationLevel = String(cString: ffiStatus.verification_level)
        self.provider = String(cString: ffiStatus.provider)
        self.verifiedAt = ffiStatus.verified_at
        self.expiresAt = ffiStatus.expires_at
        self.country = String(cString: ffiStatus.country)
    }
}
```

---

## 🎨 Step 5: Use in iOS UI

Example usage in a ViewController:

```swift
import UIKit

class WalletViewController: UIViewController {
    
    override func viewDidLoad() {
        super.viewDidLoad()
        
        // Initialize wallet
        let datadir = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0].path
        WalletFFI.initialize(datadir: datadir)
        
        // Create or restore wallet
        let result = WalletFFI.createWallet(datadir: datadir, coinType: 1448)
        if let mnemonic = result.mnemonic {
            print("Wallet created! Mnemonic: \(mnemonic)")
            // Store mnemonic securely (Keychain)
        }
        
        // Get balance
        let balance = WalletFFI.getBalance()
        print("Balance: \(balance.confirmed)")
        
        // Derive address
        if let address = WalletFFI.deriveNextAddress() {
            print("Address: \(address)")
        }
        
        // Initialize KYC provider
        WalletFFI.initializeKYCProvider(
            providerType: "openkyc",
            config: "api_url=https://openkyc.dinero-coin.com"
        )
        
        // Check KYC status
        if let status = WalletFFI.getKYCStatus() {
            print("KYC Status: \(status.isVerified)")
        }
    }
}
```

---

## 🔐 Step 6: Handle Dependencies

The FFI library depends on:

1. **OpenSSL** (for crypto)
2. **SQLite3** (for wallet storage)
3. **secp256k1** (for elliptic curve crypto)
4. **jsoncpp** (for JSON parsing)
5. **CURL** (for HTTP requests)

### Option A: Static Linking (Recommended)

All dependencies are statically linked in `libdinero_wallet_ffi.a` - no additional setup needed!

### Option B: System Frameworks

Add to Xcode:
- `Security.framework` (for Keychain)
- `Foundation.framework` (already included)

---

## 📋 Step 7: Build Settings

### Required Settings:

1. **Architectures**:
   - `arm64` (device)
   - `arm64-simulator` (if building for simulator)

2. **Minimum Deployment**:
   - iOS 13.0+ (recommended)

3. **Bitcode**:
   - Enable Bitcode: NO (if using static library)

4. **C++ Standard**:
   - C++17 (if building from source)

---

## 🧪 Step 8: Testing

### Test FFI Integration:

```swift
func testWalletFFI() {
    let datadir = NSTemporaryDirectory()
    
    // Test initialization
    XCTAssertTrue(WalletFFI.initialize(datadir: datadir))
    
    // Test wallet creation
    let result = WalletFFI.createWallet(datadir: datadir, coinType: 1448)
    XCTAssertTrue(result.success)
    XCTAssertNotNil(result.mnemonic)
    
    // Test address derivation
    let address = WalletFFI.deriveNextAddress()
    XCTAssertNotNil(address)
    XCTAssertTrue(address!.hasPrefix("din1"))
}
```

---

## 🚀 Quick Start Checklist

- [ ] Build FFI library for iOS (arm64)
- [ ] Copy library and headers to iOS project
- [ ] Add library to Xcode project
- [ ] Configure linker flags
- [ ] Create bridging header
- [ ] Create Swift wrapper class
- [ ] Test basic wallet operations
- [ ] Integrate into UI

---

## 📚 Key Differences from Tauri

| Tauri | iOS Native |
|-------|------------|
| Rust FFI bindings | Swift wrapper classes |
| Shared library (.so/.dylib) | Static library (.a) |
| Tauri commands | Direct Swift calls |
| WebView UI | Native UIKit/SwiftUI |

**The FFI C API is the same!** Only the wrapper layer changes.

---

## ✅ Next Steps

1. Build FFI library for iOS
2. Copy to iOS project
3. Create Swift wrapper
4. Test integration
5. Build UI components

**Ready to start?** Begin with Step 1 (building FFI library for iOS)! 🚀
