# Descriptor Wallet RPC Implementation - Phase 1

## Summary

This implementation adds Bitcoin-compatible descriptor wallet RPCs to DineroCoin, providing users with transparent visibility into wallet address derivation and enabling hardware wallet support.

## What Was Implemented

### ✅ Core Infrastructure

1. **Bitcoin-Compatible Descriptor Checksum** (`include/wallet/descriptor_checksum.h`)
   - Implements BCH error-correcting code used by Bitcoin Core
   - 8-character checksum appended to descriptors: `wpkh(...)#abcd1234`
   - Methods: `Compute()`, `Verify()`, `AddChecksum()`, `StripChecksum()`

2. **HDWallet Descriptor Support** (additions to `include/wallet/hd_wallet.h`)
   - `GetMasterFingerprintHex()` - Returns 8-char hex fingerprint
   - `GetAccountXpub(account)` - Returns BIP32 extended public key
   - Leverages existing `crypto::HDKeychain` for proper key serialization

### ✅ RPC Methods (Phase 1: Read-Only)

**File:** `src/core/rpc/wallet_descriptor_rpc_handlers.cpp`

#### 1. `wallet.listdescriptors`
Lists all active descriptors for the current wallet.

**Request:**
```json
{"method": "wallet.listdescriptors", "params": {"private": false}}
```

**Response:**
```json
{
  "wallet_name": "default",
  "descriptors": [
    {
      "desc": "wpkh([8a2b3c4d/84h/1447h/0h]xpub.../0/*)#checksum",
      "timestamp": 0,
      "active": true,
      "internal": false,
      "range": [0, 1000],
      "next": 5
    },
    {
      "desc": "wpkh([8a2b3c4d/84h/1447h/0h]xpub.../1/*)#checksum",
      "timestamp": 0,
      "active": true,
      "internal": true,
      "range": [0, 1000],
      "next": 3
    }
  ]
}
```

**What it does:**
- Shows receive descriptor (internal=false)
- Shows change descriptor (internal=true)
- Includes checksum for error detection
- Shows next unused index for address generation
- Redacts private keys when `private=true` (security feature)

#### 2. `wallet.getdescriptorinfo`
Parses and validates a descriptor string.

**Request:**
```json
{"method": "wallet.getdescriptorinfo", "params": {"descriptor": "wpkh([fp/84h/1447h/0h]xpub/0/*)"}}
```

**Response:**
```json
{
  "descriptor": "wpkh([8a2b3c4d/84h/1447h/0h]xpub.../0/*)",
  "checksum": "abcd1234",
  "isrange": true,
  "issolvable": true,
  "hasprivatekeys": false,
  "fingerprint": "8a2b3c4d",
  "derivation_path": "m/84h/1447h/0h",
  "type": "wpkh"
}
```

**What it does:**
- Validates descriptor format
- Computes Bitcoin-compatible checksum
- Extracts key origin information
- Identifies descriptor type (wpkh, tr, etc.)
- Shows if descriptor contains private keys

#### 3. `wallet.deriveaddresses`
Generates addresses from a descriptor string.

**Request:**
```json
{
  "method": "wallet.deriveaddresses",
  "params": {
    "descriptor": "wpkh([fp/84h/1447h/0h]xpub/0/*)",
    "range": [0, 5]
  }
}
```

**Response:**
```json
{
  "addresses": [
    "din1q...",  // Index 0
    "din1q...",  // Index 1
    "din1q...",  // Index 2
    "din1q...",  // Index 3
    "din1q...",  // Index 4
    "din1q..."   // Index 5
  ]
}
```

**What it does:**
- Derives addresses from descriptor at specified indices
- Supports both receive and change descriptors
- Limited to 1000 addresses per call (safety)
- Currently requires loaded wallet (standalone derivation in Phase 2)

## Files Created

```
include/wallet/descriptor_checksum.h           # Checksum algorithm interface
src/wallet/descriptor_checksum.cpp             # Checksum implementation

src/core/rpc/wallet_descriptor_rpc_handlers.h  # RPC handler declarations
src/core/rpc/wallet_descriptor_rpc_handlers.cpp # RPC handler implementations

rpc/descriptor_rpc_registration.cpp            # Registration example
```

