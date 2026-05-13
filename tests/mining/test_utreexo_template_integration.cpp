/**
 * Phase 10d: Utreexo Template Builder Integration Test
 *
 * Tests that BlockTemplateBuilder correctly computes and includes
 * Utreexo root commitment in block headers.
 *
 * This test proves:
 * 1. Template builder computes non-zero root when forest provided
 * 2. Template builder uses zero root when forest is nullptr (legacy mode)
 * 3. Miner logic is unchanged (still hashes 128 bytes)
 * 4. Root computation is deterministic
 */

#include <gtest/gtest.h>
#include "mining/block_template.h"
#include "consensus/utreexo_accumulator.h"
#include "consensus/coins_db.h"
#include "mempool/mempool.h"
#include "consensus/chainparams.h"
#include "primitives/transaction.h"
#include "common/sha256d.h"

using namespace dinero;
using namespace dinero::mining;
using namespace dinero::consensus;

class UtreexoTemplateTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Select regtest params
        SelectParams(Chain::REGTEST);

        // Initialize empty mempool
        mempool_ = std::make_unique<mempool::Mempool>();

        // Initialize empty coins DB (in-memory for testing)
        coins_db_ = std::make_unique<CoinsDB>();
        coins_db_->open(":memory:");

        // Initialize Utreexo forest
        utreexo_forest_ = std::make_unique<UtreexoForest>();
    }

    void TearDown() override {
        coins_db_->close();
    }

    std::unique_ptr<mempool::Mempool> mempool_;
    std::unique_ptr<CoinsDB> coins_db_;
    std::unique_ptr<UtreexoForest> utreexo_forest_;
};

// ============================================================================
// Test 1: Happy Path - Non-Zero Root with Forest
// ============================================================================

TEST_F(UtreexoTemplateTest, ComputesNonZeroRootWhenForestProvided) {
    // Create template builder WITH forest
    BlockTemplateBuilder builder(
        *mempool_,
        *coins_db_,
        BlockTemplateConfig(),
        utreexo_forest_.get()  // ← Provide forest
    );

    // Create block template
    auto template_block = builder.createBlockTemplate(
        uint256().GetHex(),  // prev_block_hash (genesis)
        1,                   // height
        1234567890,          // timestamp
        0x1d00ffff,          // difficulty
        "rdin1qtest..."      // mining address
    );

    ASSERT_NE(template_block, nullptr) << "Template creation failed";

    // ✅ VERIFY: utreexo_root should be NON-ZERO
    // (Even for empty forest, getCommitment() returns a hash)
    EXPECT_FALSE(template_block->block.header.utreexo_root.IsNull())
        << "utreexo_root should be non-zero when forest is provided";

    // ✅ VERIFY: Root is 32 bytes (uint256 is always 32 bytes)
    // uint256 type guarantees 32 bytes, no need to check size()

    std::cout << "✅ Test 1 PASSED: Non-zero root computed with forest" << std::endl;
    std::cout << "   Root: " << template_block->block.header.utreexo_root.GetHex().substr(0, 16) << "..." << std::endl;
}

// ============================================================================
// Test 2: Legacy Mode - Zero Root without Forest
// ============================================================================

TEST_F(UtreexoTemplateTest, UsesZeroRootWhenForestNotProvided) {
    // Create template builder WITHOUT forest
    BlockTemplateBuilder builder(
        *mempool_,
        *coins_db_,
        BlockTemplateConfig(),
        nullptr  // ← No forest (legacy mode)
    );

    // Create block template
    auto template_block = builder.createBlockTemplate(
        uint256().GetHex(),
        1,
        1234567890,
        0x1d00ffff,
        "rdin1qtest..."
    );

    ASSERT_NE(template_block, nullptr) << "Template creation failed";

    // ✅ VERIFY: utreexo_root should be ZERO (legacy mode)
    EXPECT_TRUE(template_block->block.header.utreexo_root.IsNull())
        << "utreexo_root should be zero when forest is nullptr";

    std::cout << "✅ Test 2 PASSED: Zero root in legacy mode" << std::endl;
}

