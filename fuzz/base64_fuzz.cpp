/**
 * LibFuzzer target for base64 decoding
 * Tests base64Decode function for integer overflow and buffer issues
 */

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

// Include your base64 implementation
// #include "daemon/rpc_server.h"  // Adjust path as needed

// Mock base64 decode function - replace with actual implementation
std::string base64Decode(const std::string& encoded) {
    static const std::string chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    
    std::string decoded;
    uint32_t val = 0;  // This was the integer overflow bug - should be uint32_t
    int bits = -8;
    
    for (char c : encoded) {
        if (c == '=') break;
        
        size_t pos = chars.find(c);
        if (pos == std::string::npos) {
            // Invalid character
            return "";
        }
        
        val = (val << 6) + static_cast<uint32_t>(pos);
        bits += 6;
        
        if (bits >= 0) {
            decoded.push_back(static_cast<char>((val >> bits) & 0xFF));
            bits -= 8;
        }
    }
    
    return decoded;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size == 0 || size > 100000) return 0;  // Skip empty or huge inputs
    
    std::string input(reinterpret_cast<const char*>(data), size);
    
    try {
        // Test base64 decoding with fuzzed input
        std::string decoded = base64Decode(input);
        
        // Test edge cases that could cause integer overflow
        if (!decoded.empty()) {
            // Verify decoded data doesn't cause issues
            volatile char first = decoded[0];
            volatile char last = decoded[decoded.size() - 1];
            (void)first; (void)last;  // Suppress unused warnings
        }
        
        // Test with padded input
        if (input.size() > 0 && input.back() != '=') {
            std::string padded = input + "=";
            std::string decoded_padded = base64Decode(padded);
            
            padded = input + "==";
            decoded_padded = base64Decode(padded);
        }
        
    } catch (...) {
        // Catch any exceptions - fuzzer should not crash
        return 0;
    }
    
    return 0;
}

// Custom mutator for base64-specific fuzzing
extern "C" size_t LLVMFuzzerCustomMutator(uint8_t* data, size_t size,
                                          size_t max_size, unsigned int seed) {
    if (size == 0 || max_size < 4) return 0;
    
    static const char base64_chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/=";
    static const char invalid_chars[] = "!@#$%^&*()[]{}|\\:;\"'<>?,./`~";
    
    // Sometimes generate valid base64
    if (seed % 5 == 0) {
        size_t len = (seed % (max_size / 4)) * 4;  // Multiple of 4
        if (len < 4) len = 4;
        if (len > max_size) len = max_size;
        
        for (size_t i = 0; i < len - 1; i++) {
            data[i] = base64_chars[seed % 64];
        }
        
        // Add padding
        if (len >= 4) {
            data[len - 1] = '=';
            if (seed % 2 == 0 && len >= 2) {
                data[len - 2] = '=';
            }
        }
        
        return len;
    }
    
    // Sometimes inject invalid characters
    if (seed % 7 == 0 && size > 0) {
        size_t pos = seed % size;
        data[pos] = invalid_chars[seed % sizeof(invalid_chars)];
        return size;
    }
    
    // Sometimes create very long input to test integer overflow
    if (seed % 11 == 0) {
        size_t len = max_size;
        for (size_t i = 0; i < len; i++) {
            data[i] = base64_chars[i % 64];
        }
        return len;
    }
    
    // Use default mutator
    return 0;
}
