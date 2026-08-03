// ============================================================================
// TEST: Subsidy schedule (Fair Launch v3)
// ============================================================================
//
// Pins Dinero's emission curve: no premine, 100 DIN initial subsidy, halving
// every 1,314,000 blocks, and a perpetual 1 DIN/block tail floor with no hard
// cap.
//
// ----------------------------------------------------------------------------
// HISTORY -- WHY THIS FILE WAS REWRITTEN
// ----------------------------------------------------------------------------
// The previous version was never registered with CTest (so it was never built
// or run) and was written entirely in assert(). It therefore described a
// monetary policy Dinero no longer has, and nothing ever noticed:
//
//   1. "Hard cap: 265,428,000 DIN"      -- there is no hard cap; emission is
//                                          perpetual and disinflationary.
//   2. "Premine (height 1): 2,627,900"  -- there is no premine. Dinero launched
//                                          fair; height 1 is an ordinary PoW
//                                          block paying the full 100 DIN.
//                                          ConsensusSubsidy::PREMINE_UNA no
//                                          longer exists.
//   3. "33 halvings until subsidy 0"    -- emission never reaches zero.
//                                          TAIL_EMISSION_UNA floors it at
//                                          1 DIN/block forever, binding from
//                                          epoch 7 onward.
//
// See issue #497.
//
// ----------------------------------------------------------------------------
// NO assert() HERE
// ----------------------------------------------------------------------------
// CI builds Release and CMAKE_CXX_FLAGS_RELEASE is "-O3 -DNDEBUG", under which
// assert(x) becomes ((void)0) and its expression is never compiled. That is
// exactly how the false claims above survived. Every check below is an
// ordinary if-statement, so it gates in every build configuration.
//
// ----------------------------------------------------------------------------
// EXPECTED VALUES ARE HAND-DERIVED, NOT READ BACK FROM THE CODE
// ----------------------------------------------------------------------------
// Totals are computed by hand from the epoch structure and written as literals.
// Calling GetPoWIssuedAtHeight() to produce its own expected value would be a
// tautology that passes regardless of what the function does.
//
//   epoch n subsidy = 100 DIN >> n, floored at 1 DIN
//   epoch n spans   = 1,314,000 blocks
//   epochs 0..6     = 1,314,000 x (100+50+25+12.5+6.25+3.125+1.5625) DIN
//                   = 1,314,000 x 198.4375 DIN
//                   = 260,746,875 DIN
// ============================================================================

#include "consensus/subsidy.h"

#include <cstdint>
#include <iostream>

using dinero::ConsensusSubsidy;

