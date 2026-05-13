#pragma once

#include <string>
#include <vector>
#include <cstdint>

namespace dinero {
namespace bip39 {

// BIP39 standard: 12, 15, 18, 21, or 24 words
enum class WordCount {
    Words12 = 12,  // 128 bits entropy
    Words15 = 15,  // 160 bits
    Words18 = 18,  // 192 bits
    Words21 = 21,  // 224 bits
    Words24 = 24   // 256 bits
};

// Generate a new mnemonic from random entropy
// Returns empty string on failure
std::string Generate(WordCount word_count = WordCount::Words12);

// Convert mnemonic to 64-byte seed using PBKDF2-HMAC-SHA512
// passphrase is optional (empty string = no passphrase)
// Returns true on success, seed will be exactly 64 bytes
bool MnemonicToSeed(const std::string& mnemonic,
                    const std::string& passphrase,
                    std::vector<uint8_t>& seed_out,
                    bool skip_checksum = false);

// Validate mnemonic (check words exist and checksum is correct)
bool ValidateMnemonic(const std::string& mnemonic);

// Convert entropy bytes to mnemonic
std::string EntropyToMnemonic(const uint8_t* entropy, size_t entropy_len);

// Convert mnemonic back to entropy (for testing)
bool MnemonicToEntropy(const std::string& mnemonic, std::vector<uint8_t>& entropy_out);

} // namespace bip39
} // namespace dinero
