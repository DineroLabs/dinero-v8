// Copyright (c) 2025 The Dinero Developers
// Phase Plan-A: Binary compact block serialization unit tests
//
// Tests:
// - CompactBlock serialize/deserialize round-trip
// - BlockTransactionsRequest serialize/deserialize round-trip
// - BlockTransactions serialize/deserialize round-trip
// - PrefilledTransaction serialize/deserialize round-trip
// - Deterministic serialization (same input → same output)
// - Malformed data handling (no crashes, returns empty/default)
// - Short txid computation consistency

#include <gtest/gtest.h>
#include "p2p/compact_block.h"
#include "primitives/block.h"
#include "primitives/transaction.h"
#include "primitives/uint256.h"
#include "primitives/amount.h"
#include "primitives/hash_domains.h"
#include <cstring>
#include <random>

namespace dinero {
namespace test {

// Helper: Create a test block header
BlockHeader createTestBlockHeader() {
    BlockHeader header;
    header.version = 1;
    std::memset(header.prev_block_hash.data, 0xAB, 32);
    std::memset(header.merkle_root.data, 0xCD, 32);
    std::memset(header.utreexo_root.data, 0x00, 32);
    header.timestamp = 1640995200;
    header.difficulty = 0x1d00ffff;
    header.nonce = 12345;
    std::memset(header.reserved, 0, sizeof(header.reserved));
    return header;
}

// Helper: Create a minimal test transaction
Transaction createTestTransaction(uint64_t salt) {
    Transaction tx;
    tx.version = 2;
    tx.lockTime = 0;

    // Add a simple input
    TxInput input;
    uint256 prev_hash;
    std::memset(prev_hash.data, static_cast<uint8_t>(salt & 0xFF), 32);
    input.prevout.txid = TxId(prev_hash);
    input.prevout.vout = static_cast<uint32_t>(salt);
    input.sequence = 0xFFFFFFFF;
    tx.vin.push_back(input);

    // Add a simple output
    TxOutput output;
    output.value = AmountUna::DIN(50);  // 50 DIN
    output.scriptPubKey = {0x76, 0xa9, 0x14};  // Minimal P2PKH prefix
    for (int i = 0; i < 20; ++i) {
        output.scriptPubKey.push_back(static_cast<uint8_t>((salt + i) & 0xFF));
    }
    output.scriptPubKey.push_back(0x88);
    output.scriptPubKey.push_back(0xac);
    tx.vout.push_back(output);

    return tx;
}

// Helper: Create a test CompactBlock
CompactBlock createTestCompactBlock(size_t num_short_txids = 5) {
    CompactBlock compact;
    compact.header = createTestBlockHeader();
    compact.nonce = 0xDEADBEEF12345678ULL;

    // Add prefilled coinbase (index 0)
    compact.prefilled.emplace_back(0, createTestTransaction(0));

    // Add short txids
    for (size_t i = 0; i < num_short_txids; ++i) {
        compact.short_txids.push_back(0xAABBCCDDEE00ULL + i);
    }

    return compact;
}

// ============================================================================
// CompactBlock Tests
// ============================================================================

TEST(CompactBlockBinary, SerializeDeserializeRoundTrip) {
    CompactBlock original = createTestCompactBlock(10);

    auto bytes = original.Serialize();
    ASSERT_GT(bytes.size(), 0u);

    CompactBlock result = CompactBlock::Deserialize(bytes);

    EXPECT_EQ(result.header.GetHash(), original.header.GetHash());
    EXPECT_EQ(result.nonce, original.nonce);
    ASSERT_EQ(result.short_txids.size(), original.short_txids.size());
    for (size_t i = 0; i < result.short_txids.size(); ++i) {
        EXPECT_EQ(result.short_txids[i], original.short_txids[i]);
    }
    ASSERT_EQ(result.prefilled.size(), original.prefilled.size());
    EXPECT_EQ(result.prefilled[0].index, original.prefilled[0].index);
}

TEST(CompactBlockBinary, DeterministicSerialization) {
    CompactBlock block = createTestCompactBlock();

    auto bytes1 = block.Serialize();
    auto bytes2 = block.Serialize();

    EXPECT_EQ(bytes1, bytes2) << "Serialization must be deterministic";
}

TEST(CompactBlockBinary, EmptyBlock) {
    CompactBlock empty;
    empty.header = createTestBlockHeader();
    empty.nonce = 0;

    auto bytes = empty.Serialize();
    ASSERT_GT(bytes.size(), 0u);

    CompactBlock result = CompactBlock::Deserialize(bytes);
    EXPECT_EQ(result.header.GetHash(), empty.header.GetHash());
    EXPECT_EQ(result.nonce, 0u);
    EXPECT_TRUE(result.short_txids.empty());
    EXPECT_TRUE(result.prefilled.empty());
}

TEST(CompactBlockBinary, MalformedDataReturnsEmpty) {
    // Test with garbage data
    std::vector<uint8_t> garbage = {0x00, 0x01, 0x02};
    auto result = CompactBlock::Deserialize(garbage);
    EXPECT_TRUE(result.short_txids.empty());

    // Test with truncated header
    std::vector<uint8_t> truncated(50, 0xAB);
    result = CompactBlock::Deserialize(truncated);
    EXPECT_TRUE(result.short_txids.empty());

    // Empty data
    std::vector<uint8_t> empty;
    result = CompactBlock::Deserialize(empty);
    EXPECT_TRUE(result.short_txids.empty());
}

TEST(CompactBlockBinary, LargeBlock) {
    CompactBlock large = createTestCompactBlock(2000);  // 2000 short txids

    auto bytes = large.Serialize();
    ASSERT_GT(bytes.size(), 0u);

    CompactBlock result = CompactBlock::Deserialize(bytes);
    EXPECT_EQ(result.short_txids.size(), 2000u);
    EXPECT_EQ(result.nonce, large.nonce);
}

TEST(CompactBlockBinary, ShortTxId48Bit) {
    CompactBlock compact;
    compact.header = createTestBlockHeader();
    compact.nonce = 0;

    // Test that 48-bit (6-byte) short txids are preserved
    compact.short_txids.push_back(0xFFFFFFFFFFFFULL);  // Max 48-bit value
    compact.short_txids.push_back(0x000000000000ULL);  // Min value
    compact.short_txids.push_back(0xAABBCCDDEEFFULL);  // Arbitrary value

    auto bytes = compact.Serialize();
    CompactBlock result = CompactBlock::Deserialize(bytes);

    ASSERT_EQ(result.short_txids.size(), 3u);
    EXPECT_EQ(result.short_txids[0], 0xFFFFFFFFFFFFULL);
    EXPECT_EQ(result.short_txids[1], 0x000000000000ULL);
    EXPECT_EQ(result.short_txids[2], 0xAABBCCDDEEFFULL);
}

TEST(CompactBlockBinary, CompleteReconstructionPreservesPartiallyRecoveredTransactions) {
    Block full_block;
    full_block.header = createTestBlockHeader();
    full_block.vtx = {
        createTestTransaction(0),
        createTestTransaction(1),
        createTestTransaction(2)
    };

    Block partial_block = full_block;
    partial_block.vtx[2] = Transaction{};

    std::vector<uint32_t> missing_indexes = {2};
    std::vector<Transaction> missing_txs = {full_block.vtx[2]};

    auto reconstructed = CompactBlockCodec::CompleteReconstruction(
        partial_block,
        missing_txs,
        missing_indexes
    );

    ASSERT_TRUE(reconstructed.has_value());
    ASSERT_EQ(reconstructed->vtx.size(), full_block.vtx.size());
    EXPECT_EQ(reconstructed->vtx[0].GetTxid(), full_block.vtx[0].GetTxid());
    EXPECT_EQ(reconstructed->vtx[1].GetTxid(), full_block.vtx[1].GetTxid());
    EXPECT_EQ(reconstructed->vtx[2].GetTxid(), full_block.vtx[2].GetTxid());
}

// ============================================================================
// BlockTransactionsRequest Tests
// ============================================================================

TEST(BlockTransactionsRequestBinary, RoundTrip) {
    BlockTransactionsRequest req;
    std::memset(req.block_hash.data, 0xAB, 32);
    req.indexes = {0, 5, 10, 15, 100, 1000};

    auto bytes = req.Serialize();
    ASSERT_GT(bytes.size(), 0u);

    auto result = BlockTransactionsRequest::Deserialize(bytes);

    EXPECT_EQ(result.block_hash, req.block_hash);
    ASSERT_EQ(result.indexes.size(), req.indexes.size());
    for (size_t i = 0; i < result.indexes.size(); ++i) {
        EXPECT_EQ(result.indexes[i], req.indexes[i]);
    }
}

TEST(BlockTransactionsRequestBinary, DeterministicSerialization) {
    BlockTransactionsRequest req;
    std::memset(req.block_hash.data, 0xCD, 32);
    req.indexes = {1, 2, 3};

    auto bytes1 = req.Serialize();
    auto bytes2 = req.Serialize();

    EXPECT_EQ(bytes1, bytes2);
}

TEST(BlockTransactionsRequestBinary, EmptyIndexes) {
    BlockTransactionsRequest req;
    std::memset(req.block_hash.data, 0xEF, 32);
    req.indexes.clear();

    auto bytes = req.Serialize();
    auto result = BlockTransactionsRequest::Deserialize(bytes);

    EXPECT_EQ(result.block_hash, req.block_hash);
    EXPECT_TRUE(result.indexes.empty());
}

TEST(BlockTransactionsRequestBinary, MalformedData) {
    // Too short for block hash
    std::vector<uint8_t> short_data(20, 0x00);
    auto result = BlockTransactionsRequest::Deserialize(short_data);
    EXPECT_TRUE(result.indexes.empty());
}

// ============================================================================
// BlockTransactions Tests
// ============================================================================

TEST(BlockTransactionsBinary, RoundTrip) {
    BlockTransactions txns;
    std::memset(txns.block_hash.data, 0x12, 32);
    txns.transactions.push_back(createTestTransaction(1));
    txns.transactions.push_back(createTestTransaction(2));
    txns.transactions.push_back(createTestTransaction(3));

    auto bytes = txns.Serialize();
    ASSERT_GT(bytes.size(), 0u);

    auto result = BlockTransactions::Deserialize(bytes);

    EXPECT_EQ(result.block_hash, txns.block_hash);
    EXPECT_EQ(result.transactions.size(), txns.transactions.size());
}

TEST(BlockTransactionsBinary, EmptyTransactions) {
    BlockTransactions txns;
    std::memset(txns.block_hash.data, 0x34, 32);

    auto bytes = txns.Serialize();
    auto result = BlockTransactions::Deserialize(bytes);

    EXPECT_EQ(result.block_hash, txns.block_hash);
    EXPECT_TRUE(result.transactions.empty());
}

// ============================================================================
// PrefilledTransaction Tests
// ============================================================================

TEST(PrefilledTransactionBinary, RoundTrip) {
    PrefilledTransaction prefilled(5, createTestTransaction(42));

    auto bytes = prefilled.Serialize();
    ASSERT_GT(bytes.size(), 0u);

    size_t offset = 0;
    auto result = PrefilledTransaction::Deserialize(bytes, offset);

    EXPECT_EQ(result.index, prefilled.index);
    EXPECT_EQ(offset, bytes.size());
}

TEST(PrefilledTransactionBinary, LargeIndex) {
    PrefilledTransaction prefilled(65535, createTestTransaction(99));

    auto bytes = prefilled.Serialize();
    size_t offset = 0;
    auto result = PrefilledTransaction::Deserialize(bytes, offset);

    EXPECT_EQ(result.index, 65535u);
}

// ============================================================================
// Short TxId Computation Tests
// ============================================================================

TEST(ShortTxIdComputation, Consistency) {
    uint256 block_hash;
    std::memset(block_hash.data, 0xAB, 32);

    uint256 txid;
    std::memset(txid.data, 0xCD, 32);

    uint64_t nonce = 0x123456789ABCDEFULL;

    // Same inputs should produce same output
    uint64_t short_id1 = CompactBlockCodec::ComputeShortTxId(block_hash, nonce, txid);
    uint64_t short_id2 = CompactBlockCodec::ComputeShortTxId(block_hash, nonce, txid);

    EXPECT_EQ(short_id1, short_id2);

    // Result should be 48-bit (upper 16 bits should be zero)
    EXPECT_EQ(short_id1 & 0xFFFF000000000000ULL, 0u);
}

TEST(ShortTxIdComputation, DifferentNoncesDifferentResults) {
    uint256 block_hash;
    std::memset(block_hash.data, 0xAB, 32);

    uint256 txid;
    std::memset(txid.data, 0xCD, 32);

    uint64_t short_id1 = CompactBlockCodec::ComputeShortTxId(block_hash, 1, txid);
    uint64_t short_id2 = CompactBlockCodec::ComputeShortTxId(block_hash, 2, txid);

    EXPECT_NE(short_id1, short_id2);
}

TEST(ShortTxIdComputation, DifferentTxidsDifferentResults) {
    uint256 block_hash;
    std::memset(block_hash.data, 0xAB, 32);

    uint256 txid1;
    std::memset(txid1.data, 0x01, 32);

    uint256 txid2;
    std::memset(txid2.data, 0x02, 32);

    uint64_t nonce = 12345;

    uint64_t short_id1 = CompactBlockCodec::ComputeShortTxId(block_hash, nonce, txid1);
    uint64_t short_id2 = CompactBlockCodec::ComputeShortTxId(block_hash, nonce, txid2);

    EXPECT_NE(short_id1, short_id2);
}

// ============================================================================
// Wire Format Size Tests
// ============================================================================

TEST(CompactBlockBinary, WireFormatSize) {
    CompactBlock compact = createTestCompactBlock(100);

    auto bytes = compact.Serialize();

    // Expected minimum size:
    // - Header: 128 bytes (BlockHeader)
    // - Nonce: 8 bytes
    // - Short txids count: 1+ bytes (CompactSize)
    // - Short txids: 100 * 6 = 600 bytes
    // - Prefilled count: 1+ bytes
    // - Prefilled tx: variable

    EXPECT_GE(bytes.size(), 128u + 8u + 1u + 600u + 1u);
}

TEST(BlockTransactionsRequestBinary, WireFormatSize) {
    BlockTransactionsRequest req;
    std::memset(req.block_hash.data, 0x00, 32);
    req.indexes = {0, 1, 2};

    auto bytes = req.Serialize();

    // Expected: 32 (hash) + 1 (count) + 3 (indexes as CompactSize 1 byte each)
    EXPECT_GE(bytes.size(), 36u);
}

// ============================================================================
// Edge Cases
// ============================================================================

TEST(CompactBlockBinary, MaxShortTxIds) {
    CompactBlock compact;
    compact.header = createTestBlockHeader();
    compact.nonce = 0;

    // BIP152 allows up to ~10000 transactions per block
    // Test with a reasonable large number
    for (int i = 0; i < 5000; ++i) {
        compact.short_txids.push_back(static_cast<uint64_t>(i) | 0xAA0000000000ULL);
    }

    auto bytes = compact.Serialize();
    auto result = CompactBlock::Deserialize(bytes);

    EXPECT_EQ(result.short_txids.size(), 5000u);
    EXPECT_EQ(result.short_txids[0], 0xAA0000000000ULL);
    EXPECT_EQ(result.short_txids[4999], 4999ULL | 0xAA0000000000ULL);
}

TEST(CompactBlockBinary, MultiplePrefilledTransactions) {
    CompactBlock compact;
    compact.header = createTestBlockHeader();
    compact.nonce = 0;

    // Prefill multiple transactions (coinbase + others not in mempool)
    compact.prefilled.emplace_back(0, createTestTransaction(0));
    compact.prefilled.emplace_back(5, createTestTransaction(5));
    compact.prefilled.emplace_back(10, createTestTransaction(10));

    // Short txids for remaining slots
    compact.short_txids = {0x111111111111ULL, 0x222222222222ULL};

    auto bytes = compact.Serialize();
    auto result = CompactBlock::Deserialize(bytes);

    EXPECT_EQ(result.prefilled.size(), 3u);
    EXPECT_EQ(result.prefilled[0].index, 0u);
    EXPECT_EQ(result.prefilled[1].index, 5u);
    EXPECT_EQ(result.prefilled[2].index, 10u);
    EXPECT_EQ(result.short_txids.size(), 2u);
}

// ============================================================================
// Randomized Fuzz Tests (Quick fuzzing without libFuzzer)
// ============================================================================

TEST(CompactBlockFuzz, RandomDataNeverCrashes) {
    // Quick fuzz test: feed random data, must never crash
    std::random_device rd;
    std::mt19937 gen(rd());
    // C++20 [rand.req.genl]/1.5 — std::uniform_int_distribution requires a
    // type from {short, int, long, long long, unsigned variants of those}.
    // uint8_t is explicitly disallowed; libstdc++ permits it as an extension
    // but MSVC enforces the standard. Use uint16_t and narrow on use.
    std::uniform_int_distribution<uint16_t> byte_dist(0, 255);
    std::uniform_int_distribution<size_t> size_dist(0, 2000);

    const size_t iterations = 1000;
    size_t successful_parses = 0;

    for (size_t i = 0; i < iterations; ++i) {
        size_t size = size_dist(gen);
        std::vector<uint8_t> data(size);
        for (size_t j = 0; j < size; ++j) {
            data[j] = static_cast<uint8_t>(byte_dist(gen));
        }

        // These must never crash, regardless of input
        auto compact = CompactBlock::Deserialize(data);
        auto request = BlockTransactionsRequest::Deserialize(data);
        auto txns = BlockTransactions::Deserialize(data);

        if (!compact.short_txids.empty() || !compact.prefilled.empty()) {
            successful_parses++;
        }
    }

    // We expect most random data to fail parsing (which is fine)
    // The test passes if we didn't crash
    SUCCEED() << "Processed " << iterations << " random inputs without crashing. "
              << "Successful parses: " << successful_parses;
}

TEST(CompactBlockFuzz, RoundTripConsistency) {
    // Generate valid compact blocks, serialize, deserialize, verify match
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<uint64_t> nonce_dist;
    std::uniform_int_distribution<size_t> txid_count_dist(0, 100);
    std::uniform_int_distribution<uint64_t> short_txid_dist(0, 0xFFFFFFFFFFFFULL);

    const size_t iterations = 100;
    size_t round_trip_success = 0;

    for (size_t i = 0; i < iterations; ++i) {
        CompactBlock original;
        original.header = createTestBlockHeader();
        original.nonce = nonce_dist(gen);

        // Add random short txids
        size_t txid_count = txid_count_dist(gen);
        for (size_t j = 0; j < txid_count; ++j) {
            original.short_txids.push_back(short_txid_dist(gen));
        }

        // Serialize and deserialize
        auto bytes = original.Serialize();
        auto result = CompactBlock::Deserialize(bytes);

        // Verify round-trip
        ASSERT_EQ(result.nonce, original.nonce);
        ASSERT_EQ(result.short_txids.size(), original.short_txids.size());
        for (size_t j = 0; j < original.short_txids.size(); ++j) {
            ASSERT_EQ(result.short_txids[j], original.short_txids[j]);
        }

        // Verify determinism (serialize twice, same result)
        auto bytes2 = original.Serialize();
        ASSERT_EQ(bytes, bytes2);

        round_trip_success++;
    }

    EXPECT_EQ(round_trip_success, iterations);
}

TEST(CompactBlockFuzz, BlockTransactionsRequestRoundTrip) {
    std::random_device rd;
    std::mt19937 gen(rd());
    // uniform_int_distribution<uint8_t> is UB per the C++ standard; use
    // uint16_t and narrow at the call-site. See the matching note in
    // CompactBlockFuzz.RandomDataNeverCrashes above.
    std::uniform_int_distribution<uint16_t> byte_dist(0, 255);
    std::uniform_int_distribution<size_t> index_count_dist(0, 50);
    std::uniform_int_distribution<uint32_t> index_dist(0, 10000);

    const size_t iterations = 100;

    for (size_t i = 0; i < iterations; ++i) {
        BlockTransactionsRequest original;

        // Random block hash
        for (int j = 0; j < 32; ++j) {
            original.block_hash.data[j] = static_cast<uint8_t>(byte_dist(gen));
        }

        // Random indexes
        size_t index_count = index_count_dist(gen);
        for (size_t j = 0; j < index_count; ++j) {
            original.indexes.push_back(index_dist(gen));
        }

        // Round-trip
        auto bytes = original.Serialize();
        auto result = BlockTransactionsRequest::Deserialize(bytes);

        ASSERT_EQ(result.block_hash, original.block_hash);
        ASSERT_EQ(result.indexes.size(), original.indexes.size());
        for (size_t j = 0; j < original.indexes.size(); ++j) {
            ASSERT_EQ(result.indexes[j], original.indexes[j]);
        }

        // Determinism
        auto bytes2 = original.Serialize();
        ASSERT_EQ(bytes, bytes2);
    }
}

} // namespace test
} // namespace dinero

// Main entry point for Google Test
int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
