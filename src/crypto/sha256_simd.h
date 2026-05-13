#pragma once

#include <cstdint>
#include <cstddef>
#include <string>

namespace dinero {
namespace crypto {

/**
 * SIMD-optimized SHA-256 implementation
 * 
 * Automatically detects CPU capabilities and uses best available:
 * - SHA-NI (Intel SHA Extensions): 10-20x speedup
 * - ARM Crypto Extensions: 8-15x speedup
 * - AVX2 (8-way parallel): 4-6x speedup
 * - SSSE3 (4-way parallel): 2-3x speedup
 * - Scalar (fallback): baseline
 */

enum class SIMDLevel {
    None,      // Scalar implementation
    SSE2,      // Basic 4-way (not implemented yet)
    SSSE3,     // 4-way parallel
    AVX2,      // 8-way parallel
    SHA_NI,    // Intel SHA Extensions (hardware)
    NEON,      // ARM NEON 4-way
    ARM_SHA    // ARM SHA Extensions (hardware)
};

/**
 * Detect best available SIMD level at runtime
 */
SIMDLevel DetectBestSIMD();

/**
 * Get human-readable name for SIMD level
 */
const char* SIMDLevelName(SIMDLevel level);

/**
 * Double SHA-256 (as used in Bitcoin/Dinero mining)
 * Input: data (any length)
 * Output: 32-byte hash
 */
void DoubleSHA256_SIMD(const uint8_t* data, size_t len, uint8_t out32[32], SIMDLevel level = SIMDLevel::None);

/**
 * Single SHA-256
 */
void SHA256_SIMD(const uint8_t* data, size_t len, uint8_t out32[32], SIMDLevel level = SIMDLevel::None);

/**
 * Specialized version for mining: double SHA-256 on 80-byte block headers
 * This is the hot path - optimized for mining loops
 */
void SHA256d_BlockHeader(const uint8_t header80[80], uint8_t out32[32], SIMDLevel level = SIMDLevel::None);

/**
 * RAII wrapper for automatic SIMD detection
 */
class SIMDContext {
public:
    SIMDContext();
    
    SIMDLevel level() const { return level_; }
    const char* name() const { return SIMDLevelName(level_); }
    
    // Convenience wrappers
    void doubleSHA256(const uint8_t* data, size_t len, uint8_t out32[32]) const {
        DoubleSHA256_SIMD(data, len, out32, level_);
    }
    
    void sha256d_header(const uint8_t header80[80], uint8_t out32[32]) const {
        SHA256d_BlockHeader(header80, out32, level_);
    }

private:
    SIMDLevel level_;
};

} // namespace crypto
} // namespace dinero