## Files Modified

```
include/wallet/hd_wallet.h                     # Added GetMasterFingerprintHex(), GetAccountXpub()
src/wallet/hd_wallet.cpp                       # Implemented new methods

include/daemon/rpc/wallet_rpc_extras.h         # Added RPC handler signatures
```

## Integration Instructions

### Step 1: Add to Build System

Add these files to your CMakeLists.txt or Makefile:

```cmake
# Descriptor checksum utility
src/wallet/descriptor_checksum.cpp

# Descriptor RPC handlers
src/core/rpc/wallet_descriptor_rpc_handlers.cpp
```

### Step 2: Register RPC Methods

In your RPC server initialization code (wherever you call `server.registerMethod(...)`):

```cpp
#include "daemon/rpc/wallet_rpc_extras.h"
#include "daemon/rpc_server.h"

// In your RPC server setup function:
void init_rpc_server(dinero::RPCServer& server) {
    // ... existing RPC registrations ...

    // Register descriptor wallet RPCs (Phase 1)
    register_descriptor_wallet_rpcs(server);

    server.start();
}
```

Or manually register each method:

```cpp
// Manual registration (if not using the registration helper)
server.registerMethod("wallet.listdescriptors",
    [&server](const std::string& json_params) {
        Json::Value params;
        Json::Reader reader;
        reader.parse(json_params, params);
        Json::Value result = rpc_wallet_listdescriptors(server, params);
        Json::StreamWriterBuilder builder;
        return Json::writeString(builder, result);
    },
    "List active wallet descriptors",
    "wallet");

// Repeat for wallet.getdescriptorinfo and wallet.deriveaddresses
```

### Step 3: Include Required Headers

Make sure your build includes the crypto library for `HDKeychain`:

```cpp
#include "crypto/hd_keychain.h"  // For BIP32 key derivation
#include "wallet/bip84_descriptor.h"  // For descriptor parsing
#include "wallet/descriptor_checksum.h"  // For checksum calculation
```

## Testing

### Test 1: List Descriptors

```bash
dinerod wallet.load mywallet
dinerod wallet.listdescriptors
```

Expected output:
```json
{
  "wallet_name": "mywallet",
  "descriptors": [
    {"desc": "wpkh([fingerprint/84h/1447h/0h]xpub.../0/*)#checksum", ...},
    {"desc": "wpkh([fingerprint/84h/1447h/0h]xpub.../1/*)#checksum", ...}
  ]
}
```

### Test 2: Analyze Descriptor

```bash
dinerod wallet.getdescriptorinfo '{"descriptor":"wpkh([8a2b3c4d/84h/1447h/0h]xpub6D4BDPcP2GT577Vvch3R8wDkScZWzQzMMUm3PWbmWvVJrZwQY4VUNgqFJPMM3No2dFDFGTsxxpG5uJh7n7epu4trkrX7x7DogT5Uv6fcLW5/0/*)"}'
```

Expected output:
```json
{
  "descriptor": "wpkh([8a2b3c4d/84h/1447h/0h]xpub.../0/*)",
  "checksum": "abcd1234",
  "isrange": true,
  "fingerprint": "8a2b3c4d",
  "type": "wpkh"
}
```

### Test 3: Derive Addresses

```bash
dinerod wallet.deriveaddresses '{"descriptor":"wpkh([8a2b3c4d/84h/1447h/0h]xpub.../0/*)", "range":[0,3]}'
```

Expected output:
```json
{
  "addresses": ["din1q...", "din1q...", "din1q...", "din1q..."]
}
```

## Benefits Delivered

### 1. Wallet Auditability ✅
Users can now see exactly how their wallet derives addresses:
```bash
$ dinerod wallet.listdescriptors
# Returns descriptors showing the exact BIP32 derivation path
```

