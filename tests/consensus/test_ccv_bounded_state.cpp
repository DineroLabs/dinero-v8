// Bounded-state analysis for CCV successor binding.
//
// HOW THIS DIFFERS FROM THE OTHER COVENANT TESTS
// ----------------------------------------------
// The adversarial suite picks specific attacks. The fuzzer samples randomly.
// Neither enumerates. This file **exhaustively enumerates reduced domains** and
// predicts the verdict at every point from the spec, then requires production to
// agree at every point -- not merely to reject the cases someone thought of.
//
// The distinction matters: a sampling test that passes tells you the points it
// happened to visit were fine. An exhaustive test over a reduced domain tells
// you there is no surviving counterexample *inside that domain*. The strength of
// the claim then rests entirely on whether the reduction is faithful, which is
// why every reduction below states the production property it models and the
// assumption that makes it sound.
//
// WHAT A REDUCTION IS AND IS NOT
// ------------------------------
// Enumerating counters over {0,1,2,MAX-2,MAX-1,MAX} is not a proof about all
// 2^32 counters. It is a proof about those six, plus an argued claim that the
// rule depends on the counter only through `prev == MAX` and
// `next == prev + 1` -- never on magnitude. That claim is inspectable in
// VerifyContractTransition and is stated here so a reviewer can check it rather
// than take it on trust. If production ever grew a magnitude-dependent rule
// (say, a minimum counter), this reduction would silently stop being faithful.
// That is the standing risk of the technique and it is recorded deliberately.
//
// Companion artifact: docs/consensus/COVENANT_BOUNDED_STATE.md

#include "consensus/chainparams.h"
#include "consensus/covenant_activation.h"
#include "consensus/covenants.h"
#include "consensus/script.h"
#include "consensus/script_interpreter.h"
#include "primitives/transaction.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace {

using namespace dinero;
using namespace dinero::consensus;

constexpr uint32_t kU32Max = std::numeric_limits<uint32_t>::max();

// A CCV scenario whose commitments are always internally consistent, so the
// only variable is the dimension under enumeration.
struct Bench {
    std::vector<uint8_t> script{static_cast<uint8_t>(OP_CHECKCONTRACTVERIFY),
                                static_cast<uint8_t>(OP_TRUE)};
    std::array<uint8_t, 32> merkle_root{};
    ContractState previous;
    ContractState next;
    std::array<uint8_t, 32> internal_key{};
    uint8_t parity = 0;
    Transaction tx;
    std::vector<UTXOEntry> spent;
    uint32_t index = 0;
    uint64_t value = 250'000;

    Bench() {
        for (size_t i = 0; i < merkle_root.size(); ++i) {
            merkle_root[i] = static_cast<uint8_t>(i + 1);
        }
        previous.codeHash = ComputeContractCodeHash(script);
        previous.counter = 41;
        previous.data = {0x10};
        next.codeHash = previous.codeHash;
        next.counter = 42;
        next.data = {0x40};
    }

    // Recompute every derived commitment. Callers mutate only the dimension
    // under test and then rebuild, so a point can never fail for an unrelated
    // reason such as a stale script.
    bool Rebuild(size_t output_count, uint32_t successor_index) {
        previous.stateHash = ComputeContractStateHash(previous);
        next.stateHash = ComputeContractStateHash(next);
        if (!DeriveContractInternalKey(previous, internal_key)) {
            return false;
        }
        std::vector<uint8_t> spent_script;
        if (!ComputeContractOutputScript(
                previous, merkle_root, spent_script, &parity)) {
            return false;
        }
        std::vector<uint8_t> successor_script;
        if (!ComputeContractOutputScript(
                next, merkle_root, successor_script)) {
            return false;
        }

        tx = Transaction{};
        for (size_t i = 0; i <= index; ++i) {
            tx.vin.emplace_back();
        }
        for (size_t i = 0; i < output_count; ++i) {
            // Filler outputs are distinct from the successor script.
            tx.vout.emplace_back(
                AmountUna::Una(value),
                std::vector<uint8_t>{static_cast<uint8_t>(OP_TRUE),
                                     static_cast<uint8_t>(i)});
        }
        if (successor_index < output_count) {
            tx.vout[successor_index] =
                TxOutput(AmountUna::Una(value), successor_script);
        }

        spent.clear();
        for (size_t i = 0; i < tx.vin.size(); ++i) {
            spent.emplace_back(AmountUna::Una(value), spent_script, 100, false);
        }
        return true;
    }

    bool Verify() const {
        const ContractSpendContext context{
            spent, script, internal_key, merkle_root, parity};
        return VerifyContractTransition(tx, index, previous, next, context);
    }
};

