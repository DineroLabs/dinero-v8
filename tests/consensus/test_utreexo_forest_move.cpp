// Copyright (c) 2026 Dinero Labs.
//
// UtreexoForest must be movable.
//
// It user-declares an EMPTY destructor (`~UtreexoForest() {}`), which under the
// Rule of Five suppresses the implicit move constructor and move assignment.
// The class then silently falls back to the COPY constructor whenever an
// rvalue is bound — so `std::move(forest)` deep-copies a structure holding
// vectors, an unordered_map of every leaf position, and a deleted-position set.
//
// `std::is_move_constructible` does NOT catch this: it is satisfied by the copy
// constructor, since a const-lvalue-ref binds to an rvalue. The discriminator
// is NOTHROW: a defaulted move over RAII members is noexcept(true), while the
// copy constructor allocates and is noexcept(false).
//
// This matters because ConnectBlock deep-copies the forest per block
// (block_validation.cpp 2102 / 2133 / 342). A profile of a live mainnet
// AssumeUTXO replay showed 19.4% in ~UtreexoForest(), 16.4% in the copy
// constructor and ~45% in malloc/free, with throughput decaying 27 -> 4.1 ->
// 1.8 blocks/min as the forest grew. Any fix that hands ownership around is
// worthless while every move is secretly a copy.

#include <type_traits>

#include "consensus/utreexo_accumulator.h"

#include <gtest/gtest.h>

using dinero::consensus::UtreexoForest;

TEST(UtreexoForestMove, IsNothrowMovable) {
    // The real assertion. Fails while the implicit moves are suppressed.
    static_assert(std::is_nothrow_move_constructible_v<UtreexoForest>,
                  "UtreexoForest has no move constructor — std::move() copies it");
    static_assert(std::is_nothrow_move_assignable_v<UtreexoForest>,
                  "UtreexoForest has no move assignment — moves copy it");
    SUCCEED();
}

TEST(UtreexoForestMove, CopyingIsStillAvailableAndExplicitViaClone) {
    // Moving must not come at the cost of copy support: clone() and the copy
    // constructor are used deliberately in the validation paths.
    static_assert(std::is_copy_constructible_v<UtreexoForest>,
                  "copying must remain available");
    SUCCEED();
}

// A moved-from forest must be destructible and reusable-after-assignment —
// the states the validation paths actually put it through.
TEST(UtreexoForestMove, MovedFromIsSafeToDestroyAndReassign) {
    UtreexoForest a;
    a.add(dinero::consensus::UtreexoHash{});
    const uint64_t leaves = a.getNumLeaves();

    UtreexoForest b = std::move(a);
    EXPECT_EQ(b.getNumLeaves(), leaves) << "move must preserve contents";

    a = UtreexoForest{};              // reassign the moved-from object
    EXPECT_EQ(a.getNumLeaves(), 0u);  // and it behaves as a fresh forest
}