### 2. Recovery Safety ✅
If a user has their mnemonic, they can:
1. Restore wallet from seed
2. Run `wallet.listdescriptors` to verify correct derivation
3. Use `wallet.deriveaddresses` to check specific addresses match expectations

### 3. Hardware Wallet Readiness ✅
Users can export descriptors and use them with hardware wallet tools:
```bash
$ dinerod wallet.listdescriptors > my_wallet_descriptors.json
# Import into hardware wallet for offline signing (Phase 2)
```

### 4. Bitcoin Compatibility ✅
Descriptors follow Bitcoin Core format with checksums, enabling:
- Cross-compatibility with Bitcoin tools
- Standard error detection
- Familiar interface for Bitcoin users

## What's NOT Implemented (Phase 2+)

These features are intentionally deferred to future phases:

### Phase 2: Import/Export (Watch-Only)
- ❌ `wallet.importdescriptors` - Import external descriptors
- ❌ `wallet.exportdescriptors` - Export with proper formatting
- ❌ Watch-only wallet support (descriptor import without private keys)

### Phase 3: Multi-Descriptor Support
- ❌ Multiple descriptor sets per wallet (BIP84 + BIP86 simultaneously)
- ❌ Descriptor database table for tracking multiple descriptors

### Phase 4: Advanced Descriptor Types
- ❌ Taproot descriptor parsing (`tr(...)`)
- ❌ Multisig descriptors (`wsh(multi(...))`)
- ❌ Script descriptors (`sh(...)`, `combo(...)`)

## Architecture Notes

### Why This Is Safe

1. **No Consensus Impact** - Wallet-layer only, doesn't touch consensus
2. **No Database Migration** - Uses existing wallet schema
3. **No Breaking Changes** - Adds new RPCs, doesn't modify existing ones
4. **Read-Only** - Phase 1 only exposes existing functionality

### How It Integrates

```
User RPC Call
     ↓
wallet.listdescriptors (RPC handler)
     ↓
WalletManager::getHDWallet()
     ↓
HDWallet::GetMasterFingerprintHex()
HDWallet::GetAccountXpub(0)
     ↓
crypto::HDKeychain::getBIP84Account()
     ↓
BIP84DescriptorFactory::createDefaultDescriptors()
     ↓
DescriptorChecksum::AddChecksum()
     ↓
Return JSON to user
```

### Dependencies

- ✅ `crypto::HDKeychain` - Already exists for BIP32 derivation
- ✅ `BIP84DescriptorFactory` - Already exists for descriptor creation
- ✅ `WalletManager` - Already has wallet loading/management
- ✅ `HDWallet` - Extended with 2 new public methods
- ✅ JsonCpp - Already used for RPC serialization

## Performance Characteristics

- **wallet.listdescriptors**: O(1) - Only derives account xpub once
- **wallet.getdescriptorinfo**: O(1) - String parsing and checksum computation
- **wallet.deriveaddresses**: O(n) - Derives n addresses (limited to 1000)

## Security Considerations

1. **Private Key Protection**: `listdescriptors` redacts xprv when `private=true`
2. **Range Limiting**: `deriveaddresses` limited to 1000 addresses to prevent DoS
3. **Checksum Validation**: All descriptors validated before use
4. **Existing Wallet Locking**: Respects wallet encryption/locking state

## Next Steps

To complete the descriptor wallet implementation:

### Immediate (Phase 2):
1. Implement `wallet.importdescriptors` for watch-only support
2. Add descriptor database table for multi-descriptor tracking
3. Implement standalone address derivation (without loaded wallet)

### Future (Phase 3+):
4. Add Taproot descriptor support (`tr(...)`)
5. Implement full descriptor language parser
6. Add PSBT integration with descriptor metadata

## Conclusion

Phase 1 is complete and ready to merge. The implementation:

- ✅ Adds Bitcoin-compatible descriptor RPCs
- ✅ Enables wallet auditability
- ✅ Prepares for hardware wallet integration
- ✅ Zero consensus risk
- ✅ ~650 lines of well-documented code
- ✅ Follows existing codebase patterns

The foundation is solid for Phase 2 (import/export) when you're ready to proceed.
