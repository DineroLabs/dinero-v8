/**
 * @file test_hash_domains_m3.cpp
 * @brief Phase M.3: Semantic Hash Domains - Compiler Enforcement Tests
 *
 * Purpose: Verify that semantic hash types prevent cross-domain confusion at compile time
 *
 * What We're Testing:
 * 1. Domain types are distinct (no implicit conversions)
 * 2. Domain-locked constructors work correctly
 * 3. Storage/serialization unchanged (32 bytes)
 * 4. Comparison operators work within same domain
 * 5. Hash functions work for std::unordered_map
 */

#include <gtest/gtest.h>
#include "primitives/hash_domains.h"
#include "primitives/block.h"
#include "primitives/transaction.h"
#include <unordered_set>
#include <set>

using namespace dinero;

// ═══════════════════════════════════════════════════════════════════════
// Test 1: Semantic Types Are Distinct
// ═══════════════════════════════════════════════════════════════════════

TEST(HashDomainsM3Test, TypesAreDistinct) {
    // These static asserts are in hash_domains.h, but we verify them here
    // to ensure the test itself enforces the invariants

    // BlockHash and TxId are NOT interchangeable
    static_assert(!std::is_convertible<BlockHash, TxId>::value,
        "BlockHash must NOT convert to TxId");
    static_assert(!std::is_convertible<TxId, BlockHash>::value,
        "TxId must NOT convert to BlockHash");

    // TxId and WTxId are NOT interchangeable (malleability protection)
    static_assert(!std::is_convertible<TxId, WTxId>::value,
        "TxId must NOT convert to WTxId");
    static_assert(!std::is_convertible<WTxId, TxId>::value,
        "WTxId must NOT convert to TxId");

    // Merkle roots are NOT transaction IDs
    static_assert(!std::is_convertible<MerkleRoot, TxId>::value,
        "MerkleRoot must NOT convert to TxId");
    static_assert(!std::is_convertible<TxId, MerkleRoot>::value,
        "TxId must NOT convert to MerkleRoot");

    // Utreexo roots are NOT block hashes
    static_assert(!std::is_convertible<UtreexoRoot, BlockHash>::value,
        "UtreexoRoot must NOT convert to BlockHash");
    static_assert(!std::is_convertible<BlockHash, UtreexoRoot>::value,
        "BlockHash must NOT convert to UtreexoRoot");

    SUCCEED() << "✓ All domain types are distinct (no implicit conversions)";
}

// ═══════════════════════════════════════════════════════════════════════
// Test 2: Storage Guarantee (32 bytes)
// ═══════════════════════════════════════════════════════════════════════

TEST(HashDomainsM3Test, StorageIs32Bytes) {
    // All semantic hash types must be exactly 32 bytes (same as uint256)
    EXPECT_EQ(sizeof(BlockHash), 32);
    EXPECT_EQ(sizeof(TxId), 32);
    EXPECT_EQ(sizeof(WTxId), 32);
    EXPECT_EQ(sizeof(MerkleRoot), 32);
    EXPECT_EQ(sizeof(UtreexoRoot), 32);

    // Same as uint256
    EXPECT_EQ(sizeof(BlockHash), sizeof(uint256));
    EXPECT_EQ(sizeof(TxId), sizeof(uint256));

    std::cout << "✓ All domain types are exactly 32 bytes (storage-compatible with uint256)" << std::endl;
}

// ═══════════════════════════════════════════════════════════════════════
// Test 3: Trivially Copyable (Performance Guarantee)
// ═══════════════════════════════════════════════════════════════════════

TEST(HashDomainsM3Test, TriviallyCopyable) {
    // All semantic hash types must be trivially copyable (memcpy-safe)
    static_assert(std::is_trivially_copyable<BlockHash>::value,
        "BlockHash must be trivially copyable");
    static_assert(std::is_trivially_copyable<TxId>::value,
        "TxId must be trivially copyable");
    static_assert(std::is_trivially_copyable<WTxId>::value,
        "WTxId must be trivially copyable");
    static_assert(std::is_trivially_copyable<MerkleRoot>::value,
        "MerkleRoot must be trivially copyable");
    static_assert(std::is_trivially_copyable<UtreexoRoot>::value,
        "UtreexoRoot must be trivially copyable");

    SUCCEED() << "✓ All domain types are trivially copyable (high-performance)";
}

