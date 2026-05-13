/**
 * Fork Choice Test - Chain Selection & Reorg (Phase 3B)
 *
 * Purpose: Prove that fork choice selects the chain with most cumulative work
 *
 * Scope (LOCKED):
 * ✅ Chainwork calculation (WorkForBits, AddWork)
 * ✅ Fork choice (GetBestCandidate selects highest chainwork)
 * ✅ Candidate management (AddCandidate/RemoveCandidate)
 * ✅ Equal-work tiebreaking (lowest hash wins)
 *
 * NOT in scope:
 * ❌ Full ActivateBestChain orchestration (future phase)
 * ❌ Actual UTXO reorgs (proven in test_ibd_reorg)
 * ❌ Persistent storage
 *
 * Test Flow:
 * 1. Create genesis block
 * 2. Create two competing chains from genesis:
 *    - Chain A: blocks 1A-2A-3A (low difficulty)
 *    - Chain B: blocks 1B-2B (high difficulty, more total work)
 * 3. Add all blocks to block index
 * 4. Verify GetBestCandidate() selects Chain B tip (most work)
 * 5. Test equal-work tiebreaking (lowest hash wins)
 *
 * What This Proves:
 * - Chainwork calculation is correct
 * - Fork choice selects chain with most cumulative work
 * - Equal-work scenarios use hash as tiebreaker
 * - Candidate management works correctly
 */

#include <iostream>
#include <cassert>
#include <vector>
#include <iomanip>

// Dinero core includes
#include "consensus/block_index.h"
#include "consensus/block_lifecycle.h"  // BLOCK_HAVE_DATA flag, IsEligibleForCandidacy
#include "consensus/chainwork.h"
#include "consensus/chainparams.h"
#include "primitives/block.h"
#include "crypto/sha256.h"

using namespace dinero;

/**
 * Test Helper: Create a test block header with specific difficulty
 */
BlockHeader CreateTestHeader(
    const std::string& prev_hash,
    uint32_t difficulty_bits,
    uint32_t nonce = 0
) {
    BlockHeader header;
    header.version = 2;
    // Ensure prevBlockHash is always 64 hex characters (use all zeros for genesis)
    header.prev_block_hash = prev_hash.empty() ?
        uint256::FromHexUnsafe("0000000000000000000000000000000000000000000000000000000000000000") :
        uint256::FromHexUnsafe(prev_hash);
    header.merkle_root = uint256::FromHexUnsafe("0000000000000000000000000000000000000000000000000000000000000000");
    header.timestamp = 1772496000;
    header.difficulty = difficulty_bits;
    header.utreexo_root = uint256();  // Null for test
    std::memset(header.reserved, 0, 12);  // Zero reserved field
    header.nonce = nonce;
    return header;
}

/**
 * Test Helper: Print block index info
 */
void PrintBlockInfo(const CBlockIndex* block, const std::string& label) {
    std::cout << "  " << label << ":\n";
    std::cout << "    Hash: " << block->hash.GetHex().substr(0, 16) << "...\n";
    std::cout << "    Height: " << block->height << "\n";
    std::cout << "    Bits: 0x" << std::hex << block->bits << std::dec << "\n";
    std::cout << "    Chainwork: ..." << block->chainwork.substr(48, 16) << "\n";
    std::cout << "    Status: " << block->status << "\n";
}

/**
 * Main Fork Choice Test
 */
