#include "wallet/bip39.h"
#include "crypto/pbkdf2.h"
#include "crypto/dinero_crypto_minimal.h"
#include "third_party/bip39/english_words.hpp"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <unordered_map>
#include <cstring>

namespace dinero {
namespace bip39 {

namespace {
    std::vector<std::string> g_wordlist;
    std::unordered_map<std::string, uint16_t> g_word_index;
    bool g_initialized = false;

    bool InitializeWordlist() {
        if (g_initialized) return true;

        // Use embedded wordlist from english_words.hpp
        g_wordlist.clear();
        g_word_index.clear();

        for (uint16_t i = 0; i < 2048; i++) {
            std::string word(kBip39English[i]);
            g_wordlist.push_back(word);
            g_word_index[word] = i;
        }

        g_initialized = (g_wordlist.size() == 2048);
        return g_initialized;
    }
}

std::string Generate(WordCount word_count) {
    if (!InitializeWordlist()) return "";
    
    int wc = static_cast<int>(word_count);
    
    // Calculate entropy size: ENT = (word_count * 11 - checksum_bits) / 8
    // checksum_bits = ENT / 32
    // Solving: word_count * 11 = ENT + ENT/32 = ENT * 33/32
    // ENT = word_count * 11 * 32 / 33
    size_t entropy_bits = (wc * 32) / 3;  // Simplified formula
    size_t entropy_bytes = entropy_bits / 8;
    
    // Generate random entropy
    std::vector<uint8_t> entropy(entropy_bytes);
    if (!CF_GenerateRandomBytes(entropy.data(), entropy_bytes)) {
        return "";
    }
    
    return EntropyToMnemonic(entropy.data(), entropy_bytes);
}

std::string EntropyToMnemonic(const uint8_t* entropy, size_t entropy_len) {
    if (!InitializeWordlist()) return "";
    
    // Valid entropy lengths: 16, 20, 24, 28, 32 bytes
    if (entropy_len < 16 || entropy_len > 32 || entropy_len % 4 != 0) {
        return "";
    }
    
    // Calculate checksum
    uint8_t hash[32];
    sha256(entropy, entropy_len, hash);
    
    // Append checksum bits to entropy
    size_t checksum_bits = entropy_len / 4;
    std::vector<uint8_t> bits;
    
    // Convert entropy to bits
    for (size_t i = 0; i < entropy_len; i++) {
        for (int bit = 7; bit >= 0; bit--) {
            bits.push_back((entropy[i] >> bit) & 1);
        }
    }
    
    // Append checksum bits
    for (size_t i = 0; i < checksum_bits; i++) {
        bits.push_back((hash[0] >> (7 - i)) & 1);
    }
    
    // Split into 11-bit groups and convert to words
    std::vector<std::string> words;
    for (size_t i = 0; i < bits.size(); i += 11) {
        uint16_t index = 0;
        for (int j = 0; j < 11 && (i + j) < bits.size(); j++) {
            index = (index << 1) | bits[i + j];
        }
        if (index < g_wordlist.size()) {
            words.push_back(g_wordlist[index]);
        }
    }
    
    // Join with spaces
    std::string result;
    for (size_t i = 0; i < words.size(); i++) {
        if (i > 0) result += " ";
        result += words[i];
    }
    
    return result;
}

bool MnemonicToEntropy(const std::string& mnemonic, std::vector<uint8_t>& entropy_out) {
    if (!InitializeWordlist()) return false;
    
    // Split mnemonic into words
    std::vector<std::string> words;
    std::istringstream iss(mnemonic);
    std::string word;
    while (iss >> word) {
        words.push_back(word);
    }
    
    // Valid word counts: 12, 15, 18, 21, 24
    if (words.size() < 12 || words.size() > 24 || words.size() % 3 != 0) {
        return false;
    }
    
    // Convert words to indices
    std::vector<uint16_t> indices;
    for (const auto& w : words) {
        auto it = g_word_index.find(w);
        if (it == g_word_index.end()) {
            return false;
        }
        indices.push_back(it->second);
    }
    
    // Convert indices to bits
    std::vector<uint8_t> bits;
    for (uint16_t idx : indices) {
        for (int bit = 10; bit >= 0; bit--) {
            bits.push_back((idx >> bit) & 1);
        }
    }
    
    // Split entropy and checksum
    size_t total_bits = bits.size();
    size_t checksum_bits = total_bits / 33;
    size_t entropy_bits = total_bits - checksum_bits;
    size_t entropy_bytes = entropy_bits / 8;
    
    // Extract entropy
    entropy_out.resize(entropy_bytes);
    for (size_t i = 0; i < entropy_bytes; i++) {
        uint8_t byte = 0;
        for (int bit = 7; bit >= 0; bit--) {
            byte = (byte << 1) | bits[i * 8 + (7 - bit)];
        }
        entropy_out[i] = byte;
    }
    
    // Verify checksum
    uint8_t hash[32];
    sha256(entropy_out.data(), entropy_bytes, hash);
    
    for (size_t i = 0; i < checksum_bits; i++) {
        uint8_t expected = (hash[0] >> (7 - i)) & 1;
        uint8_t actual = bits[entropy_bits + i];
        if (expected != actual) {
            return false;
        }
    }
    
    return true;
}

bool ValidateMnemonic(const std::string& mnemonic) {
    std::vector<uint8_t> entropy;
    return MnemonicToEntropy(mnemonic, entropy);
}

bool MnemonicToSeed(const std::string& mnemonic,
                    const std::string& passphrase,
                    std::vector<uint8_t>& seed_out,
                    bool skip_checksum) {
    // Validate mnemonic (checksum can be bypassed for recovery of old wallets)
    if (!skip_checksum && !ValidateMnemonic(mnemonic)) {
        return false;
    }
    
    // BIP39 requires UTF-8 NFKD normalization
    // For ASCII-only strings (like standard BIP39 words), NFKD = original
    // But we must ensure we're using raw UTF-8 bytes, not C-string with null terminator
    std::string normalized_mnemonic = mnemonic;
    std::string normalized_passphrase = passphrase;
    
    // BIP39: salt = "mnemonic" + passphrase (both normalized)
    std::string salt_str = "mnemonic" + normalized_passphrase;

    // PBKDF2-HMAC-SHA512 with 2048 iterations
    // Use .data() and .size() to get raw UTF-8 bytes (not null-terminated C-string)
    seed_out.resize(64);
    dinero::crypto::PBKDF2_HMAC_SHA512(
        reinterpret_cast<const uint8_t*>(normalized_mnemonic.data()), normalized_mnemonic.size(),
        reinterpret_cast<const uint8_t*>(salt_str.data()), salt_str.size(),
        2048,
        seed_out.data(), 64
    );
    
    return true;
}

} // namespace bip39
} // namespace dinero
