#pragma once
#include <cstdint>
#include <cmath>
#include <limits>
#include <vector>

// -------------------------------------------------------------
// Compact <-> Difficulty Helpers (Simplified for Dinero)
// -------------------------------------------------------------
//  nBits = (exp<<24) | mant (with mant in [0x000001, 0x007fffff], sign bit clear).
//  target = mant * 256^(exp - 3).
//
//  These provide the same functionality as Bitcoin Core's helpers
//  but work with standard C++ types.
// -------------------------------------------------------------

// Simple target representation (256-bit as bytes)
struct SimpleTarget {
    std::vector<uint8_t> data;
    
    SimpleTarget() : data(32, 0) {}
    SimpleTarget(uint64_t value) : data(32, 0) {
        for (int i = 0; i < 8 && i < 32; ++i) {
            data[31 - i] = (value >> (i * 8)) & 0xFF;
        }
    }
    
    // Get compact representation
    uint32_t GetCompact() const {
        if (data.empty()) return 0;
        
        // Find first non-zero byte
        size_t first_nonzero = 0;
        while (first_nonzero < data.size() && data[first_nonzero] == 0) {
            first_nonzero++;
        }
        
        if (first_nonzero == data.size()) return 0; // All zeros
        
        size_t size = data.size() - first_nonzero;
        if (size > 3) size = 3;
        
        uint32_t mantissa = 0;
        for (size_t i = 0; i < size; ++i) {
            mantissa = (mantissa << 8) | data[first_nonzero + i];
        }
        
        return (static_cast<uint32_t>(size) << 24) | mantissa;
    }
    
    // Set from compact
    void SetCompact(uint32_t nBits) {
        data.assign(32, 0);

        uint32_t size = nBits >> 24;
        uint32_t mantissa = nBits & 0x00ffffff;
        
        if (size <= 3) {
            // Shift right
            mantissa >>= 8 * (3 - size);
            data[31] = mantissa & 0xFF;
            if (size > 1) data[30] = (mantissa >> 8) & 0xFF;
            if (size > 2) data[29] = (mantissa >> 16) & 0xFF;
        } else {
            // Shift left
            size_t start_pos = 32 - size;
            data[start_pos] = mantissa & 0xFF;
            data[start_pos + 1] = (mantissa >> 8) & 0xFF;
            data[start_pos + 2] = (mantissa >> 16) & 0xFF;
        }
    }
};

// -------------------------------------------------------------
// Compact <-> Target (Simplified)
// -------------------------------------------------------------

inline SimpleTarget TargetFromCompact(uint32_t nBits, bool* pfNegative=nullptr, bool* pfOverflow=nullptr)
{
    if (pfNegative) *pfNegative = (nBits & 0x00800000) != 0;
    if (pfOverflow) *pfOverflow = false;
    
    SimpleTarget target;
    target.SetCompact(nBits);
    return target;
}

inline uint32_t CompactFromTarget(const SimpleTarget& target)
{
    return target.GetCompact();
}

// -------------------------------------------------------------
// Difficulty <-> Compact
// -------------------------------------------------------------
// Difficulty is defined relative to Bitcoin's D=1 reference:
//   D = D1_target / target
// where D1_target corresponds to nBits=0x1d00ffff.
//
// Using compact algebra (no 256-bit division needed):
//   If nBits=(exp<<24)|mant,   target = mant * 256^(exp-3)
//   Let D1 have exp=0x1d, mant=0x00ffff (=65535).
//   Then: D = (65535 / mant) * 256^(0x1d - exp)
// -------------------------------------------------------------

inline double DifficultyFromCompact(uint32_t nBits)
{
    uint32_t exp  = nBits >> 24;
    uint32_t mant = nBits & 0x00ffffff;
    if (mant == 0) return std::numeric_limits<double>::infinity(); // zero target => infinite difficulty

    // D = (65535 / mant) * 256^(0x1d - exp)
    // Use long double to keep precision for very small/large D, then cast.
    long double ratioMant = 65535.0L / static_cast<long double>(mant);
    int expDiff = static_cast<int>(0x1d) - static_cast<int>(exp);
    long double pow256 = std::pow(256.0L, static_cast<long double>(expDiff));
    long double D = ratioMant * pow256;
    return static_cast<double>(D);
}

inline uint32_t CompactFromDifficulty(double D)
{
    // Guardrails
    if (!(D > 0.0) || !std::isfinite(D)) {
        // Extremely easy sentinel (debug-friendly)
        return 0x207fffff;
    }

    // Start from D1 frame (exp=0x1d): target = (65535 * 256^(0x1d-3)) / D
    // => initial mant ≈ 65535 / D (with exp=0x1d), then normalize.
    long double mant = std::floor((65535.0L / static_cast<long double>(D)) + 0.5L);
    uint32_t exp = 0x1d;

    // Normalize so mant fits 3 bytes and top bit is NOT set.
    while (mant >= 0x00800000LL) { mant = std::floor((mant + 0xFF) / 256.0L); ++exp; }
    while (mant > 0 && mant < 0x000080) { mant *= 256.0L; if (exp == 0) break; --exp; }

    uint32_t mant_u = static_cast<uint32_t>(mant);
    if (mant_u >= 0x00800000U) { mant_u >>= 8; ++exp; } // safety

    return (exp << 24) | (mant_u & 0x00ffffff);
}

// -------------------------------------------------------------
// Convenience: Desired block time -> nBits
// -------------------------------------------------------------
// For a miner (or network) with hashrate H (hashes/second), the
// expected time per block is:
//     E[T] = (D * 2^32) / H
// So for a target time T, desired difficulty is:
//     D = (H * T) / 2^32
// Then encode to compact with CompactFromDifficulty(D).
// -------------------------------------------------------------

inline uint32_t CompactForDesiredBlockTime(double hashrate_hps, double target_seconds)
{
    const long double two32 = 4294967296.0L; // 2^32
    if (!(hashrate_hps > 0.0) || !(target_seconds > 0.0)) return 0x207fffff;
    long double D = (static_cast<long double>(hashrate_hps) * static_cast<long double>(target_seconds)) / two32;
    return CompactFromDifficulty(static_cast<double>(D));
}

// -------------------------------------------------------------
// Optional: Build target directly from desired time/hashrate
// -------------------------------------------------------------

inline SimpleTarget TargetForDesiredBlockTime(double hashrate_hps, double target_seconds)
{
    uint32_t nBits = CompactForDesiredBlockTime(hashrate_hps, target_seconds);
    return TargetFromCompact(nBits);
}
