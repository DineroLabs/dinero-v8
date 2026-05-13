/**
 * Day 2.1: Fork-Choice Logic Test (Simplified)
 *
 * Tests the fork-choice algorithm WITHOUT full state transition:
 * 1. Fork graph construction (multi-branch)
 * 2. FindForkPoint correctness
 * 3. GetChainPath correctness
 * 4. Rollback target selection
 *
 * This proves the LOGIC is correct before integrating with ConnectBlock/DisconnectBlock.
 *
 * Scenario:
 *   Genesis (0)
 *       |
 *    Block 1
 *       |
 *    Block 2 (FORK POINT)
 *       |
 *       +--- Block 3A → 4A → 5A (Chain A - active)
 *       |
 *       +--- Block 3B → 4B → 5B → 6B (Chain B - candidate)
 *
 * Expected:
 * - Fork point: Block 2
 * - Disconnect path: [5A, 4A, 3A]
 * - Connect path: [3B, 4B, 5B, 6B]
 */

#include "integration_test_runner.h"
#include <map>
#include <vector>
#include <string>
#include <algorithm>
#include <iostream>

using namespace dinero::test;

//=============================================================================
// Simplified Types for Fork Logic Testing
//=============================================================================

struct SimpleHash {
    std::string value;

    SimpleHash() : value("") {}
    explicit SimpleHash(const std::string& v) : value(v) {}

    bool operator==(const SimpleHash& other) const { return value == other.value; }
    bool operator!=(const SimpleHash& other) const { return value != other.value; }
    bool operator<(const SimpleHash& other) const { return value < other.value; }
    bool empty() const { return value.empty(); }
};

struct SimpleBlockIndex {
    SimpleHash hash;
    SimpleHash prev_hash;
    uint32_t height;
    uint64_t chainwork;

    SimpleBlockIndex() : height(0), chainwork(0) {}
};

//=============================================================================
// Simple Block Index DB (In-Memory)
//=============================================================================

class SimpleBlockIndexDB {
public:
    void add(const SimpleHash& hash, SimpleBlockIndex* index) {
        index_map_[hash] = index;
    }

    SimpleBlockIndex* get(const SimpleHash& hash) {
        auto it = index_map_.find(hash);
        return (it != index_map_.end()) ? it->second : nullptr;
    }

private:
    std::map<SimpleHash, SimpleBlockIndex*> index_map_;
};

//=============================================================================
// Fork-Choice Functions (Simplified from ActivateBestChain)
//=============================================================================

/**
 * Find the most recent common ancestor of two chains
 */
SimpleBlockIndex* FindForkPoint(
    const SimpleBlockIndex& chain_a,
    const SimpleBlockIndex& chain_b,
    SimpleBlockIndexDB& block_index_db
) {
    SimpleHash hash_a = chain_a.hash;
    SimpleHash hash_b = chain_b.hash;
    uint32_t height_a = chain_a.height;
    uint32_t height_b = chain_b.height;

    // First, bring both chains to the same height
    while (height_a > height_b) {
        SimpleBlockIndex* block = block_index_db.get(hash_a);
        if (!block) return nullptr;
        hash_a = block->prev_hash;
        height_a--;
    }

    while (height_b > height_a) {
        SimpleBlockIndex* block = block_index_db.get(hash_b);
        if (!block) return nullptr;
        hash_b = block->prev_hash;
        height_b--;
    }

    // Now walk backward together until we find common ancestor
    while (hash_a != hash_b) {
        SimpleBlockIndex* block_a = block_index_db.get(hash_a);
        SimpleBlockIndex* block_b = block_index_db.get(hash_b);

        if (!block_a || !block_b) {
            return nullptr;  // Fork point not found
        }

        // Move to parents
        hash_a = block_a->prev_hash;
        hash_b = block_b->prev_hash;
    }

    return block_index_db.get(hash_a);
}

/**
 * Get the path from start block to end block (exclusive of start)
 */
std::vector<SimpleBlockIndex*> GetChainPath(
    const SimpleHash& start_hash,
    const SimpleHash& end_hash,
    SimpleBlockIndexDB& block_index_db
) {
    std::vector<SimpleBlockIndex*> path;

    SimpleHash current = end_hash;
    while (current != start_hash) {
        SimpleBlockIndex* block = block_index_db.get(current);
        if (!block) {
            return {};  // Path not found
        }

        path.push_back(block);
        current = block->prev_hash;
    }

    // Reverse to get correct order (from start to end)
    std::reverse(path.begin(), path.end());
    return path;
}

