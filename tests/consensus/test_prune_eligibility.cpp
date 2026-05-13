// SPDX-License-Identifier: MIT
// Phase P.1 — Prune Eligibility Tests (Unit Tests)
//
// These tests validate CBlockIndex prune eligibility flag semantics.
// They do NOT test ChainManager integration (which requires Mempool).
// Integration tests will be added when Phase M.1 (Mempool) is implemented.

#include <gtest/gtest.h>
#include "consensus/block_index.h"
#include "consensus/block_lifecycle.h"
#include <memory>

namespace dinero {
namespace test {

/**
 * Phase P.1 — Prune Eligibility Unit Tests
 *
 * These tests validate that the BLOCK_PRUNE_ELIGIBLE flag:
 *   1. Can be set and cleared correctly
 *   2. Preserves other status flags
 *   3. Works with block tree structures
 *
 * Note: ChainManager integration tests (UpdatePruneEligibility after reorgs)
 * are deferred until Phase M.1 (Mempool) is implemented.
 */
class PruneEligibilityTest : public ::testing::Test {
protected:
    // Helper: Create a test block index
    CBlockIndex* CreateBlockIndex(const std::string& hash, uint32_t height,
                                  const uint256& prev_hash) {
        auto index = std::make_unique<CBlockIndex>();
        index->hash = uint256::FromHexUnsafe(hash);
        index->height = height;
        index->prev_hash = prev_hash;
        index->chainwork = std::to_string(height * 1000);  // Simple chainwork
        index->status = BLOCK_VALID_CHAIN | BLOCK_HAVE_DATA | BLOCK_HAVE_UNDO;

        CBlockIndex* ptr = index.get();
        block_indices_[hash] = std::move(index);
        return ptr;
    }

    std::unordered_map<std::string, std::unique_ptr<CBlockIndex>> block_indices_;
};

/**
 * Test: Fork Scenario Flag Handling
 *
 * Validates that BLOCK_PRUNE_ELIGIBLE flag can be set on fork blocks
 * and cleared on main chain blocks.
 *
 * Block tree:
 *   Main:  Genesis → A → B → C
 *   Fork:  B → D → E
 *
 * Expected:
 *   - Fork blocks (D, E) can have PRUNE_ELIGIBLE flag set
 *   - Main chain blocks (B, C) can have flag cleared
 *   - Flag operations don't corrupt other status bits
 */
TEST_F(PruneEligibilityTest, test_fork_scenario) {
    // Create block tree
    CBlockIndex* genesis = CreateBlockIndex(std::string(64, '0'), 0, uint256());
    CBlockIndex* block_a = CreateBlockIndex("A" + std::string(63, 'a'), 1, genesis->hash);
    CBlockIndex* block_b = CreateBlockIndex("B" + std::string(63, 'b'), 2, block_a->hash);
    CBlockIndex* block_c = CreateBlockIndex("C" + std::string(63, 'c'), 3, block_b->hash);
    CBlockIndex* block_d = CreateBlockIndex("D" + std::string(63, 'd'), 3, block_b->hash);  // Fork from B
    CBlockIndex* block_e = CreateBlockIndex("E" + std::string(63, 'e'), 4, block_d->hash);

    // Link parent pointers (for structural testing)
    block_a->pprev = genesis;
    block_b->pprev = block_a;
    block_c->pprev = block_b;
    block_d->pprev = block_b;  // D forks from B
    block_e->pprev = block_d;

    // Test: Fork blocks can be marked prune-eligible
    block_d->status |= BLOCK_PRUNE_ELIGIBLE;
    EXPECT_TRUE(block_d->status & BLOCK_PRUNE_ELIGIBLE);

    block_e->status |= BLOCK_PRUNE_ELIGIBLE;
    EXPECT_TRUE(block_e->status & BLOCK_PRUNE_ELIGIBLE);

    // Test: Main chain blocks have flag cleared
    block_b->status &= ~BLOCK_PRUNE_ELIGIBLE;
    EXPECT_FALSE(block_b->status & BLOCK_PRUNE_ELIGIBLE);

    // Verify other flags preserved
    EXPECT_TRUE(block_b->status & BLOCK_VALID_CHAIN);
    EXPECT_TRUE(block_d->status & BLOCK_VALID_CHAIN);
}

/**
 * Test: Flag Semantics (Set/Clear)
 *
 * Validates that BLOCK_PRUNE_ELIGIBLE flag:
 *   - Defaults to unset (0)
 *   - Can be set with |= operator
 *   - Can be cleared with &= ~ operator
 *   - Doesn't interfere with other status bits
 *
 * Note: ChainDB persistence testing deferred to integration tests
 * (requires ChainManager with full persistence layer).
 */
TEST_F(PruneEligibilityTest, test_flag_semantics) {
    // Create test block
    CBlockIndex* block_a = CreateBlockIndex("A" + std::string(63, 'a'), 1, uint256::FromHexUnsafe(std::string(64, '0')));

    // Initially NOT prune-eligible
    EXPECT_FALSE(block_a->status & BLOCK_PRUNE_ELIGIBLE);

    // Mark as prune-eligible
    block_a->status |= BLOCK_PRUNE_ELIGIBLE;
    EXPECT_TRUE(block_a->status & BLOCK_PRUNE_ELIGIBLE);

    // Other flags should be preserved
    EXPECT_TRUE(block_a->status & BLOCK_VALID_CHAIN);
    EXPECT_TRUE(block_a->status & BLOCK_HAVE_DATA);
    EXPECT_TRUE(block_a->status & BLOCK_HAVE_UNDO);

    // Clear prune-eligible flag
    block_a->status &= ~BLOCK_PRUNE_ELIGIBLE;
    EXPECT_FALSE(block_a->status & BLOCK_PRUNE_ELIGIBLE);

    // Other flags still preserved
    EXPECT_TRUE(block_a->status & BLOCK_VALID_CHAIN);
}

/**
 * Test: Flag Handling with Large Block Trees
 *
 * Validates that BLOCK_PRUNE_ELIGIBLE flag works correctly
 * even with large block trees (300+ blocks).
 *
 * This ensures the flag doesn't have memory corruption issues
 * or unexpected interactions with long chains.
 */
TEST_F(PruneEligibilityTest, test_deep_burial) {
    // Create main chain
    CBlockIndex* genesis = CreateBlockIndex(std::string(64, '0'), 0, uint256());
    CBlockIndex* current = genesis;

    // Build 300-block main chain
    for (uint32_t i = 1; i <= 300; i++) {
        std::string hash = "M" + std::to_string(i) + std::string(62, 'm');
        CBlockIndex* block = CreateBlockIndex(hash, i, current->hash);
        block->pprev = current;
        current = block;
    }

    // Create old fork block at height 10
    CBlockIndex* fork_block = CreateBlockIndex("F" + std::string(63, 'f'), 10,
                                                 uint256::FromHexUnsafe(std::string(64, '0')));

    // Test: Old fork can be marked prune-eligible
    fork_block->status |= BLOCK_PRUNE_ELIGIBLE;
    EXPECT_TRUE(fork_block->status & BLOCK_PRUNE_ELIGIBLE);

    // Test: Recent fork has flag cleared
    CBlockIndex* recent_fork = CreateBlockIndex("R" + std::string(63, 'r'), 295,
                                                  uint256::FromHexUnsafe(std::string(64, '0')));
    recent_fork->status &= ~BLOCK_PRUNE_ELIGIBLE;
    EXPECT_FALSE(recent_fork->status & BLOCK_PRUNE_ELIGIBLE);
}

} // namespace test
} // namespace dinero

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
