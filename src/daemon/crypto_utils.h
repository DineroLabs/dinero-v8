#pragma once

#include <vector>
#include <cstdint>
#include <string>
#include <cstddef>

// Real cryptographic utilities - no placeholders
// Uses OpenSSL EVP for cross-platform compatibility

class CryptoUtils {
public:
    // Standard Bitcoin HASH160 = RIPEMD160(SHA256(data))
    static std::vector<uint8_t> HASH160(const uint8_t* data, size_t length);
    
    // Individual hash functions
    static std::vector<uint8_t> SHA256(const uint8_t* data, size_t length);
    static std::vector<uint8_t> RIPEMD160(const uint8_t* data, size_t length);
    
    // Double SHA256 (for txids/block hashes)
    static std::vector<uint8_t> DoubleSHA256(const uint8_t* data, size_t length);
    
    // Utility functions
    static std::string bytes_to_hex(const uint8_t* bytes, size_t length);
    static std::vector<uint8_t> hex_to_bytes(const std::string& hex);
    
private:
    // Initialize OpenSSL if needed
    static void ensure_openssl_init();
};
