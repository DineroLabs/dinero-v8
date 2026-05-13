#pragma once
#include <vector>
#include <string>
#include <cstdint>

namespace Dinero {
namespace BIP39 {

/**
 * BIP39 Mnemonic Implementation for Dinero HD Wallet
 * 
 * Features:
 * - Generate cryptographically secure mnemonics
 * - Validate mnemonic phrases
 * - Convert mnemonic to seed
 * - Support for multiple languages (English default)
 * - Entropy validation and checksum verification
 */

class MnemonicGenerator {
public:
    // Generate a new mnemonic phrase
    static std::string generateMnemonic(int entropyBits = 256);
    
    // Generate mnemonic from specific entropy
    static std::string generateMnemonicFromEntropy(const std::vector<uint8_t>& entropy);
    
    // Validate a mnemonic phrase
    static bool validateMnemonic(const std::string& mnemonic);
    
    // Convert mnemonic to seed (for HD wallet)
    static std::vector<uint8_t> mnemonicToSeed(const std::string& mnemonic, const std::string& passphrase = "");
    
    // Convert seed to mnemonic (for recovery)
    static std::string seedToMnemonic(const std::vector<uint8_t>& seed);
    
    // Get word list for validation
    static const std::vector<std::string>& getWordList();
    
    // Check if word is in BIP39 word list
    static bool isValidWord(const std::string& word);
    
    // Get entropy bits from mnemonic word count
    static int getEntropyBits(int wordCount);
    
    // Get word count from entropy bits
    static int getWordCount(int entropyBits);

private:
    // Internal entropy generation
    static std::vector<uint8_t> generateEntropy(int bits);
    
    // Calculate checksum
    static uint8_t calculateChecksum(const std::vector<uint8_t>& entropy);
    
    // Split mnemonic into words
    static std::vector<std::string> splitMnemonic(const std::string& mnemonic);
    
    // Join words into mnemonic
    static std::string joinMnemonic(const std::vector<std::string>& words);
    
    // Convert entropy to mnemonic indices
    static std::vector<int> entropyToIndices(const std::vector<uint8_t>& entropy);
    
    // Convert mnemonic indices to entropy
    static std::vector<uint8_t> indicesToEntropy(const std::vector<int>& indices);
    
    // PBKDF2 implementation for seed generation
    static std::vector<uint8_t> pbkdf2(const std::string& password, const std::string& salt, int iterations, int keyLength);
    
    // HMAC-SHA512 implementation
    static std::vector<uint8_t> hmacSha512(const std::vector<uint8_t>& key, const std::vector<uint8_t>& data);
};

} // namespace BIP39
} // namespace Dinero