//=============================================================================
// Test: Fork Point Finding
//=============================================================================

bool TestForkPointFinding() {
    std::cout << "\n========================================\n";
    std::cout << "Test: Fork Point Finding\n";
    std::cout << "========================================\n";

    SimpleBlockIndexDB db;

    // Build fork graph
    // Genesis (0)
    SimpleBlockIndex* genesis = new SimpleBlockIndex();
    genesis->hash = SimpleHash("genesis");
    genesis->prev_hash = SimpleHash("");
    genesis->height = 0;
    genesis->chainwork = 1;
    db.add(genesis->hash, genesis);

    std::cout << "  [✅ Genesis created (height 0)]\n";

    // Block 1
    SimpleBlockIndex* block1 = new SimpleBlockIndex();
    block1->hash = SimpleHash("block1");
    block1->prev_hash = genesis->hash;
    block1->height = 1;
    block1->chainwork = 2;
    db.add(block1->hash, block1);

    std::cout << "  [✅ Block 1 created (height 1)]\n";

    // Block 2 (fork point)
    SimpleBlockIndex* block2 = new SimpleBlockIndex();
    block2->hash = SimpleHash("block2");
    block2->prev_hash = block1->hash;
    block2->height = 2;
    block2->chainwork = 3;
    db.add(block2->hash, block2);

    std::cout << "  [✅ Block 2 created (height 2) - FORK POINT]\n";

    // Chain A: 3A → 4A → 5A
    SimpleBlockIndex* block3a = new SimpleBlockIndex();
    block3a->hash = SimpleHash("block3a");
    block3a->prev_hash = block2->hash;
    block3a->height = 3;
    block3a->chainwork = 4;
    db.add(block3a->hash, block3a);

    SimpleBlockIndex* block4a = new SimpleBlockIndex();
    block4a->hash = SimpleHash("block4a");
    block4a->prev_hash = block3a->hash;
    block4a->height = 4;
    block4a->chainwork = 5;
    db.add(block4a->hash, block4a);

    SimpleBlockIndex* block5a = new SimpleBlockIndex();
    block5a->hash = SimpleHash("block5a");
    block5a->prev_hash = block4a->hash;
    block5a->height = 5;
    block5a->chainwork = 6;
    db.add(block5a->hash, block5a);

    std::cout << "  [✅ Chain A created: 3A → 4A → 5A]\n";

    // Chain B: 3B → 4B → 5B → 6B
    SimpleBlockIndex* block3b = new SimpleBlockIndex();
    block3b->hash = SimpleHash("block3b");
    block3b->prev_hash = block2->hash;
    block3b->height = 3;
    block3b->chainwork = 4;
    db.add(block3b->hash, block3b);

    SimpleBlockIndex* block4b = new SimpleBlockIndex();
    block4b->hash = SimpleHash("block4b");
    block4b->prev_hash = block3b->hash;
    block4b->height = 4;
    block4b->chainwork = 5;
    db.add(block4b->hash, block4b);

    SimpleBlockIndex* block5b = new SimpleBlockIndex();
    block5b->hash = SimpleHash("block5b");
    block5b->prev_hash = block4b->hash;
    block5b->height = 5;
    block5b->chainwork = 6;
    db.add(block5b->hash, block5b);

    SimpleBlockIndex* block6b = new SimpleBlockIndex();
    block6b->hash = SimpleHash("block6b");
    block6b->prev_hash = block5b->hash;
    block6b->height = 6;
    block6b->chainwork = 7;  // More work than Chain A
    db.add(block6b->hash, block6b);

    std::cout << "  [✅ Chain B created: 3B → 4B → 5B → 6B (more work)]\n";

    // Test fork point finding
    std::cout << "\n  [Finding fork point between 5A and 6B...]\n";
    SimpleBlockIndex* fork_point = FindForkPoint(*block5a, *block6b, db);

    ASSERT_TRUE(fork_point != nullptr);
    ASSERT_TRUE(fork_point->hash.value == "block2");
    ASSERT_EQ(fork_point->height, 2);

    std::cout << "  [✅ Fork point found: Block 2 (height " << fork_point->height << ")]\n";

    std::cout << "\n========================================\n";
    std::cout << "✅ Test Passed: Fork Point Finding\n";
    std::cout << "========================================\n";

    // Cleanup
    delete genesis;
    delete block1;
    delete block2;
    delete block3a;
    delete block4a;
    delete block5a;
    delete block3b;
    delete block4b;
    delete block5b;
    delete block6b;

    return true;
}