namespace {

int g_failures = 0;

#define CHECK(cond)                                                           \
    do {                                                                      \
        if (!(cond)) {                                                        \
            ++g_failures;                                                     \
            std::cerr << "\n  CHECK FAILED: " #cond "\n    at " << __FILE__   \
                      << ":" << __LINE__ << "\n";                             \
        }                                                                     \
    } while (false)

#define CHECK_EQ(actual, expected)                                            \
    do {                                                                      \
        const uint64_t a_ = static_cast<uint64_t>(actual);                    \
        const uint64_t e_ = static_cast<uint64_t>(expected);                  \
        if (a_ != e_) {                                                       \
            ++g_failures;                                                     \
            std::cerr << "\n  CHECK_EQ FAILED: " #actual " == " #expected     \
                      << "\n    actual:   " << a_                             \
                      << "\n    expected: " << e_                             \
                      << "\n    at " << __FILE__ << ":" << __LINE__ << "\n";  \
        }                                                                     \
    } while (false)

constexpr uint64_t UNA = ConsensusSubsidy::UNA_PER_DIN;
constexpr uint32_t HALV = ConsensusSubsidy::HALVING_INTERVAL;
constexpr uint64_t INITIAL = ConsensusSubsidy::INITIAL_SUBSIDY;
constexpr uint64_t TAIL = ConsensusSubsidy::TAIL_EMISSION_UNA;

uint64_t SubsidyAt(uint32_t h) {
    return ConsensusSubsidy::GetBlockSubsidy(h).GetUna();
}

// ---------------------------------------------------------------------------
// 1. Fair launch: no premine
// ---------------------------------------------------------------------------
void testNoPremine() {
    std::cout << "Test: fair launch, no premine... ";

    // Genesis is an unspendable OP_RETURN burn, not a payable coinbase.
    CHECK_EQ(SubsidyAt(0), 0);

    // Height 1 is the FIRST PoW block and pays a full subsidy. A premine would
    // show up precisely as height 1 differing from height 2.
    CHECK_EQ(SubsidyAt(1), INITIAL);
    CHECK_EQ(SubsidyAt(2), INITIAL);
    CHECK_EQ(SubsidyAt(1), SubsidyAt(2));

    // The whole of epoch 0 pays the same rate.
    CHECK_EQ(SubsidyAt(HALV), INITIAL);

    std::cout << (g_failures == 0 ? "PASSED\n" : "FAILED\n");
}

// ---------------------------------------------------------------------------
// 2. Halving boundaries are exact
// ---------------------------------------------------------------------------
void testHalvingBoundaries() {
    std::cout << "Test: halving boundaries... ";

    // Epoch n covers heights [n*HALV + 1, (n+1)*HALV]. Checking the last block
    // of each epoch AND the first block of the next makes this an exact
    // boundary test: an off-by-one in the halving arithmetic breaks exactly
    // one of the two.
    for (uint32_t n = 0; n < 10; ++n) {
        const uint64_t halved = (INITIAL >> n);
        const uint64_t expected_epoch_n = (halved > TAIL) ? halved : TAIL;

        const uint64_t next_halved = (INITIAL >> (n + 1));
        const uint64_t expected_epoch_next =
            (next_halved > TAIL) ? next_halved : TAIL;

        const uint32_t last_of_epoch = (n + 1) * HALV;
        const uint32_t first_of_next = last_of_epoch + 1;

        CHECK_EQ(SubsidyAt(last_of_epoch), expected_epoch_n);
        CHECK_EQ(SubsidyAt(first_of_next), expected_epoch_next);
    }

    // Well-known early values stated explicitly, so a change to the loop above
    // cannot quietly redefine what "correct" means.
    CHECK_EQ(SubsidyAt(HALV + 1), 50 * UNA);
    CHECK_EQ(SubsidyAt(2 * HALV + 1), 25 * UNA);
    CHECK_EQ(SubsidyAt(3 * HALV + 1), 1250000000ULL);  // 12.5 DIN

    std::cout << (g_failures == 0 ? "PASSED\n" : "FAILED\n");
}

// ---------------------------------------------------------------------------
// 3. Tail emission: binds at epoch 7, never reaches zero
// ---------------------------------------------------------------------------
void testTailEmission() {
    std::cout << "Test: tail emission floor... ";

    // 100 DIN >> 6 = 1.5625 DIN, still above the floor.
    CHECK_EQ(SubsidyAt(6 * HALV + 1), 156250000ULL);

    // 100 DIN >> 7 = 0.78125 DIN, BELOW the 1 DIN floor -- epoch 7 is the first
    // epoch where the tail binds. This is the boundary the old test denied when
    // it claimed emission reaches zero.
    CHECK((INITIAL >> 7) < TAIL);
    CHECK_EQ(SubsidyAt(7 * HALV + 1), TAIL);

    // Emission never falls below the floor and never becomes zero, however far
    // out we look -- including past the "33rd halving" the old test expected to
    // zero out. Heights are kept inside uint32 range.
    for (uint32_t n : {7u, 8u, 20u, 33u, 40u, 60u}) {
        const uint64_t h64 = static_cast<uint64_t>(n) * HALV + 1;
        if (h64 > 0xFFFFFFFFULL) continue;
        const uint32_t h = static_cast<uint32_t>(h64);
        CHECK_EQ(SubsidyAt(h), TAIL);
        CHECK(SubsidyAt(h) > 0);
    }

    std::cout << (g_failures == 0 ? "PASSED\n" : "FAILED\n");
}

// ---------------------------------------------------------------------------
// 4. Cumulative issuance matches hand-derived totals
// ---------------------------------------------------------------------------
void testCumulativeIssuance() {
    std::cout << "Test: cumulative issuance... ";

    // Epoch 0 alone: 1,314,000 blocks x 100 DIN.
    CHECK_EQ(ConsensusSubsidy::GetPoWIssuedAtHeight(HALV),
             static_cast<uint64_t>(HALV) * INITIAL);

    // Through the end of epoch 6 (height 7 x 1,314,000 = 9,198,000):
    // 260,746,875 DIN, derived in the file header.
    constexpr uint32_t kEndOfEpoch6 = 7 * HALV;
    constexpr uint64_t kEpoch0Through6Din = 260746875ULL;
    CHECK_EQ(kEndOfEpoch6, 9198000u);
    CHECK_EQ(ConsensusSubsidy::GetPoWIssuedAtHeight(kEndOfEpoch6),
             kEpoch0Through6Din * UNA);

    // At the 120s target spacing there are 262,980 blocks/year, so year 35 is
    // height 9,204,300 -- 6,300 tail blocks past the epoch-6 boundary.
    constexpr uint32_t kYear35Height = 9204300u;
    CHECK_EQ(kYear35Height - kEndOfEpoch6, 6300u);
    CHECK_EQ(ConsensusSubsidy::GetPoWIssuedAtHeight(kYear35Height),
             kEpoch0Through6Din * UNA + 6300ULL * TAIL);

    // Year 100 = height 26,298,000. Everything past 9,198,000 pays 1 DIN:
    //   260,746,875 + (26,298,000 - 9,198,000) = 277,846,875 DIN
    // subsidy.h previously claimed ~346.55M here, which does not follow from a
    // 1 DIN tail. This pins the real figure so the prose cannot drift again.
    constexpr uint32_t kYear100Height = 26298000u;
    constexpr uint64_t kYear100Din = 277846875ULL;
    CHECK_EQ(kEpoch0Through6Din + (kYear100Height - kEndOfEpoch6), kYear100Din);
    CHECK_EQ(ConsensusSubsidy::GetPoWIssuedAtHeight(kYear100Height),
             kYear100Din * UNA);

    // ---- inflation rates quoted in subsidy.h ----------------------------
    // Pinned as exact integer issuance rather than a rounded percentage, so
    // there is nothing to fudge. 262,980 blocks/year at the 1 DIN floor:
    //   year 35:  262,980 / 260,753,175 = 0.1009%/yr
    //   year 100: 262,980 / 277,846,875 = 0.0947%/yr
    constexpr uint32_t kBlocksPerYear = 262980u;
    const uint64_t tail_year_at_35 =
        ConsensusSubsidy::GetPoWIssuedAtHeight(kYear35Height + kBlocksPerYear) -
        ConsensusSubsidy::GetPoWIssuedAtHeight(kYear35Height);
    CHECK_EQ(tail_year_at_35, static_cast<uint64_t>(kBlocksPerYear) * TAIL);

    const uint64_t tail_year_at_100 =
        ConsensusSubsidy::GetPoWIssuedAtHeight(kYear100Height + kBlocksPerYear) -
        ConsensusSubsidy::GetPoWIssuedAtHeight(kYear100Height);
    CHECK_EQ(tail_year_at_100, static_cast<uint64_t>(kBlocksPerYear) * TAIL);

    // The epoch-6 rate is HIGHER (1.5625 DIN/block, ~0.158%/yr) and must not be
    // mistaken for the tail rate. Measuring a year-on-year delta that straddles
    // the epoch-6/7 boundary reports this, not the tail -- which is exactly how
    // the header comment came to quote 0.156%/yr for year 35.
    constexpr uint32_t kMidEpoch6 = 6u * HALV + 100000u;
    const uint64_t epoch6_year =
        ConsensusSubsidy::GetPoWIssuedAtHeight(kMidEpoch6 + kBlocksPerYear) -
        ConsensusSubsidy::GetPoWIssuedAtHeight(kMidEpoch6);
    CHECK_EQ(epoch6_year, static_cast<uint64_t>(kBlocksPerYear) * 156250000ULL);
    CHECK(epoch6_year > tail_year_at_35);

    std::cout << (g_failures == 0 ? "PASSED\n" : "FAILED\n");
}

// ---------------------------------------------------------------------------
// 5. Total issued includes the genesis burn
// ---------------------------------------------------------------------------
void testTotalIssued() {
    std::cout << "Test: total issued... ";

    // Height 0: only the symbolic 100 DIN OP_RETURN burn.
    CHECK_EQ(ConsensusSubsidy::GetTotalIssuedAtHeight(0),
             ConsensusSubsidy::GENESIS_UNSPENDABLE_UNA);

    // Height 1: genesis burn + one full subsidy. NOT a premine.
    CHECK_EQ(ConsensusSubsidy::GetTotalIssuedAtHeight(1),
             ConsensusSubsidy::GENESIS_UNSPENDABLE_UNA + INITIAL);

    // Total = genesis burn + PoW issuance at every sampled height.
    for (uint32_t h : {0u, 1u, 2u, 1000u, HALV, HALV + 1, 7u * HALV}) {
        CHECK_EQ(ConsensusSubsidy::GetTotalIssuedAtHeight(h),
                 ConsensusSubsidy::GENESIS_UNSPENDABLE_UNA +
                     ConsensusSubsidy::GetPoWIssuedAtHeight(h));
    }

    std::cout << (g_failures == 0 ? "PASSED\n" : "FAILED\n");
}

// ---------------------------------------------------------------------------
// 6. No hard cap: disinflationary, not bounded
// ---------------------------------------------------------------------------
void testNoHardCap() {
    std::cout << "Test: no hard cap... ";

    // Deep in the tail, supply must still be strictly increasing. A hard cap
    // would show up as two distant heights reporting equal totals.
    const uint64_t a = ConsensusSubsidy::GetTotalIssuedAtHeight(20u * HALV);
    const uint64_t b = ConsensusSubsidy::GetTotalIssuedAtHeight(20u * HALV + 1);
    const uint64_t c = ConsensusSubsidy::GetTotalIssuedAtHeight(40u * HALV);
    CHECK(b > a);
    CHECK(c > b);
    CHECK_EQ(b - a, TAIL);

    // Monotonic across the early curve as well.
    uint64_t prev = 0;
    for (uint32_t h = 0; h < 2000; ++h) {
        const uint64_t cur = ConsensusSubsidy::GetTotalIssuedAtHeight(h);
        CHECK(cur >= prev);
        prev = cur;
    }

    std::cout << (g_failures == 0 ? "PASSED\n" : "FAILED\n");
}

}  // namespace

int main() {
    std::cout << "\n=== Dinero subsidy schedule (Fair Launch v3) ===\n\n";

    testNoPremine();
    testHalvingBoundaries();
    testTailEmission();
    testCumulativeIssuance();
    testTotalIssued();
    testNoHardCap();

    std::cout << "\n";
    if (g_failures != 0) {
        std::cout << "FAILED - " << g_failures << " check(s) did not hold\n\n";
        return 1;
    }
    std::cout << "PASSED - emission curve matches the documented policy\n\n";
    return 0;
}