// ===========================================================================
// Dimension 1 -- counter transitions
// ===========================================================================
//
// PRODUCTION PROPERTY MODELLED
//   prevState.counter != UINT32_MAX && newState.counter == prevState.counter + 1
//
// REDUCTION
//   The full domain is 2^32 x 2^32 pairs. Enumerated here: all 36 ordered pairs
//   drawn from {0, 1, 2, MAX-2, MAX-1, MAX}.
//
// SOUNDNESS ASSUMPTION (inspectable in VerifyContractTransition)
//   The rule reads the counter only through the equality `prev == MAX` and the
//   successor relation `next == prev + 1`. It never compares magnitudes, so
//   behaviour is uniform across the interior of the range and can only change at
//   the wrap boundary. The chosen points cover both ends plus interior values,
//   including the pair (MAX, 0) that a wrap would produce.
TEST(CcvBoundedState, EnumeratesCounterTransitionsExhaustively) {
    constexpr uint32_t kCounters[] = {0, 1, 2, kU32Max - 2, kU32Max - 1, kU32Max};

    int accepted = 0;
    int rejected = 0;
    for (const uint32_t prev : kCounters) {
        for (const uint32_t next : kCounters) {
            Bench bench;
            bench.previous.counter = prev;
            bench.next.counter = next;
            ASSERT_TRUE(bench.Rebuild(1, 0));

            // Predicted from the spec, not observed from production.
            const bool expected =
                (prev != kU32Max) && (next == prev + 1);

            SCOPED_TRACE("prev=" + std::to_string(prev) +
                         " next=" + std::to_string(next));
            EXPECT_EQ(bench.Verify(), expected);
            expected ? ++accepted : ++rejected;
        }
    }
    // The domain must contain both outcomes, or "production agreed everywhere"
    // would be satisfied by a verifier that always answers the same way.
    EXPECT_EQ(accepted + rejected, 36);
    EXPECT_GT(accepted, 0);
    EXPECT_GT(rejected, 0);
}

// ===========================================================================
// Dimension 2 -- state data sizes
// ===========================================================================
//
// PRODUCTION PROPERTY MODELLED
//   prevState.data.size() <= MAX_CONTRACT_STATE_DATA_SIZE &&
//   newState.data.size() <= MAX_CONTRACT_STATE_DATA_SIZE
//
// REDUCTION
//   All 49 ordered pairs from {0, 1, 2, 446, 447, 448, 449} for (prev, next).
//
// SOUNDNESS ASSUMPTION
//   The bound is a single `>` comparison against one constant, so behaviour can
//   only change at 448/449. Interior sizes are included to confirm the rule is
//   not accidentally size-sensitive elsewhere, and both sides of the boundary
//   are enumerated in both positions -- an off-by-one that rejected exactly 448
//   would break every legitimate full-size contract and is caught here.
TEST(CcvBoundedState, EnumeratesStateDataSizesExhaustively) {
    constexpr size_t kMax = MAX_CONTRACT_STATE_DATA_SIZE;
    const size_t sizes[] = {0, 1, 2, kMax - 2, kMax - 1, kMax, kMax + 1};

    int accepted = 0;
    int rejected = 0;
    for (const size_t prev_size : sizes) {
        for (const size_t next_size : sizes) {
            Bench bench;
            bench.previous.data.assign(prev_size, 0x11);
            bench.next.data.assign(next_size, 0x22);
            ASSERT_TRUE(bench.Rebuild(1, 0));

            const bool expected = prev_size <= kMax && next_size <= kMax;

            SCOPED_TRACE("prev_size=" + std::to_string(prev_size) +
                         " next_size=" + std::to_string(next_size));
            EXPECT_EQ(bench.Verify(), expected);
            expected ? ++accepted : ++rejected;
        }
    }
    EXPECT_EQ(accepted + rejected, 49);
    EXPECT_GT(accepted, 0);
    EXPECT_GT(rejected, 0);
}

// ===========================================================================
// Dimension 3 -- successor placement across output vectors
// ===========================================================================
//
// PRODUCTION PROPERTY MODELLED
//   tx.vout[inputIndex] is the successor, and no OTHER output carries the same
//   successor script.
//
// REDUCTION
//   Output vectors of length 1..4, CCV input index 0..3, successor placed at
//   every position 0..output_count inclusive, where position == output_count
//   encodes "successor absent". The number of placements therefore varies with
//   the vector length, giving 4 * (2+3+4+5) = 56 points. The count is derived
//   below rather than hardcoded, so it cannot silently drift from the loops.
//
// SOUNDNESS ASSUMPTION
//   The index rule is a direct subscript comparison and the uniqueness rule is a
//   linear scan, so both are structural in the output vector rather than
//   dependent on its absolute length. Small vectors therefore exercise the same
//   code paths a large one would, and lengths beyond 4 add no new branches.
TEST(CcvBoundedState, EnumeratesSuccessorPlacementExhaustively) {
    int accepted = 0;
    int rejected = 0;
    int points = 0;
    int expected_points = 0;
    for (size_t output_count = 1; output_count <= 4; ++output_count) {
        expected_points += 4 * static_cast<int>(output_count + 1);
    }

    for (size_t output_count = 1; output_count <= 4; ++output_count) {
        for (uint32_t ccv_index = 0; ccv_index < 4; ++ccv_index) {
            for (uint32_t successor_index = 0;
                 successor_index <= output_count; ++successor_index) {
                Bench bench;
                bench.index = ccv_index;
                ASSERT_TRUE(bench.Rebuild(output_count, successor_index));
                ++points;

                // Accept only when the index exists and the successor sits
                // exactly there. Placement anywhere else is either a missing
                // successor or a misindexed one.
                const bool index_exists = ccv_index < output_count;
                const bool expected =
                    index_exists && successor_index == ccv_index;

                SCOPED_TRACE("outputs=" + std::to_string(output_count) +
                             " ccv_index=" + std::to_string(ccv_index) +
                             " successor_at=" + std::to_string(successor_index));
                EXPECT_EQ(bench.Verify(), expected);
                expected ? ++accepted : ++rejected;
            }
        }
    }
    EXPECT_EQ(points, expected_points);
    EXPECT_GT(accepted, 0);
    EXPECT_GT(rejected, 0);
}

