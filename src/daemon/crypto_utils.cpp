#include "crypto_utils.h"
#include "crypto/sha256.h"
#include "../crypto/ripemd160_standalone.h"
#include <stdexcept>
#include <iostream>
#include <iomanip>
#include <sstream>

void CryptoUtils::ensure_openssl_init() {
    // No longer needed - using vendored implementations
}

std::vector<uint8_t> CryptoUtils::SHA256(const uint8_t* data, size_t length) {
    std::vector<uint8_t> result(32);
    
    dinero::crypto::CSHA256 hasher;
    hasher.Write(data, length);
    hasher.Finalize(result.data());
    
    return result;
}

std::vector<uint8_t> CryptoUtils::RIPEMD160(const uint8_t* data, size_t length) {
    std::vector<uint8_t> result(20);
    
    dinero::crypto::CRIPEMD160 hasher;
    hasher.Write(data, length);
    hasher.Finalize(result.data());
    
    return result;
}

std::vector<uint8_t> CryptoUtils::HASH160(const uint8_t* data, size_t length) {
    // Standard Bitcoin HASH160 = RIPEMD160(SHA256(data))
    auto sha256_result = SHA256(data, length);
    return RIPEMD160(sha256_result.data(), sha256_result.size());
}

std::vector<uint8_t> CryptoUtils::DoubleSHA256(const uint8_t* data, size_t length) {
    // Double SHA256 for txids and block hashes
    auto first_hash = SHA256(data, length);
    return SHA256(first_hash.data(), first_hash.size());
}

std::string CryptoUtils::bytes_to_hex(const uint8_t* bytes, size_t length) {
    std::ostringstream hex_stream;
    hex_stream << std::hex << std::setfill('0');
    
    for (size_t i = 0; i < length; i++) {
        hex_stream << std::setw(2) << static_cast<int>(bytes[i]);
    }
    
    return hex_stream.str();
}

std::vector<uint8_t> CryptoUtils::hex_to_bytes(const std::string& hex) {
    if (hex.length() % 2 != 0) {
        throw std::invalid_argument("Hex string must have even length");
    }
    
    std::vector<uint8_t> result;
    result.reserve(hex.length() / 2);
    
    for (size_t i = 0; i < hex.length(); i += 2) {
        std::string byte_str = hex.substr(i, 2);
        uint8_t byte = static_cast<uint8_t>(std::stoul(byte_str, nullptr, 16));
        result.push_back(byte);
    }
    
    return result;
}
