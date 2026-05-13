#ifndef DINERO_CRYPTO_SHA256_H
#define DINERO_CRYPTO_SHA256_H

#include <vector>
#include <string>
#include <cstdint>

namespace dinero {
namespace crypto {

// SHA-256 implementation for Dinero
class CSHA256 {
private:
    uint32_t state[8];
    uint8_t buffer[64];
    uint64_t length;
    int buffer_pos;
    
    static uint32_t right_rotate(uint32_t value, int shift);
    static uint32_t choice(uint32_t x, uint32_t y, uint32_t z);
    static uint32_t majority(uint32_t x, uint32_t y, uint32_t z);
    static uint32_t sigma0(uint32_t x);
    static uint32_t sigma1(uint32_t x);
    static uint32_t gamma0(uint32_t x);
    static uint32_t gamma1(uint32_t x);
    
    void transform(const uint8_t* data);
    
public:
    CSHA256();
    void reset();
    CSHA256& Write(const uint8_t* data, size_t len);
    CSHA256& Write(const std::string& data);
    void Finalize(uint8_t hash[32]);
    std::vector<uint8_t> Finalize();

    // Midstate support for mining optimization
    // Midstate is the SHA256 internal state after processing N complete 64-byte blocks
    void GetMidstate(uint32_t out_state[8]) const;
    void SetMidstate(const uint32_t in_state[8], uint64_t bytes_processed);

    // Process a single 64-byte block (for external midstate computation)
    void TransformBlock(const uint8_t block[64]);
};

// Double SHA-256 (Bitcoin style)
std::string double_sha256(const std::vector<uint8_t>& data);
std::string double_sha256(const std::string& data);
std::string double_sha256(const uint8_t* data, size_t len);

// Utility functions
std::string bytes_to_hex(const std::vector<uint8_t>& bytes);
std::vector<uint8_t> hex_to_bytes(const std::string& hex);
std::string reverse_hex(const std::string& hex);

} // namespace crypto
} // namespace dinero

#endif // DINERO_CRYPTO_SHA256_H
