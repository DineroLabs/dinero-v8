#ifndef DINERO_SHA256D_H
#define DINERO_SHA256D_H

#include <vector>
#include <string>
#include <cstdint>

namespace Dinero {
namespace Common {

// sha256 constants (Bitcoin Core style)
extern const uint32_t SHA256_K[64];

// sha256 implementation
class sha256 {
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
    sha256();
    void reset();
    void update(const uint8_t* data, size_t len);
    std::vector<uint8_t> finalize();
};

// Double sha256 returning raw bytes (for signature hashes)
std::vector<uint8_t> double_sha256_raw(const std::vector<uint8_t>& data);
std::vector<uint8_t> double_sha256_raw(const uint8_t* data, size_t len);

// Double sha256 function (Bitcoin Core style - reversed for txid display)
std::string double_sha256(const std::vector<uint8_t>& data);
std::string double_sha256(const std::string& data);
std::string double_sha256(const uint8_t* data, size_t len);

// Utility functions
std::string bytes_to_hex(const std::vector<uint8_t>& bytes);
std::vector<uint8_t> hex_to_bytes(const std::string& hex);
std::string reverse_hex(const std::string& hex);

} // namespace Common
} // namespace Dinero

#endif // DINERO_SHA256D_H 