//=============================================================================
// Test: Chain Path Extraction
//=============================================================================

bool TestChainPathExtraction() {
    std::cout << "\n========================================\n";
    std::cout << "Test: Chain Path Extraction\n";
    std::cout << "========================================\n";

    SimpleBlockIndexDB db;

    // Build simple chain: Genesis → 1 → 2 → 3 → 4 → 5
    std::vector<SimpleBlockIndex*> blocks;

    for (int i = 0; i <= 5; i++) {
        SimpleBlockIndex* block = new SimpleBlockIndex();
        block->hash = SimpleHash("block" + std::to_string(i));
        block->prev_hash = (i == 0) ? SimpleHash("") : SimpleHash("block" + std::to_string(i - 1));
        block->height = i;
        block->chainwork = i + 1;
        db.add(block->hash, block);
        blocks.push_back(block);
    }

    std::cout << "  [✅ Built chain: Genesis → 1 → 2 → 3 → 4 → 5]\n";

    // Test path extraction from Block 2 to Block 5
    std::cout << "  [Extracting path from Block 2 to Block 5...]\n";
    auto path = GetChainPath(SimpleHash("block2"), SimpleHash("block5"), db);

    ASSERT_EQ(path.size(), 3);  // Should be [3, 4, 5]
    ASSERT_TRUE(path[0]->hash.value == "block3");
    ASSERT_TRUE(path[1]->hash.value == "block4");
    ASSERT_TRUE(path[2]->hash.value == "block5");

    std::cout << "  [✅ Path extracted: [Block 3, Block 4, Block 5]]\n";
    std::cout << "  [✅ Path length: " << path.size() << " blocks]\n";

    std::cout << "\n========================================\n";
    std::cout << "✅ Test Passed: Chain Path Extraction\n";
    std::cout << "========================================\n";

    // Cleanup
    for (auto* block : blocks) {
        delete block;
    }

    return true;
}

//=============================================================================
// Test: Reorg Path Calculation
//=============================================================================