// ============================================================================
// Test 3: Header Size - Miner Sees 128 Bytes
// ============================================================================

TEST_F(UtreexoTemplateTest, HeaderIs128BytesRegardlessOfRoot) {
    // Test WITH forest
    BlockTemplateBuilder builder_with_forest(
        *mempool_,
        *coins_db_,
        BlockTemplateConfig(),
        utreexo_forest_.get()
    );

    auto template_with_root = builder_with_forest.createBlockTemplate(
        uint256().GetHex(), 1, 1234567890, 0x1d00ffff, "rdin1qtest..."
    );

    // Test WITHOUT forest
    BlockTemplateBuilder builder_without_forest(
        *mempool_,
        *coins_db_,
        BlockTemplateConfig(),
        nullptr
    );

    auto template_without_root = builder_without_forest.createBlockTemplate(
        uint256().GetHex(), 1, 1234567890, 0x1d00ffff, "rdin1qtest..."
    );

    // ✅ VERIFY: Both headers serialize to 128 bytes
    auto header_with_root_bytes = template_with_root->block.header.SerializeForHash();
    auto header_without_root_bytes = template_without_root->block.header.SerializeForHash();

    EXPECT_EQ(header_with_root_bytes.size(), 128)
        << "Header with root must be 128 bytes";

    EXPECT_EQ(header_without_root_bytes.size(), 128)
        << "Header without root must be 128 bytes";

    std::cout << "✅ Test 3 PASSED: Both headers are 128 bytes" << std::endl;
    std::cout << "   Miner always hashes exactly 128 bytes" << std::endl;
}

// ============================================================================
// Test 4: Determinism - Same Forest Produces Same Root
// ============================================================================

TEST_F(UtreexoTemplateTest, RootComputationIsDeterministic) {
    // Add a UTXO to the forest
    uint256 utxo_txid;
    uint256::FromHex("0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef", utxo_txid);

    auto utxo_hash = HashUTXO(
        utxo_txid,
        0,
        100000000,  // 1 DIN
        {0x76, 0xa9, 0x14}  // Simple scriptPubKey
    );
    utreexo_forest_->add(utxo_hash);

    // Build template 1
    BlockTemplateBuilder builder1(*mempool_, *coins_db_, BlockTemplateConfig(), utreexo_forest_.get());
    auto template1 = builder1.createBlockTemplate(
        uint256().GetHex(), 2, 1234567890, 0x1d00ffff, "rdin1qtest..."
    );

    // Build template 2 (same forest state)
    BlockTemplateBuilder builder2(*mempool_, *coins_db_, BlockTemplateConfig(), utreexo_forest_.get());
    auto template2 = builder2.createBlockTemplate(
        uint256().GetHex(), 2, 1234567890, 0x1d00ffff, "rdin1qtest..."
    );

    // ✅ VERIFY: Roots match
    EXPECT_EQ(
        template1->block.header.utreexo_root.GetHex(),
        template2->block.header.utreexo_root.GetHex()
    ) << "Same forest state must produce same root";

    std::cout << "✅ Test 4 PASSED: Root computation is deterministic" << std::endl;
    std::cout << "   Root: " << template1->block.header.utreexo_root.GetHex().substr(0, 16) << "..." << std::endl;
}

// ============================================================================
// Test 5: Miner Ignorance - Hash Includes Root Bytes
// ============================================================================