// ===========================================================================
// Dimension 4 -- duplicate successors
// ===========================================================================
//
// PRODUCTION PROPERTY MODELLED
//   No output other than tx.vout[inputIndex] may carry the successor script.
//
// REDUCTION
//   Output vectors of length 2..5 with the successor at index 0 and a duplicate
//   planted at every other position. All combinations enumerated.
//
// SOUNDNESS ASSUMPTION
//   Detection is a full linear scan over the output vector, so it cannot be
//   position-sensitive; enumerating every position within short vectors covers
//   the adjacent case and the distant case alike.
TEST(CcvBoundedState, EnumeratesDuplicateSuccessorPositionsExhaustively) {
    int points = 0;
    for (size_t output_count = 2; output_count <= 5; ++output_count) {
        for (size_t duplicate_at = 1; duplicate_at < output_count;
             ++duplicate_at) {
            Bench bench;
            ASSERT_TRUE(bench.Rebuild(output_count, 0));
            bench.tx.vout[duplicate_at] = bench.tx.vout[0];
            ++points;

            SCOPED_TRACE("outputs=" + std::to_string(output_count) +
                         " duplicate_at=" + std::to_string(duplicate_at));
            // A duplicate anywhere makes the lineage ambiguous: always reject.
            EXPECT_FALSE(bench.Verify());
        }
    }
    EXPECT_EQ(points, 1 + 2 + 3 + 4);
}

// ===========================================================================
// Dimension 5 -- confidentiality flags
// ===========================================================================
//
// PRODUCTION PROPERTY MODELLED
//   The spent output and the successor must both be transparent. V1 rejects
//   confidential CCV because equality of Pedersen commitments is not a proof of
//   value equality when blinders may differ.
//
// REDUCTION
//   The complete 2 x 2 cross-product of (spent confidential, successor
//   confidential). This dimension is genuinely exhaustive, not reduced.
TEST(CcvBoundedState, EnumeratesConfidentialityCombinationsExhaustively) {
    int points = 0;
    for (const bool spent_confidential : {false, true}) {
        for (const bool successor_confidential : {false, true}) {
            Bench bench;
            ASSERT_TRUE(bench.Rebuild(1, 0));
            bench.spent[0].is_confidential = spent_confidential;
            bench.tx.vout[0].is_confidential = successor_confidential;
            ++points;

            const bool expected =
                !spent_confidential && !successor_confidential;

            SCOPED_TRACE(std::string("spent_conf=") +
                         (spent_confidential ? "1" : "0") +
                         " successor_conf=" +
                         (successor_confidential ? "1" : "0"));
            EXPECT_EQ(bench.Verify(), expected);
        }
    }
    EXPECT_EQ(points, 4);
}

// ===========================================================================
// Dimension 6 -- value relation
// ===========================================================================
//
// PRODUCTION PROPERTY MODELLED
//   The successor preserves the spent value EXACTLY. A CCV coin cannot pay its
//   own fee; fees must come from a separate input.
//
// REDUCTION
//   Successor values at {spent-2, spent-1, spent, spent+1, spent+2} and the
//   degenerate 0. Exactly one point may be accepted.
//
// SOUNDNESS ASSUMPTION
//   The rule is a single equality comparison, so it cannot be sensitive to the
//   magnitude of the difference -- only to whether one exists. Neighbours on
//   both sides plus zero therefore cover it.
TEST(CcvBoundedState, EnumeratesValueRelationExhaustively) {
    constexpr uint64_t kSpent = 250'000;
    const uint64_t values[] = {0, kSpent - 2, kSpent - 1, kSpent,
                               kSpent + 1, kSpent + 2};

    int accepted = 0;
    for (const uint64_t successor_value : values) {
        Bench bench;
        ASSERT_TRUE(bench.Rebuild(1, 0));
        bench.tx.vout[0].value = AmountUna::Una(successor_value);

        const bool expected = successor_value == kSpent;
        SCOPED_TRACE("successor_value=" + std::to_string(successor_value));
        EXPECT_EQ(bench.Verify(), expected);
        if (expected) {
            ++accepted;
        }
    }
    // Exactly one value in the domain preserves the amount.
    EXPECT_EQ(accepted, 1);
}

}  // namespace
