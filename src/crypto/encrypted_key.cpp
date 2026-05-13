#include "crypto/encrypted_key.h"
#include "crypto/decrypt_encrypted_key.h"
#include "common/logger.h"
#include <cstring>
#include <memory>
#include <sstream>
#include <iomanip>

namespace dinero::crypto {

bool ParseBase64(const std::string& base64_data, std::vector<uint8_t>& output) {
    output.clear();
    
    if (base64_data.empty()) {
        return false;
    }
    
    // Simple base64 decoder
    static const std::string chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    static int decode_table[128];
    static bool table_initialized = false;
    
    if (!table_initialized) {
        std::fill(decode_table, decode_table + 128, -1);
        for (int i = 0; i < 64; i++) {
            decode_table[chars[i]] = i;
        }
        table_initialized = true;
    }
    
    std::string input = base64_data;
    // Remove padding
    while (!input.empty() && input.back() == '=') {
        input.pop_back();
    }
    
    if (input.length() % 4 == 1) {
        return false; // Invalid base64
    }
    
    output.reserve((input.length() * 3) / 4);
    
    for (size_t i = 0; i < input.length(); i += 4) {
        uint32_t val = 0;
        int padding = 0;
        
        for (int j = 0; j < 4; j++) {
            if (i + j < input.length()) {
                char c = input[i + j];
                unsigned char uc = static_cast<unsigned char>(c);
                if (uc >= 128 || decode_table[uc] == static_cast<char>(-1)) {
                    return false;
                }
                val = (val << 6) | decode_table[uc];
            } else {
                val <<= 6;
                padding++;
            }
        }
        
        if (padding < 3) output.push_back((val >> 16) & 0xFF);
        if (padding < 2) output.push_back((val >> 8) & 0xFF);
        if (padding < 1) output.push_back(val & 0xFF);
    }
    
    return true;
}

void SecureZero(void* ptr, size_t size) {
    if (ptr && size > 0) {
        // Secure memory clearing - prevent compiler optimization
        volatile uint8_t* vptr = static_cast<volatile uint8_t*>(ptr);
        for (size_t i = 0; i < size; i++) {
            vptr[i] = 0;
        }
    }
}

bool DecryptPrivateKey(const EncryptedKeyParams& params, 
                       std::array<uint8_t, 32>& private_key) {
    // Clear output
    SecureZero(private_key);
    
    try {
        // Decrypt using OpenSSL directly
        auto result = decrypt_pbkdf2_aes256_gcm(params);
        
        if (!result.ok) {
            dinero::g_logger.error("Encrypted key decryption failed: " + result.err);
            return false;
        }
        
        if (result.key32.size() != 32) {
            dinero::g_logger.error("Decrypted key is not 32 bytes: " + std::to_string(result.key32.size()));
            return false;
        }
        
        // Copy to output array
        std::copy(result.key32.begin(), result.key32.end(), private_key.begin());
        
        // Securely clear the decrypted key vector
        SecureZero(result.key32);
        
        dinero::g_logger.info("Successfully decrypted private key using " + params.enc + 
                             " with " + std::to_string(params.iter) + " iterations");
        
        return true;
        
    } catch (const std::exception& e) {
        dinero::g_logger.error("Exception during encrypted key decryption: " + std::string(e.what()));
        SecureZero(private_key);
        return false;
    }
}

} // namespace dinero::crypto