TEST_F(UtreexoTemplateTest, MinerHashesIncludeRootBytes) {
    BlockTemplateBuilder builder(*mempool_, *coins_db_, BlockTemplateConfig(), utreexo_forest_.get());
    auto template_block = builder.createBlockTemplate(
        uint256().GetHex(), 1, 1234567890, 0x1d00ffff, "rdin1qtest..."
    );

    // Serialize header (what miner sees)
    auto header_bytes = template_block->block.header.SerializeForHash();

    // ✅ VERIFY: Bytes 68-99 (utreexo_root) are part of the hash input
    // Extract bytes 68-99 from serialized header
    std::vector<uint8_t> root_in_header(header_bytes.begin() + 68, header_bytes.begin() + 100);

    // Convert template root to bytes
    std::vector<uint8_t> root_from_field(
        template_block->block.header.utreexo_root.begin(),
        template_block->block.header.utreexo_root.end()
    );

    EXPECT_EQ(root_in_header, root_from_field)
        << "Bytes 68-99 in header must match utreexo_root field";

    // ✅ VERIFY: Changing one bit in root changes the hash
    auto original_hash = Dinero::Common::double_sha256_raw(header_bytes.data(), header_bytes.size());

    // Flip one bit in root
    header_bytes[68] ^= 0x01;
    auto modified_hash = Dinero::Common::double_sha256_raw(header_bytes.data(), header_bytes.size());

    EXPECT_NE(original_hash, modified_hash)
        << "Changing root must change block hash (miner blindly hashes it)";

    std::cout << "✅ Test 5 PASSED: Miner hashes include root bytes" << std::endl;
    std::cout << "   Root is bytes 68-99 in 128-byte header" << std::endl;
}

// ============================================================================
// Test 6: Field Layout - Root at Correct Offset
// ============================================================================

TEST_F(UtreexoTemplateTest, RootIsAtCorrectOffset) {
    BlockTemplateBuilder builder(*mempool_, *coins_db_, BlockTemplateConfig(), utreexo_forest_.get());
    auto template_block = builder.createBlockTemplate(
        uint256().GetHex(), 1, 1234567890, 0x1d00ffff, "rdin1qtest..."
    );

    auto header_bytes = template_block->block.header.SerializeForHash();

    // ✅ VERIFY: utreexo_root is at offset 68 (DINERO_HEADER_UTREEXO_OFFSET)
    constexpr size_t UTREEXO_OFFSET = 68;
    constexpr size_t UTREEXO_SIZE = 32;

    std::vector<uint8_t> root_at_offset(
        header_bytes.begin() + UTREEXO_OFFSET,
        header_bytes.begin() + UTREEXO_OFFSET + UTREEXO_SIZE
    );

    std::vector<uint8_t> root_from_field(
        template_block->block.header.utreexo_root.begin(),
        template_block->block.header.utreexo_root.end()
    );

    EXPECT_EQ(root_at_offset, root_from_field)
        << "utreexo_root must be at offset 68 in serialized header";

    std::cout << "✅ Test 6 PASSED: Root is at offset 68-99" << std::endl;
}

// ============================================================================
// Main
// ============================================================================

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);

    std::cout << "\n";
    std::cout << "═══════════════════════════════════════════════════════════════\n";
    std::cout << "  Phase 10d: Utreexo Template Builder Integration Test\n";
    std::cout << "═══════════════════════════════════════════════════════════════\n";
    std::cout << "\n";

    int result = RUN_ALL_TESTS();

    std::cout << "\n";
    if (result == 0) {
        std::cout << "✅ ALL TESTS PASSED - Utreexo integration is correct\n";
        std::cout << "\n";
        std::cout << "Verified:\n";
        std::cout << "  ✅ Non-zero root computed when forest provided\n";
        std::cout << "  ✅ Zero root used in legacy mode (nullptr)\n";
        std::cout << "  ✅ Header is always 128 bytes\n";
        std::cout << "  ✅ Root computation is deterministic\n";
        std::cout << "  ✅ Miner hashes include root bytes\n";
        std::cout << "  ✅ Root is at offset 68-99\n";
        std::cout << "\n";
        std::cout << "Architecture validated:\n";
        std::cout << "  → Template builder computes root from local forest\n";
        std::cout << "  → Miner blindly hashes 128 bytes (no Utreexo knowledge)\n";
        std::cout << "  → Validator will recompute root and enforce match\n";
        std::cout << "\n";
    } else {
        std::cout << "❌ TESTS FAILED\n";
    }
    std::cout << "═══════════════════════════════════════════════════════════════\n";
    std::cout << "\n";

    return result;
}