// ═══════════════════════════════════════════════════════════════════════
// Test 4: Comparison Operators (Within Same Domain)
// ═══════════════════════════════════════════════════════════════════════

TEST(HashDomainsM3Test, ComparisonOperatorsWork) {
    // Create two different hashes
    uint256 hash1, hash2;
    hash1.SetNull();
    hash2.SetNull();
    *(reinterpret_cast<uint32_t*>(&hash2)) = 1;  // Make hash2 different

    BlockHash bh1(hash1), bh2(hash2);
    TxId tx1(hash1), tx2(hash2);

    // Equality
    EXPECT_TRUE(bh1 == bh1);
    EXPECT_FALSE(bh1 == bh2);
    EXPECT_TRUE(tx1 == tx1);
    EXPECT_FALSE(tx1 == tx2);

    // Inequality
    EXPECT_FALSE(bh1 != bh1);
    EXPECT_TRUE(bh1 != bh2);

    // Less-than (for std::set/std::map)
    EXPECT_TRUE(bh1 < bh2);
    EXPECT_FALSE(bh2 < bh1);

    std::cout << "✓ Comparison operators work within same domain" << std::endl;
}

// ═══════════════════════════════════════════════════════════════════════
// Test 5: std::unordered_set Support (Hash Function)
// ═══════════════════════════════════════════════════════════════════════

TEST(HashDomainsM3Test, UnorderedSetSupport) {
    uint256 hash1, hash2;
    hash1.SetNull();
    hash2.SetNull();
    *(reinterpret_cast<uint32_t*>(&hash2)) = 1;

    // BlockHash in unordered_set
    std::unordered_set<BlockHash> block_set;
    block_set.insert(BlockHash(hash1));
    block_set.insert(BlockHash(hash2));
    EXPECT_EQ(block_set.size(), 2);

    // TxId in unordered_set
    std::unordered_set<TxId> tx_set;
    tx_set.insert(TxId(hash1));
    tx_set.insert(TxId(hash2));
    EXPECT_EQ(tx_set.size(), 2);

    std::cout << "✓ std::unordered_set works with semantic hash types" << std::endl;
}

// ═══════════════════════════════════════════════════════════════════════
// Test 6: std::set Support (Ordered Containers)
// ═══════════════════════════════════════════════════════════════════════

TEST(HashDomainsM3Test, OrderedSetSupport) {
    uint256 hash1, hash2, hash3;
    hash1.SetNull();
    hash2.SetNull();
    hash3.SetNull();
    *(reinterpret_cast<uint32_t*>(&hash2)) = 1;
    *(reinterpret_cast<uint32_t*>(&hash3)) = 2;

    // TxId in std::set (Phase M.2 binary duplicate detection)
    std::set<TxId> tx_set;
    tx_set.insert(TxId(hash1));
    tx_set.insert(TxId(hash2));
    tx_set.insert(TxId(hash3));
    tx_set.insert(TxId(hash1));  // Duplicate - should not increase size

    EXPECT_EQ(tx_set.size(), 3);  // Only 3 unique elements

    std::cout << "✓ std::set works with semantic hash types (binary duplicate detection)" << std::endl;
}

// ═══════════════════════════════════════════════════════════════════════
// Test 7: IsNull() Works Correctly
// ═══════════════════════════════════════════════════════════════════════

