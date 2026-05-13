# BIP39 Implementation for DineroCoin

This directory contains the BIP39 (Bitcoin Improvement Proposal 39) implementation for mnemonic seed phrase generation and validation.

## Files

### `english.txt`
- **Purpose**: Official BIP39 English wordlist containing exactly 2048 words
- **Source**: Bitcoin BIPs repository (https://github.com/bitcoin/bips/blob/master/bip-0039/english.txt)
- **Format**: Plain text file, one word per line
- **Usage**: Reference for external tools, testing, and validation

### `english_words.hpp`
- **Purpose**: C++ header file with embedded BIP39 English wordlist
- **Format**: Constant array `kBip39English[2048]` containing all 2048 words
- **Usage**: Used by the C++ BIP39 implementation in `src/crypto/bip39.cpp`
- **Advantage**: Compile-time embedding for optimal performance

## BIP39 Implementation

The main BIP39 implementation is located in:
- **Header**: `src/crypto/bip39.hpp`
- **Implementation**: `src/crypto/bip39.cpp`

### Key Functions

```cpp
namespace dinero::bip39 {
    // Generate mnemonic from entropy (128-256 bits, multiple of 32)
    std::string mnemonic_from_entropy(const uint8_t* entropy, size_t len);
    
    // Convert mnemonic to seed using PBKDF2-HMAC-SHA512
    void mnemonic_to_seed(const std::string& mnemonic,
                          const std::string& passphrase,
                          uint8_t out64[64]);
}
```

## Testing

Use the provided test script to verify BIP39 functionality:

```bash
# Run BIP39 test
./scripts/test-bip39.sh

# Or compile manually
c++ -std=c++17 -I./include -I./src -I. -I./secp-prefix/include \
    -framework Security your_test.cpp src/crypto/bip39.cpp \
    src/crypto/dinero_crypto_minimal.cpp third_party/crypto/ripemd160.c \
    -L./secp-prefix/lib -lsecp256k1 -o your_test
```

## Example Usage

```cpp
#include "crypto/bip39.hpp"

// Generate mnemonic from 16 bytes of entropy (128 bits -> 12 words)
uint8_t entropy[16] = {0x00, 0x01, 0x02, /* ... */};
std::string mnemonic = dinero::bip39::mnemonic_from_entropy(entropy, 16);
// Result: "abandon amount liar amount expire adjust cage candy arch gather drum buyer"

// Convert mnemonic to seed
uint8_t seed[64];
dinero::bip39::mnemonic_to_seed(mnemonic, "", seed);
```

## Standards Compliance

- **BIP39**: Bitcoin Improvement Proposal 39 for mnemonic code generation
- **Wordlist**: Official 2048-word English wordlist
- **Entropy**: Supports 128, 160, 192, 224, and 256 bits of entropy
- **Checksum**: Proper checksum validation using SHA256
- **PBKDF2**: Standard PBKDF2-HMAC-SHA512 for seed derivation (2048 iterations)

## Integration

The BIP39 implementation is integrated into the DineroCoin wallet system:
- **HD Wallets**: Used for hierarchical deterministic wallet generation
- **Seed Phrases**: Backup and recovery of wallet keys
- **Cross-Platform**: Works on macOS, Linux, and Windows