bool TestReorgPathCalculation() {
    std::cout << "\n========================================\n";
    std::cout << "Test: Reorg Path Calculation\n";
    std::cout << "========================================\n";

    SimpleBlockIndexDB db;

    // Build fork graph (same as TestForkPointFinding)
    SimpleBlockIndex* genesis = new SimpleBlockIndex();
    genesis->hash = SimpleHash("genesis");
    genesis->prev_hash = SimpleHash("");
    genesis->height = 0;
    genesis->chainwork = 1;
    db.add(genesis->hash, genesis);

    SimpleBlockIndex* block1 = new SimpleBlockIndex();
    block1->hash = SimpleHash("block1");
    block1->prev_hash = genesis->hash;
    block1->height = 1;
    block1->chainwork = 2;
    db.add(block1->hash, block1);

    SimpleBlockIndex* block2 = new SimpleBlockIndex();
    block2->hash = SimpleHash("block2");
    block2->prev_hash = block1->hash;
    block2->height = 2;
    block2->chainwork = 3;
    db.add(block2->hash, block2);

    // Chain A
    SimpleBlockIndex* block3a = new SimpleBlockIndex();
    block3a->hash = SimpleHash("block3a");
    block3a->prev_hash = block2->hash;
    block3a->height = 3;
    block3a->chainwork = 4;
    db.add(block3a->hash, block3a);

    SimpleBlockIndex* block4a = new SimpleBlockIndex();
    block4a->hash = SimpleHash("block4a");
    block4a->prev_hash = block3a->hash;
    block4a->height = 4;
    block4a->chainwork = 5;
    db.add(block4a->hash, block4a);

    SimpleBlockIndex* block5a = new SimpleBlockIndex();
    block5a->hash = SimpleHash("block5a");
    block5a->prev_hash = block4a->hash;
    block5a->height = 5;
    block5a->chainwork = 6;
    db.add(block5a->hash, block5a);

    // Chain B
    SimpleBlockIndex* block3b = new SimpleBlockIndex();
    block3b->hash = SimpleHash("block3b");
    block3b->prev_hash = block2->hash;
    block3b->height = 3;
    block3b->chainwork = 4;
    db.add(block3b->hash, block3b);

    SimpleBlockIndex* block4b = new SimpleBlockIndex();
    block4b->hash = SimpleHash("block4b");
    block4b->prev_hash = block3b->hash;
    block4b->height = 4;
    block4b->chainwork = 5;
    db.add(block4b->hash, block4b);

    SimpleBlockIndex* block5b = new SimpleBlockIndex();
    block5b->hash = SimpleHash("block5b");
    block5b->prev_hash = block4b->hash;
    block5b->height = 5;
    block5b->chainwork = 6;
    db.add(block5b->hash, block5b);

    SimpleBlockIndex* block6b = new SimpleBlockIndex();
    block6b->hash = SimpleHash("block6b");
    block6b->prev_hash = block5b->hash;
    block6b->height = 6;
    block6b->chainwork = 7;
    db.add(block6b->hash, block6b);

    std::cout << "  [✅ Fork graph built]\n";
    std::cout << "  [Active tip: Block 5A (height 5, work 6)]\n";
    std::cout << "  [Candidate tip: Block 6B (height 6, work 7)]\n";

    // Calculate reorg paths
    std::cout << "\n  [Calculating reorg from 5A to 6B...]\n";

    SimpleBlockIndex* fork_point = FindForkPoint(*block5a, *block6b, db);
    ASSERT_TRUE(fork_point != nullptr);
    ASSERT_TRUE(fork_point->hash.value == "block2");

    std::cout << "  [✅ Fork point: Block 2]\n";

    // Disconnect path (from active tip back to fork point)
    auto disconnect_path = GetChainPath(fork_point->hash, block5a->hash, db);
    ASSERT_EQ(disconnect_path.size(), 3);  // [3A, 4A, 5A]
    ASSERT_TRUE(disconnect_path[0]->hash.value == "block3a");
    ASSERT_TRUE(disconnect_path[1]->hash.value == "block4a");
    ASSERT_TRUE(disconnect_path[2]->hash.value == "block5a");

    std::cout << "  [✅ Disconnect path: [3A, 4A, 5A] (" << disconnect_path.size() << " blocks)]\n";

    // Connect path (from fork point to candidate tip)
    auto connect_path = GetChainPath(fork_point->hash, block6b->hash, db);
    ASSERT_EQ(connect_path.size(), 4);  // [3B, 4B, 5B, 6B]
    ASSERT_TRUE(connect_path[0]->hash.value == "block3b");
    ASSERT_TRUE(connect_path[1]->hash.value == "block4b");
    ASSERT_TRUE(connect_path[2]->hash.value == "block5b");
    ASSERT_TRUE(connect_path[3]->hash.value == "block6b");

    std::cout << "  [✅ Connect path: [3B, 4B, 5B, 6B] (" << connect_path.size() << " blocks)]\n";

    std::cout << "\n  [Reorg Summary:]\n";
    std::cout << "    - Disconnect: " << disconnect_path.size() << " blocks\n";
    std::cout << "    - Connect: " << connect_path.size() << " blocks\n";
    std::cout << "    - Fork point: Block 2 (height " << fork_point->height << ")\n";
    std::cout << "    - New tip: Block 6B (height " << block6b->height << ", work " << block6b->chainwork << ")\n";

    std::cout << "\n========================================\n";
    std::cout << "✅ Test Passed: Reorg Path Calculation\n";
    std::cout << "========================================\n";

    // Cleanup
    delete genesis;
    delete block1;
    delete block2;
    delete block3a;
    delete block4a;
    delete block5a;
    delete block3b;
    delete block4b;
    delete block5b;
    delete block6b;

    return true;
}

//=============================================================================
// Main
//=============================================================================

int main() {
    IntegrationTestRunner runner;

    std::cout << "════════════════════════════════════════\n";
    std::cout << "  Day 2.1: Fork-Choice Logic Tests\n";
    std::cout << "  (Simplified - Algorithm Verification)\n";
    std::cout << "════════════════════════════════════════\n";

    runner.RunTest("Fork Point Finding", TestForkPointFinding);
    runner.RunTest("Chain Path Extraction", TestChainPathExtraction);
    runner.RunTest("Reorg Path Calculation", TestReorgPathCalculation);

    runner.PrintSummary();

    return runner.AllTestsPassed() ? 0 : 1;
}