TEST(HashDomainsM3Test, IsNullWorks) {
    // Default-constructed hashes are null
    BlockHash null_block;
    TxId null_tx;
    WTxId null_wtx;

    EXPECT_TRUE(null_block.IsNull());
    EXPECT_TRUE(null_tx.IsNull());
    EXPECT_TRUE(null_wtx.IsNull());

    // Non-null hash
    uint256 hash;
    hash.SetNull();
    *(reinterpret_cast<uint32_t*>(&hash)) = 0xDEADBEEF;

    BlockHash non_null_block(hash);
    EXPECT_FALSE(non_null_block.IsNull());

    std::cout << "✓ IsNull() works correctly for all domain types" << std::endl;
}

// ═══════════════════════════════════════════════════════════════════════
// Test 8: AsUint256() Accessor Works
// ═══════════════════════════════════════════════════════════════════════

TEST(HashDomainsM3Test, AsUint256Works) {
    uint256 original_hash;
    original_hash.SetNull();
    *(reinterpret_cast<uint32_t*>(&original_hash)) = 0x12345678;

    BlockHash block_hash(original_hash);
    const uint256& extracted = block_hash.AsUint256();

    // Should be same bytes
    EXPECT_EQ(extracted, original_hash);

    std::cout << "✓ AsUint256() accessor provides correct underlying uint256" << std::endl;
}

// ═══════════════════════════════════════════════════════════════════════
// Test 9: MerkleRoot Computation
// ═══════════════════════════════════════════════════════════════════════

TEST(HashDomainsM3Test, MerkleRootComputation) {
    // Empty merkle tree
    std::vector<uint256> empty_leaves;
    MerkleRoot empty_root = MerkleRoot::Compute(empty_leaves);
    EXPECT_TRUE(empty_root.IsNull());

    // Single leaf
    uint256 leaf1;
    leaf1.SetNull();
    *(reinterpret_cast<uint32_t*>(&leaf1)) = 1;
    std::vector<uint256> single_leaf = {leaf1};
    MerkleRoot single_root = MerkleRoot::Compute(single_leaf);
    EXPECT_EQ(single_root.AsUint256(), leaf1);

    // Two leaves (actual merkle computation)
    uint256 leaf2;
    leaf2.SetNull();
    *(reinterpret_cast<uint32_t*>(&leaf2)) = 2;
    std::vector<uint256> two_leaves = {leaf1, leaf2};
    MerkleRoot two_root = MerkleRoot::Compute(two_leaves);
    EXPECT_FALSE(two_root.IsNull());
    EXPECT_NE(two_root.AsUint256(), leaf1);
    EXPECT_NE(two_root.AsUint256(), leaf2);

    std::cout << "✓ MerkleRoot::Compute() works correctly" << std::endl;
}

// ═══════════════════════════════════════════════════════════════════════
// Main
// ═══════════════════════════════════════════════════════════════════════

int main(int argc, char** argv) {
    testing::InitGoogleTest(&argc, argv);

    std::cout << "\n"
              << "═══════════════════════════════════════════════════════════════\n"
              << "Phase M.3: Semantic Hash Domains - Compiler Enforcement Tests\n"
              << "═══════════════════════════════════════════════════════════════\n"
              << std::endl;

    int result = RUN_ALL_TESTS();

    if (result == 0) {
        std::cout << "\n"
                  << "═══════════════════════════════════════════════════════════════\n"
                  << "✅ Phase M.3 Verification: ALL TESTS PASSED\n"
                  << "═══════════════════════════════════════════════════════════════\n"
                  << "\nPhase M.3 Semantic Hash Domains correctly enforced:\n"
                  << "  ✓ No implicit conversions between domains\n"
                  << "  ✓ All types are 32 bytes (storage-compatible)\n"
                  << "  ✓ All types are trivially copyable (performance)\n"
                  << "  ✓ Comparison operators work correctly\n"
                  << "  ✓ std::unordered_set/std::set support verified\n"
                  << "  ✓ IsNull() and AsUint256() accessors work\n"
                  << "  ✓ MerkleRoot::Compute() verified\n"
                  << "\nPhase M.3 is PRODUCTION READY.\n"
                  << std::endl;
    }

    return result;
}
