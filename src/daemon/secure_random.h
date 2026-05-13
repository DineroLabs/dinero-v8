#pragma once

#include <cstdint>
#include <cstddef>

// Cross-platform cryptographically secure random number generator
// No placeholders - uses OS CSPRNG on each platform

class SecureRandom {
public:
    // Generate cryptographically secure random bytes
    // Returns true on success, false on failure
    static bool GetBytes(uint8_t* output, size_t length);
    
    // Generate a random 32-bit integer
    static uint32_t GetUInt32();
    
    // Generate a random 64-bit integer  
    static uint64_t GetUInt64();
    
    // Generate random secp256k1 private key (32 bytes)
    static bool GetPrivateKey(uint8_t private_key[32]);
    
private:
    // Platform-specific implementations
#ifdef __APPLE__
    static bool GetBytes_macOS(uint8_t* output, size_t length);
#elif defined(_WIN32)
    static bool GetBytes_Windows(uint8_t* output, size_t length);
#else
    static bool GetBytes_Linux(uint8_t* output, size_t length);
#endif
};