int main() {
    std::cout << "\n";
    std::cout << "╔═══════════════════════════════════════════════════════════╗\n";
    std::cout << "║  Fork Choice Test - Chain Selection (Phase 3B)           ║\n";
    std::cout << "╚═══════════════════════════════════════════════════════════╝\n";
    std::cout << "\n";

    try {
        // ===================================================================
        // STEP 1: Create Genesis Block
        // ===================================================================

        std::cout << "Step 1: Create genesis block...\n";

        SelectParams(Chain::REGTEST);
        const ChainParams& params = Params();

        // Create genesis block index
        BlockHeader genesis_header = CreateTestHeader(
            "",  // No parent
            0x1e0ffff0,  // Standard difficulty
            0
        );

        CBlockIndex* genesis = AddBlockIndex(genesis_header, 0);
        genesis->hash = uint256::FromHexUnsafe(params.genesis_hash);
        genesis->status = BLOCK_VALID_CHAIN | BLOCK_HAVE_DATA;  // Genesis is always valid

        std::cout << "  ✅ Genesis block created: " << genesis->hash.GetHex().substr(0, 16) << "...\n";
        std::cout << "  ✅ Genesis chainwork: " << genesis->chainwork << "\n";

        // ===================================================================
        // STEP 2: Create Two Competing Chains
        // ===================================================================

        std::cout << "\nStep 2: Create competing chains...\n";

        // Chain A: Low difficulty (0x1e0ffff0), 3 blocks
        // Each block contributes small amount of work
        uint32_t low_bits = 0x1e0ffff0;

        BlockHeader header_1a = CreateTestHeader(genesis->hash.GetHex(), low_bits, 1);
        CBlockIndex* block_1a = AddBlockIndex(header_1a, 1);
        block_1a->status = BLOCK_VALID_CHAIN | BLOCK_HAVE_DATA;

        BlockHeader header_2a = CreateTestHeader(block_1a->hash.GetHex(), low_bits, 2);
        CBlockIndex* block_2a = AddBlockIndex(header_2a, 2);
        block_2a->status = BLOCK_VALID_CHAIN | BLOCK_HAVE_DATA;

        BlockHeader header_3a = CreateTestHeader(block_2a->hash.GetHex(), low_bits, 3);
        CBlockIndex* block_3a = AddBlockIndex(header_3a, 3);
        block_3a->status = BLOCK_VALID_CHAIN | BLOCK_HAVE_DATA;

        std::cout << "  Chain A (low difficulty, 3 blocks):\n";
        PrintBlockInfo(block_1a, "Block 1A");
        PrintBlockInfo(block_2a, "Block 2A");
        PrintBlockInfo(block_3a, "Block 3A (tip)");

        // Chain B: Higher difficulty (0x1d00ffff), 2 blocks
        // Each block contributes much more work than Chain A blocks
        uint32_t high_bits = 0x1d00ffff;  // 256x more difficult

        BlockHeader header_1b = CreateTestHeader(genesis->hash.GetHex(), high_bits, 100);
        CBlockIndex* block_1b = AddBlockIndex(header_1b, 1);
        block_1b->status = BLOCK_VALID_CHAIN | BLOCK_HAVE_DATA;

        BlockHeader header_2b = CreateTestHeader(block_1b->hash.GetHex(), high_bits, 200);
        CBlockIndex* block_2b = AddBlockIndex(header_2b, 2);
        block_2b->status = BLOCK_VALID_CHAIN | BLOCK_HAVE_DATA;

        std::cout << "\n  Chain B (high difficulty, 2 blocks):\n";
        PrintBlockInfo(block_1b, "Block 1B");
        PrintBlockInfo(block_2b, "Block 2B (tip)");

        // ===================================================================
        // STEP 3: Add Candidates and Test Fork Choice
        // ===================================================================

        std::cout << "\nStep 3: Test fork choice...\n";

        // Add both chain tips as candidates
        AddCandidate(block_3a);  // Chain A tip
        AddCandidate(block_2b);  // Chain B tip

        // Get best candidate (should be Chain B due to more cumulative work)
        CBlockIndex* best = GetBestCandidate();

        if (!best) {
            std::cerr << "❌ No best candidate found\n";
            return 1;
        }

        std::cout << "  Best candidate:\n";
        PrintBlockInfo(best, "Winner");

        // Verify Chain B wins (has more cumulative work despite fewer blocks)
        int work_comparison = chainwork::CompareWork(block_2b->chainwork, block_3a->chainwork);

        std::cout << "\n  Chainwork comparison:\n";
        std::cout << "    Chain A (3 blocks): ..." << block_3a->chainwork.substr(48, 16) << "\n";
        std::cout << "    Chain B (2 blocks): ..." << block_2b->chainwork.substr(48, 16) << "\n";
        std::cout << "    Comparison result: " << work_comparison << " (1 = B > A)\n";

        if (work_comparison <= 0) {
            std::cerr << "❌ Chain B should have more work than Chain A\n";
            std::cerr << "   (Higher difficulty should result in more cumulative work)\n";
            return 1;
        }

        if (best->hash != block_2b->hash) {
            std::cerr << "❌ Fork choice selected wrong chain\n";
            std::cerr << "   Expected: Chain B tip (block_2b)\n";
            std::cerr << "   Got: " << best->hash.GetHex().substr(0, 16) << "...\n";
            return 1;
        }

        std::cout << "  ✅ Fork choice correctly selected Chain B (more work)\n";

        // ===================================================================
        // STEP 4: Test Equal-Work Tiebreaking
        // ===================================================================

        std::cout << "\nStep 4: Test equal-work tiebreaking...\n";

        // Create two chains with identical work but different hashes
        // Use same difficulty and same number of blocks
        BlockHeader header_1c = CreateTestHeader(genesis->hash.GetHex(), low_bits, 500);
        CBlockIndex* block_1c = AddBlockIndex(header_1c, 1);
        block_1c->status = BLOCK_VALID_CHAIN | BLOCK_HAVE_DATA;

        BlockHeader header_1d = CreateTestHeader(genesis->hash.GetHex(), low_bits, 600);
        CBlockIndex* block_1d = AddBlockIndex(header_1d, 1);
        block_1d->status = BLOCK_VALID_CHAIN | BLOCK_HAVE_DATA;

        // Both should have identical chainwork
        if (block_1c->chainwork != block_1d->chainwork) {
            std::cerr << "❌ Blocks should have equal chainwork\n";
            return 1;
        }

        std::cout << "  Created two blocks with equal work:\n";
        std::cout << "    Block 1C hash: " << block_1c->hash.GetHex().substr(0, 16) << "...\n";
        std::cout << "    Block 1D hash: " << block_1d->hash.GetHex().substr(0, 16) << "...\n";
        std::cout << "    Both chainwork: ..." << block_1c->chainwork.substr(48, 16) << "\n";

        // Clear candidates and test tiebreaking
        g_candidates.clear();
        AddCandidate(block_1c);
        AddCandidate(block_1d);

        CBlockIndex* tiebreak_winner = GetBestCandidate();
        if (!tiebreak_winner) {
            std::cerr << "❌ No winner in tiebreaking test\n";
            return 1;
        }

        // Winner should be the one with lowest hash (lexicographic order)
        CBlockIndex* expected_winner = (block_1c->hash < block_1d->hash) ? block_1c : block_1d;

        if (tiebreak_winner->hash != expected_winner->hash) {
            std::cerr << "❌ Tiebreaking failed\n";
            std::cerr << "   Expected lowest hash to win\n";
            return 1;
        }

        std::cout << "  ✅ Tiebreaking: Lowest hash wins\n";
        std::cout << "    Winner: " << tiebreak_winner->hash.GetHex().substr(0, 16) << "...\n";

        // ===================================================================
        // STEP 5: Test Candidate Management
        // ===================================================================

        std::cout << "\nStep 5: Test candidate management...\n";

        g_candidates.clear();

        // Add multiple candidates
        AddCandidate(block_1a);
        AddCandidate(block_2a);
        AddCandidate(block_3a);
        AddCandidate(block_1b);
        AddCandidate(block_2b);

        std::cout << "  Added 5 candidates to set\n";
        std::cout << "  Candidate set size: " << g_candidates.size() << "\n";

        // Best should be the one with most work
        best = GetBestCandidate();
        if (!best) {
            std::cerr << "❌ No best candidate\n";
            return 1;
        }

        // Remove best candidate
        RemoveCandidate(best);
        std::cout << "  Removed best candidate: " << best->hash.GetHex().substr(0, 16) << "...\n";
        std::cout << "  Candidate set size after removal: " << g_candidates.size() << "\n";

        // Get new best
        CBlockIndex* second_best = GetBestCandidate();
        if (!second_best) {
            std::cerr << "❌ No second best candidate\n";
            return 1;
        }

        // Second best should have less work than original best
        if (chainwork::CompareWork(second_best->chainwork, best->chainwork) >= 0) {
            std::cerr << "❌ Second best should have less work than original best\n";
            return 1;
        }

        std::cout << "  ✅ Candidate management works correctly\n";
        std::cout << "    Second best: " << second_best->hash.GetHex().substr(0, 16) << "...\n";

        // ===================================================================
        // STEP 6: Summary
        // ===================================================================

        std::cout << "\n";
        std::cout << "╔═══════════════════════════════════════════════════════════╗\n";
        std::cout << "║  ✅ FORK CHOICE TEST PASSED                               ║\n";
        std::cout << "╚═══════════════════════════════════════════════════════════╝\n";
        std::cout << "\n";

        std::cout << "Verified:\n";
        std::cout << "  ✅ Chainwork calculation (WorkForBits, AddWork)\n";
        std::cout << "  ✅ Fork choice selects chain with most work\n";
        std::cout << "  ✅ Higher difficulty = more work per block\n";
        std::cout << "  ✅ Cumulative work matters (not just block count)\n";
        std::cout << "  ✅ Equal-work tiebreaking (lowest hash wins)\n";
        std::cout << "  ✅ Candidate management (add/remove/get best)\n";
        std::cout << "\n";

        std::cout << "What This Proves:\n";
        std::cout << "  • Fork choice infrastructure is correct\n";
        std::cout << "  • Chain with most proof-of-work wins\n";
        std::cout << "  • Deterministic tiebreaking prevents splits\n";
        std::cout << "  • Ready for ActivateBestChain integration\n";
        std::cout << "\n";

        std::cout << "Phase 3B Complete - Fork Choice Proven\n";
        std::cout << "\n";

        return 0;

    } catch (const std::exception& e) {
        std::cerr << "\n❌ TEST FAILED: " << e.what() << "\n";
        return 1;
    }
}
