#pragma once
#include <cstdint>
#include <cmath>
#include <cassert>
#include <limits>
#include <iostream>

// ===============================
// Compact <-> Difficulty Helpers
// ===============================
//
// Definitions relative to Bitcoin's "difficulty 1" reference:
//   nBits(D=1) = 0x1d00ffff
//   D = D1_target / target
//
// Where compact nBits encodes target as:
//   target = mant * 256^(exp - 3)
//   nBits = (exp << 24) | mant
//
// Notes:
// - We treat mantissa as unsigned 23-bit magnitude (top bit must not be set).
// - For negative/overflow compacts (Bitcoin allows), this helper assumes valid, non-negative targets.
// - This is ideal for alt/test chains where you want human-friendly difficulty settings.

// --- Decode compact (nBits) -> difficulty (double)
inline double DifficultyFromCompact(uint32_t nBits) {
    uint32_t exp  = nBits >> 24;
    uint32_t mant = nBits & 0x007fffff;          // magnitude (top bit clear)
    if (mant == 0) return std::numeric_limits<double>::infinity(); // zero target -> infinite difficulty

    // D = (65535 / mant) * 256^(0x1d - exp)
    // Use long double to keep precision for very small/large D, then cast.
    long double ratioMant = 65535.0L / static_cast<long double>(mant);
    int expDiff = static_cast<int>(0x1d) - static_cast<int>(exp);
    long double pow256 = std::pow(256.0L, static_cast<long double>(expDiff));
    long double D = ratioMant * pow256;
    return static_cast<double>(D);
}

// --- Encode difficulty (double) -> compact (nBits)
inline uint32_t CompactFromDifficulty(double D) {
    // Guardrails
    if (!(D > 0.0) || !std::isfinite(D)) {
        // Invalid difficulty: return "infinite target" sentinel (very easy)
        return 0x207fffff; // extremely easy; also useful as a diagnostic
    }

    // We want target = D1_target / D = (65535 * 256^(0x1d - 3)) / D
    // If we start with exp = 0x1d, then mant ~= 65535 / D.
    long double mant = std::floor((65535.0L / static_cast<long double>(D)) + 0.5L);
    uint32_t exp = 0x1d;

    // Normalize so that mant fits in 3 bytes and top bit (0x00800000) is NOT set.
    while (mant >= 0x00800000LL) {
        mant = std::floor((mant + 0xFF) / 256.0L); // round while shifting
        ++exp;
    }
    // If mant got too small (rare for typical D), shift left and reduce exponent.
    while (mant > 0 && mant < 0x000080) {
        mant *= 256.0L;
        if (exp == 0) break;
        --exp;
    }

    uint32_t mant_u = static_cast<uint32_t>(mant);
    if (mant_u >= 0x00800000U) { // safety (shouldn't happen after loop)
        mant_u >>= 8;
        ++exp;
    }

    // Pack. (Sign bit is 0 for valid positive targets)
    uint32_t nBits = (exp << 24) | (mant_u & 0x007fffff);
    return nBits;
}

// ===============================
// Tiny self-test (enable in one TU)
// ===============================
#ifdef DIFFBITS_TEST_MAIN
int main() {
    auto near = [](double a, double b, double rel=1e-8){ return std::fabs(a-b) <= rel*std::max({1.0, std::fabs(a), std::fabs(b)}); };

    // 1) Baseline: 0x1d00ffff -> D ≈ 1
    {
        uint32_t n1 = 0x1d00ffff;
        double d1 = DifficultyFromCompact(n1);
        std::cout << "D(0x1d00ffff) = " << d1 << "\n";
        assert(near(d1, 1.0, 1e-12));
        uint32_t r1 = CompactFromDifficulty(1.0);
        std::cout << "nBits(D=1) = 0x" << std::hex << r1 << std::dec << "\n";
        // Allow off-by-one mant rounding; it should round-trip "near" 0x1d00ffff.
        assert((r1 >> 24) == 0x1d);
    }

    // 2) Easier by 256x: 0x1e00ffff -> D ≈ 1/256 ≈ 0.00390625
    {
        uint32_t n = 0x1e00ffff;
        double d = DifficultyFromCompact(n);
        std::cout << "D(0x1e00ffff) = " << d << "\n";
        assert(near(d, 1.0/256.0, 1e-12));
    }

    // 3) Very easy target used in tests: ~D = 0.0001
    {
        uint32_t n = 0x1f002710; // from hand calc (≈0.0001)
        double d = DifficultyFromCompact(n);
        std::cout << "D(0x1f002710) = " << d << "\n";
        assert(near(d, 0.0001, 5e-5)); // a little tolerance due to compact rounding

        uint32_t back = CompactFromDifficulty(0.0001);
        std::cout << "nBits(D=1e-4) = 0x" << std::hex << back << std::dec << "\n";
        assert((back >> 24) == 0x1f); // exponent should be 0x1f for this vicinity
    }

    // 4) Your mis-encoded example: 0x1f000001 should be ~D≈1 (NOT 1.1e12)
    {
        uint32_t n = 0x1f000001;
        double d = DifficultyFromCompact(n);
        std::cout << "D(0x1f000001) = " << d << "\n";
        // Expect ~ (65535/1) * 256^(0x1d-0x1f) = 65535 / 65536 ≈ 0.99998474
        assert(near(d, 65535.0/65536.0, 1e-12));
    }

    // 5) Ridiculously easy: 0x2100ffff (should be ~2^-32)
    {
        uint32_t n = 0x2100ffff;
        double d = DifficultyFromCompact(n);
        std::cout << "D(0x2100ffff) = " << d << "\n";
        // 2^(0x1d - 0x21) = 2^(-4*8) = 2^-32
        assert(near(d, std::ldexp(1.0, -32))); // = 1 / 2^32
    }

    std::cout << "All tests passed.\n";
    return 0;
}
#endif
