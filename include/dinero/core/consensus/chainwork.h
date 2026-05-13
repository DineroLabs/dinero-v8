#pragma once

#include <string>
#include <cstdint>
#include <array>

namespace dinero {

/**
 * Simple 256-bit arithmetic integer class for chainwork calculations
 * This is a minimal implementation focused on chainwork operations
 */
class arith_uint256 {
private:
    std::array<uint64_t, 4> data; // 4 * 64 bits = 256 bits, little-endian
    
public:
    arith_uint256() : data{0, 0, 0, 0} {}
    
    explicit arith_uint256(uint64_t value) : data{value, 0, 0, 0} {}
    
    // Set from compact difficulty representation
    void SetCompact(uint32_t compact);
    
    // Get compact difficulty representation
    uint32_t GetCompact() const;
    
    // Arithmetic operations
    arith_uint256 operator+(const arith_uint256& other) const;
    arith_uint256 operator/(const arith_uint256& other) const;
    arith_uint256& operator+=(const arith_uint256& other);
    arith_uint256& operator*=(uint64_t multiplier);
    arith_uint256& operator<<=(unsigned int shift);
    arith_uint256& operator>>=(unsigned int shift);
    
    // Comparison operators
    bool operator==(const arith_uint256& other) const;
    bool operator!=(const arith_uint256& other) const { return !(*this == other); }
    bool operator<(const arith_uint256& other) const;
    bool operator>(const arith_uint256& other) const;
    bool operator<=(const arith_uint256& other) const { return *this < other || *this == other; }
    bool operator>=(const arith_uint256& other) const { return *this > other || *this == other; }
    
    // Conversion
    std::string GetHex() const;
    bool IsZero() const;
    
    // Static constants
    static arith_uint256 Max(); // 2^256 - 1
    static arith_uint256 One() { return arith_uint256(1); }
    static arith_uint256 Zero() { return arith_uint256(); }
    
    // Access to internal data for conversion functions
    void SetWord(int index, uint64_t value) { data[index] = value; }
    uint64_t GetWord(int index) const { return data[index]; }
    
private:
    // Internal helpers
    bool IsLessThan(const arith_uint256& other) const;
    arith_uint256 DivideBy(const arith_uint256& divisor) const;
};

/**
 * Calculate work for a single block from its difficulty bits
 * Work = 2^256 / (target + 1)
 */
arith_uint256 GetBlockProof(uint32_t nBits);

/**
 * Convert arith_uint256 to hex string for storage/comparison
 */
std::string ChainworkToHex(const arith_uint256& chainwork);

/**
 * Convert hex string back to arith_uint256
 */
arith_uint256 ChainworkFromHex(const std::string& hex);

/**
 * Compare two chainwork hex strings
 * Returns: -1 if a < b, 0 if a == b, 1 if a > b
 */
int CompareChainwork(const std::string& work_a, const std::string& work_b);

} // namespace dinero
