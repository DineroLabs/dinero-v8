// Regression test for the ASERT fixed-point exponent on the fast-block branch.
//
// CalculateASERT_Target() computes `(excessTime << 16) / halfLife`, where
// excessTime is NEGATIVE when blocks arrive faster than target. Left-shifting a
// negative signed value is undefined behavior before C++20 (the iOS/NodeCore
// toolchain builds this consensus header with -std=c++17). The fix uses a signed
// multiply instead. This test exercises that negative-excessTime path and pins
// its observable behavior:
//   - a fast block (excessTime < 0) must INCREASE difficulty  (target decreases)
//   - a slow block (excessTime > 0) must DECREASE difficulty  (target increases)
//   - the result is deterministic.
// A broken sign path (e.g. garbage from UB) fails the direction checks.
//
// Header-only (CalculateASERT_Target / TargetFromBitsBE are inline). Gates on the
// process exit code, not assert().
#include "consensus/pow_asert.hpp"

#include <array>
#include <cstdint>
#include <cstdio>

int main() {
    Consensus c;  // defaults: targetSpacingSec=120, asertHalfLifeSec=43200
    const uint32_t anchorHeight = 0;
    const int64_t  anchorTime   = 1'000'000;
    // Anchor HARDER than the pow limit (0x1d31ffce) so a slow block has room to
    // ease off without clamping (exponent 0x1c => 256x harder target).
    const uint32_t anchorBits   = 0x1c31ffce;
    const uint32_t N            = 1000;  // blocks past the anchor
    const int64_t  idealTime    = anchorTime + static_cast<int64_t>(N) * c.targetSpacingSec;

    // Fast: 60000s ahead of ideal over N blocks -> excessTime < 0 (the UB path).
    const uint32_t fast_bits = dinero::CalculateASERT_Target(
        anchorHeight + N, idealTime - 60'000, anchorHeight, anchorTime, anchorBits, c);
    // Slow: 60000s behind ideal -> excessTime > 0.
    const uint32_t slow_bits = dinero::CalculateASERT_Target(
        anchorHeight + N, idealTime + 60'000, anchorHeight, anchorTime, anchorBits, c);

    // Determinism on the negative-excessTime path.
    const uint32_t fast_bits_again = dinero::CalculateASERT_Target(
        anchorHeight + N, idealTime - 60'000, anchorHeight, anchorTime, anchorBits, c);
    if (fast_bits_again != fast_bits) {
        std::fprintf(stderr, "FAIL: CalculateASERT_Target is non-deterministic on the fast path\n");
        return 1;
    }

    // Decode compact bits to big-endian 32-byte targets; std::array<>::operator<
    // is lexicographic == numeric ordering for big-endian. Smaller target = harder.
    const std::array<uint8_t, 32> anchorT = dinero::TargetFromBitsBE(anchorBits);
    const std::array<uint8_t, 32> fastT   = dinero::TargetFromBitsBE(fast_bits);
    const std::array<uint8_t, 32> slowT   = dinero::TargetFromBitsBE(slow_bits);

    if (!(fastT < anchorT)) {
        std::fprintf(stderr, "FAIL: fast block (negative excessTime) did not increase difficulty\n");
        return 1;
    }
    if (!(slowT > anchorT)) {
        std::fprintf(stderr, "FAIL: slow block (positive excessTime) did not decrease difficulty\n");
        return 1;
    }

    std::printf("PASS: ASERT fast(neg excessTime)=harder, slow=easier, deterministic\n");
    return 0;
}
