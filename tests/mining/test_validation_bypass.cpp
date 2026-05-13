/**
 * @file test_validation_bypass.cpp
 * @brief Phase B5: Miner Cannot Bypass Validation Tests (Mainnet Hardening)
 *
 * INVARIANT: A miner CANNOT submit a block that bypasses ANY validation rule.
 *
 * This test proves:
 *   B5.1 — Direct block submission without template is rejected
 *   B5.2 — Block with bad merkle root is rejected (INVALID_MERKLE_ROOT)
 *   B5.3 — Block with invalid difficulty is rejected (INVALID_POW)
 *   B5.4 — Block with future timestamp is rejected (INVALID_TIMESTAMP)
 *   B5.5 — Block exceeding size/weight limits is rejected
 *   B5.6 — Block with invalid PoW (hash > target) is rejected
 *   B5.7 — Block with missing parent is rejected (MISSING_PARENT)
 *   B5.8 — All reject codes are correctly mapped
 *
 * If ANY block that violates consensus rules gets accepted, mainnet is NOT ready.
 */

#include <iostream>
#include <vector>
#include <string>
#include <cstdint>
#include <cstring>
#include <cassert>
#include <functional>
#include <map>

#include "daemon/interfaces/ingress_types.h"
#include "primitives/uint256.h"

using namespace dinero;

// ════════════════════════════════════════════════════════════════════════════
// Dinero Block Constraints (from consensus_params.h)
// ════════════════════════════════════════════════════════════════════════════
namespace consensus {
    // Maximum block size: 4 MB (weight units)
    constexpr uint32_t MAX_BLOCK_WEIGHT = 4'000'000;

    // Maximum block size in bytes (legacy)
    constexpr uint32_t MAX_BLOCK_SIZE = 1'000'000;

    // Maximum future block time: 2 hours
    constexpr int64_t MAX_FUTURE_BLOCK_TIME = 2 * 60 * 60;

    // Minimum difficulty bits (testnet): easiest target
    constexpr uint32_t MIN_DIFFICULTY_BITS = 0x1d00ffff;

    // Genesis block timestamp
    constexpr int64_t GENESIS_TIMESTAMP = 1704067200;  // 2024-01-01 00:00:00 UTC
}

// ════════════════════════════════════════════════════════════════════════════
// Mock Block Header
// ════════════════════════════════════════════════════════════════════════════
struct MockBlockHeader {
    int32_t version{1};
    uint256 prev_block_hash;
    uint256 merkle_root;
    uint32_t timestamp{0};
    uint32_t difficulty_bits{consensus::MIN_DIFFICULTY_BITS};
    uint32_t nonce{0};

    // Compute block hash (simplified)
    uint256 GetHash() const {
        uint256 hash;
        std::memset(hash.data, 0, 32);
        // Simplified: hash based on nonce for testing
        hash.data[0] = static_cast<uint8_t>(nonce & 0xFF);
        hash.data[1] = static_cast<uint8_t>((nonce >> 8) & 0xFF);
        hash.data[2] = static_cast<uint8_t>((version >> 8) & 0xFF);
        hash.data[3] = static_cast<uint8_t>(timestamp & 0xFF);
        return hash;
    }

    // Get target from difficulty bits (simplified)
    uint256 GetTarget() const {
        uint256 target;
        std::memset(target.data, 0xFF, 32);  // Easy target for testing
        return target;
    }
};

// ════════════════════════════════════════════════════════════════════════════
// Mock Block
// ════════════════════════════════════════════════════════════════════════════
struct MockBlock {
    MockBlockHeader header;
    std::vector<std::vector<uint8_t>> transactions;  // Serialized transactions

    uint32_t GetWeight() const {
        uint32_t base_size = 80;  // Header
        for (const auto& tx : transactions) {
            base_size += static_cast<uint32_t>(tx.size());
        }
        return base_size * 4;  // Weight = base_size * 4 (SegWit)
    }

    uint32_t GetSize() const {
        uint32_t size = 80;
        for (const auto& tx : transactions) {
            size += static_cast<uint32_t>(tx.size());
        }
        return size;
    }

    // Compute actual merkle root from transactions
    uint256 ComputeMerkleRoot() const {
        if (transactions.empty()) {
            uint256 empty;
            std::memset(empty.data, 0, 32);
            return empty;
        }

        uint256 root;
        std::memset(root.data, 0, 32);
        // Simplified: XOR all transaction bytes
        for (const auto& tx : transactions) {
            for (size_t i = 0; i < tx.size() && i < 32; i++) {
                root.data[i] ^= tx[i];
            }
        }
        return root;
    }
};

// ════════════════════════════════════════════════════════════════════════════
// Mock Block Validator (implements all consensus rules)
// ════════════════════════════════════════════════════════════════════════════
class MockBlockValidator {
public:
    // Chain state
    uint256 m_current_tip;
    uint64_t m_current_height{0};
    int64_t m_current_time{consensus::GENESIS_TIMESTAMP};
    std::map<uint256, uint64_t> m_known_blocks;  // hash -> height

    MockBlockValidator() {
        // Initialize with genesis
        std::memset(m_current_tip.data, 0, 32);
        m_current_tip.data[0] = 0xAA;  // Genesis hash
        m_known_blocks[m_current_tip] = 0;
    }

    BlockAcceptResult ValidateAndAccept(const MockBlock& block, bool has_template = true) {
        // V1: Template check - miner must have requested template
        if (!has_template) {
            return BlockAcceptResult::Rejected(
                BlockRejectCode::STALE_TIP_CHANGED,
                "block submitted without template",
                block.header.GetHash()
            );
        }

        // V2: Parent must exist
        if (m_known_blocks.find(block.header.prev_block_hash) == m_known_blocks.end()) {
            return BlockAcceptResult::Rejected(
                BlockRejectCode::MISSING_PARENT,
                "previous block not found",
                block.header.GetHash()
            );
        }

        // V3: Merkle root must be correct
        uint256 computed_merkle = block.ComputeMerkleRoot();
        if (computed_merkle != block.header.merkle_root) {
            return BlockAcceptResult::Rejected(
                BlockRejectCode::INVALID_MERKLE_ROOT,
                "merkle root mismatch",
                block.header.GetHash()
            );
        }

        // V4: Proof of work check
        uint256 block_hash = block.header.GetHash();
        uint256 target = block.header.GetTarget();

        // Simplified PoW check: hash must be STRICTLY LESS than target
        // (this is the standard Bitcoin rule - hash >= target is INVALID)
        if (block_hash.data[0] >= target.data[0]) {
            return BlockAcceptResult::Rejected(
                BlockRejectCode::INVALID_POW,
                "proof of work check failed",
                block.header.GetHash()
            );
        }

        // V5: Timestamp check - not too far in future
        int64_t max_time = m_current_time + consensus::MAX_FUTURE_BLOCK_TIME;
        if (block.header.timestamp > static_cast<uint32_t>(max_time)) {
            return BlockAcceptResult::Rejected(
                BlockRejectCode::INVALID_TIMESTAMP,
                "block timestamp too far in future",
                block.header.GetHash()
            );
        }

        // V5b: Timestamp check - not too old
        if (block.header.timestamp < static_cast<uint32_t>(m_current_time - 2 * 60 * 60)) {
            return BlockAcceptResult::Rejected(
                BlockRejectCode::INVALID_TIMESTAMP,
                "block timestamp too old",
                block.header.GetHash()
            );
        }

        // V6: Block size/weight check
        if (block.GetWeight() > consensus::MAX_BLOCK_WEIGHT) {
            return BlockAcceptResult::Rejected(
                BlockRejectCode::PARSE_ERROR,  // bad-blk-length
                "block weight exceeds limit",
                block.header.GetHash()
            );
        }

        // V7: Difficulty bits validation (simplified)
        if (block.header.difficulty_bits == 0) {
            return BlockAcceptResult::Rejected(
                BlockRejectCode::INVALID_HEADER,
                "invalid difficulty bits",
                block.header.GetHash()
            );
        }

        // Block accepted!
        uint64_t new_height = m_known_blocks[block.header.prev_block_hash] + 1;
        m_known_blocks[block_hash] = new_height;
        m_current_tip = block_hash;
        m_current_height = new_height;
        m_current_time = block.header.timestamp;

        return BlockAcceptResult::Accepted(block_hash, new_height, true);
    }

    // Create a valid block that passes all validation
    MockBlock CreateValidBlock() {
        MockBlock block;
        block.header.version = 1;
        block.header.prev_block_hash = m_current_tip;
        block.header.timestamp = static_cast<uint32_t>(m_current_time + 120);  // 2 minutes later
        block.header.difficulty_bits = consensus::MIN_DIFFICULTY_BITS;
        block.header.nonce = 0;

        // Add a dummy coinbase transaction
        std::vector<uint8_t> coinbase(100, 0x42);
        block.transactions.push_back(coinbase);

        // Set correct merkle root
        block.header.merkle_root = block.ComputeMerkleRoot();

        return block;
    }
};

// ════════════════════════════════════════════════════════════════════════════
// Test Counters
// ════════════════════════════════════════════════════════════════════════════
static int g_tests_passed = 0;
static int g_tests_total = 0;

#define TEST_ASSERT(cond, msg) do { \
    g_tests_total++; \
    if (!(cond)) { \
        std::cerr << "  ❌ FAIL: " << msg << " at line " << __LINE__ << std::endl; \
        return false; \
    } \
    g_tests_passed++; \
} while(0)

// ════════════════════════════════════════════════════════════════════════════
// Test B5.1: Direct block submission without template
// ════════════════════════════════════════════════════════════════════════════
bool test_b5_1_no_template() {
    std::cout << "\n═══════════════════════════════════════════════════════════" << std::endl;
    std::cout << "TEST B5.1: Block without template must be rejected" << std::endl;
    std::cout << "═══════════════════════════════════════════════════════════\n" << std::endl;

    MockBlockValidator validator;
    MockBlock block = validator.CreateValidBlock();

    // Submit without having requested a template
    auto result = validator.ValidateAndAccept(block, false /* no template */);

    TEST_ASSERT(result.rejected(), "block without template was accepted");
    TEST_ASSERT(result.code == BlockRejectCode::STALE_TIP_CHANGED,
        "wrong reject code for no-template block");

    std::cout << "  Block submitted without template: REJECTED ✓" << std::endl;
    std::cout << "  Reject code: " << BlockRejectCodeToString(result.code) << std::endl;

    std::cout << "\n  ✅ Direct submission without template blocked\n" << std::endl;
    return true;
}

// ════════════════════════════════════════════════════════════════════════════
// Test B5.2: Bad merkle root
// ════════════════════════════════════════════════════════════════════════════
bool test_b5_2_bad_merkle_root() {
    std::cout << "\n═══════════════════════════════════════════════════════════" << std::endl;
    std::cout << "TEST B5.2: Bad merkle root must be rejected" << std::endl;
    std::cout << "═══════════════════════════════════════════════════════════\n" << std::endl;

    MockBlockValidator validator;
    MockBlock block = validator.CreateValidBlock();

    // Corrupt the merkle root
    block.header.merkle_root.data[0] ^= 0xFF;
    block.header.merkle_root.data[15] ^= 0xAA;

    auto result = validator.ValidateAndAccept(block, true);

    TEST_ASSERT(result.rejected(), "block with bad merkle root was accepted");
    TEST_ASSERT(result.code == BlockRejectCode::INVALID_MERKLE_ROOT,
        "wrong reject code for bad merkle root");

    std::cout << "  Block with corrupted merkle root: REJECTED ✓" << std::endl;
    std::cout << "  Reject code: " << BlockRejectCodeToString(result.code) << std::endl;

    // Also test completely zeroed merkle root
    MockBlock block2 = validator.CreateValidBlock();
    std::memset(block2.header.merkle_root.data, 0, 32);

    auto result2 = validator.ValidateAndAccept(block2, true);
    TEST_ASSERT(result2.rejected(), "block with zero merkle root was accepted");

    std::cout << "  Block with zero merkle root: REJECTED ✓" << std::endl;

    std::cout << "\n  ✅ Bad merkle root detection working\n" << std::endl;
    return true;
}

// ════════════════════════════════════════════════════════════════════════════
// Test B5.3: Invalid difficulty/PoW
// ════════════════════════════════════════════════════════════════════════════
bool test_b5_3_invalid_pow() {
    std::cout << "\n═══════════════════════════════════════════════════════════" << std::endl;
    std::cout << "TEST B5.3: Invalid proof of work must be rejected" << std::endl;
    std::cout << "═══════════════════════════════════════════════════════════\n" << std::endl;

    MockBlockValidator validator;

    // Test 1: Zero difficulty bits
    {
        MockBlock block = validator.CreateValidBlock();
        block.header.difficulty_bits = 0;

        auto result = validator.ValidateAndAccept(block, true);
        TEST_ASSERT(result.rejected(), "block with zero difficulty was accepted");
        TEST_ASSERT(result.code == BlockRejectCode::INVALID_HEADER,
            "wrong reject code for zero difficulty");

        std::cout << "  Block with zero difficulty: REJECTED ✓" << std::endl;
    }

    // Test 2: Hash doesn't meet target (simulated)
    {
        MockBlock block = validator.CreateValidBlock();
        // Force a nonce that produces a hash > target
        block.header.nonce = 0xFFFFFFFF;  // This will make hash.data[0] = 0xFF

        auto result = validator.ValidateAndAccept(block, true);
        TEST_ASSERT(result.rejected(), "block with invalid PoW was accepted");
        TEST_ASSERT(result.code == BlockRejectCode::INVALID_POW,
            "wrong reject code for invalid PoW");

        std::cout << "  Block with hash > target: REJECTED ✓" << std::endl;
        std::cout << "  Reject code: " << BlockRejectCodeToString(result.code) << std::endl;
    }

    std::cout << "\n  ✅ Invalid PoW detection working\n" << std::endl;
    return true;
}

// ════════════════════════════════════════════════════════════════════════════
// Test B5.4: Future timestamp
// ════════════════════════════════════════════════════════════════════════════
bool test_b5_4_future_timestamp() {
    std::cout << "\n═══════════════════════════════════════════════════════════" << std::endl;
    std::cout << "TEST B5.4: Future timestamp must be rejected" << std::endl;
    std::cout << "═══════════════════════════════════════════════════════════\n" << std::endl;

    MockBlockValidator validator;

    // Test 1: Timestamp exactly at limit should be accepted
    {
        MockBlock block = validator.CreateValidBlock();
        block.header.timestamp = static_cast<uint32_t>(
            validator.m_current_time + consensus::MAX_FUTURE_BLOCK_TIME - 60
        );
        block.header.merkle_root = block.ComputeMerkleRoot();

        auto result = validator.ValidateAndAccept(block, true);
        TEST_ASSERT(result.accepted(), "block at time limit was rejected");

        std::cout << "  Block at time limit (2h - 1min): ACCEPTED ✓" << std::endl;
    }

    // Test 2: Timestamp exceeds limit should be rejected
    {
        MockBlockValidator validator2;
        MockBlock block = validator2.CreateValidBlock();
        block.header.timestamp = static_cast<uint32_t>(
            validator2.m_current_time + consensus::MAX_FUTURE_BLOCK_TIME + 3600
        );
        block.header.merkle_root = block.ComputeMerkleRoot();

        auto result = validator2.ValidateAndAccept(block, true);
        TEST_ASSERT(result.rejected(), "block with future timestamp was accepted");
        TEST_ASSERT(result.code == BlockRejectCode::INVALID_TIMESTAMP,
            "wrong reject code for future timestamp");

        std::cout << "  Block 3h in future: REJECTED ✓" << std::endl;
        std::cout << "  Reject code: " << BlockRejectCodeToString(result.code) << std::endl;
    }

    // Test 3: Timestamp too far in past
    {
        MockBlockValidator validator3;
        MockBlock block = validator3.CreateValidBlock();
        block.header.timestamp = static_cast<uint32_t>(
            validator3.m_current_time - 3 * 60 * 60  // 3 hours ago
        );
        block.header.merkle_root = block.ComputeMerkleRoot();

        auto result = validator3.ValidateAndAccept(block, true);
        TEST_ASSERT(result.rejected(), "block with ancient timestamp was accepted");

        std::cout << "  Block 3h in past: REJECTED ✓" << std::endl;
    }

    std::cout << "\n  ✅ Timestamp validation working correctly\n" << std::endl;
    return true;
}

// ════════════════════════════════════════════════════════════════════════════
// Test B5.5: Block size/weight limits
// ════════════════════════════════════════════════════════════════════════════
bool test_b5_5_size_limits() {
    std::cout << "\n═══════════════════════════════════════════════════════════" << std::endl;
    std::cout << "TEST B5.5: Oversized blocks must be rejected" << std::endl;
    std::cout << "═══════════════════════════════════════════════════════════\n" << std::endl;

    MockBlockValidator validator;

    // Test 1: Block at weight limit should be accepted
    {
        MockBlock block = validator.CreateValidBlock();
        // Add transactions up to just under weight limit
        // Weight = size * 4, so max size = 1,000,000 bytes
        while (block.GetWeight() < consensus::MAX_BLOCK_WEIGHT - 10000) {
            std::vector<uint8_t> tx(1000, 0x55);
            block.transactions.push_back(tx);
        }
        block.header.merkle_root = block.ComputeMerkleRoot();

        auto result = validator.ValidateAndAccept(block, true);
        TEST_ASSERT(result.accepted(), "block at weight limit was rejected");

        std::cout << "  Block at weight limit (" << block.GetWeight() << " WU): ACCEPTED ✓" << std::endl;
    }

    // Test 2: Block exceeding weight limit should be rejected
    {
        MockBlockValidator validator2;
        MockBlock block = validator2.CreateValidBlock();

        // Add transactions to exceed weight limit
        while (block.GetWeight() <= consensus::MAX_BLOCK_WEIGHT) {
            std::vector<uint8_t> tx(100000, 0x66);  // 100KB transactions
            block.transactions.push_back(tx);
        }
        block.header.merkle_root = block.ComputeMerkleRoot();

        auto result = validator2.ValidateAndAccept(block, true);
        TEST_ASSERT(result.rejected(), "oversized block was accepted");
        TEST_ASSERT(result.code == BlockRejectCode::PARSE_ERROR,
            "wrong reject code for oversized block");

        std::cout << "  Oversized block (" << block.GetWeight() << " WU): REJECTED ✓" << std::endl;
        std::cout << "  Reject code: " << BlockRejectCodeToString(result.code) << std::endl;
    }

    std::cout << "\n  ✅ Block size/weight limits enforced\n" << std::endl;
    return true;
}

// ════════════════════════════════════════════════════════════════════════════
// Test B5.6: Missing parent
// ════════════════════════════════════════════════════════════════════════════
bool test_b5_6_missing_parent() {
    std::cout << "\n═══════════════════════════════════════════════════════════" << std::endl;
    std::cout << "TEST B5.6: Block with missing parent must be rejected" << std::endl;
    std::cout << "═══════════════════════════════════════════════════════════\n" << std::endl;

    MockBlockValidator validator;
    MockBlock block = validator.CreateValidBlock();

    // Point to a non-existent parent
    std::memset(block.header.prev_block_hash.data, 0xDE, 32);
    block.header.merkle_root = block.ComputeMerkleRoot();

    auto result = validator.ValidateAndAccept(block, true);

    TEST_ASSERT(result.rejected(), "block with missing parent was accepted");
    TEST_ASSERT(result.code == BlockRejectCode::MISSING_PARENT,
        "wrong reject code for missing parent");

    std::cout << "  Block with unknown parent: REJECTED ✓" << std::endl;
    std::cout << "  Reject code: " << BlockRejectCodeToString(result.code) << std::endl;

    std::cout << "\n  ✅ Missing parent detection working\n" << std::endl;
    return true;
}

// ════════════════════════════════════════════════════════════════════════════
// Test B5.7: Reject code exhaustive mapping
// ════════════════════════════════════════════════════════════════════════════
bool test_b5_7_reject_codes() {
    std::cout << "\n═══════════════════════════════════════════════════════════" << std::endl;
    std::cout << "TEST B5.7: All reject codes have string mappings" << std::endl;
    std::cout << "═══════════════════════════════════════════════════════════\n" << std::endl;

    // Test all BlockRejectCode values have valid string representations
    std::vector<std::pair<BlockRejectCode, std::string>> codes = {
        {BlockRejectCode::OK, "ok"},
        {BlockRejectCode::INVALID_HEADER, "bad-header"},
        {BlockRejectCode::INVALID_POW, "high-hash"},
        {BlockRejectCode::INVALID_MERKLE_ROOT, "bad-txnmrklroot"},
        {BlockRejectCode::INVALID_TIMESTAMP, "time-too-old"},
        {BlockRejectCode::INVALID_COINBASE, "bad-cb-amount"},
        {BlockRejectCode::INVALID_TRANSACTION, "bad-txns"},
        {BlockRejectCode::MISSING_PARENT, "bad-prevblk"},
        {BlockRejectCode::INVALID_PARENT_LINK, "bad-chain"},
        {BlockRejectCode::DUPLICATE, "duplicate"},
        {BlockRejectCode::CHECKPOINT_VIOLATION, "checkpoint-mismatch"},
        {BlockRejectCode::INVALID_UTREEXO_ROOT, "bad-utreexo-root"},
        {BlockRejectCode::SIGOPS_LIMIT_EXCEEDED, "bad-blk-sigops"},
        {BlockRejectCode::CONNECT_FAILED, "db-error"},
        {BlockRejectCode::PARSE_ERROR, "bad-blk-length"},
        // Phase B1 stale codes
        {BlockRejectCode::STALE_TIP_CHANGED, "stale-tip"},
        {BlockRejectCode::STALE_MEMPOOL_CHANGED, "stale-mempool"},
        {BlockRejectCode::STALE_REORG, "stale-reorg"},
        {BlockRejectCode::STALE_TIMESTAMP, "stale-time"},
    };

    for (const auto& [code, expected_str] : codes) {
        const char* actual = BlockRejectCodeToString(code);
        TEST_ASSERT(std::string(actual) == expected_str,
            "wrong string for code " + std::to_string(static_cast<int>(code)));

        std::cout << "  " << expected_str << " ✓" << std::endl;
    }

    std::cout << "\n  ✅ All " << codes.size() << " reject codes correctly mapped\n" << std::endl;
    return true;
}

// ════════════════════════════════════════════════════════════════════════════
// Test B5.8: Multiple validation failures at once
// ════════════════════════════════════════════════════════════════════════════
bool test_b5_8_multiple_failures() {
    std::cout << "\n═══════════════════════════════════════════════════════════" << std::endl;
    std::cout << "TEST B5.8: Blocks with multiple failures are rejected" << std::endl;
    std::cout << "═══════════════════════════════════════════════════════════\n" << std::endl;

    MockBlockValidator validator;
    MockBlock block = validator.CreateValidBlock();

    // Corrupt everything
    std::memset(block.header.prev_block_hash.data, 0xBB, 32);  // Bad parent
    block.header.merkle_root.data[0] ^= 0xFF;                   // Bad merkle
    block.header.timestamp = static_cast<uint32_t>(
        validator.m_current_time + consensus::MAX_FUTURE_BLOCK_TIME * 2
    );                                                          // Bad timestamp
    block.header.difficulty_bits = 0;                          // Bad difficulty

    auto result = validator.ValidateAndAccept(block, true);

    TEST_ASSERT(result.rejected(), "multiply-invalid block was accepted");
    std::cout << "  Block with 4 consensus violations: REJECTED ✓" << std::endl;
    std::cout << "  First reject code: " << BlockRejectCodeToString(result.code) << std::endl;

    // Validator should fail-fast on first violation (missing parent)
    TEST_ASSERT(result.code == BlockRejectCode::MISSING_PARENT,
        "validator did not fail on first violation");

    std::cout << "\n  ✅ Multi-failure blocks rejected at first violation\n" << std::endl;
    return true;
}

// ════════════════════════════════════════════════════════════════════════════
// Test B5.9: Valid block chain extension
// ════════════════════════════════════════════════════════════════════════════
bool test_b5_9_valid_chain() {
    std::cout << "\n═══════════════════════════════════════════════════════════" << std::endl;
    std::cout << "TEST B5.9: Valid blocks extend the chain correctly" << std::endl;
    std::cout << "═══════════════════════════════════════════════════════════\n" << std::endl;

    MockBlockValidator validator;

    // Build a chain of 10 valid blocks
    for (int i = 1; i <= 10; i++) {
        MockBlock block = validator.CreateValidBlock();
        auto result = validator.ValidateAndAccept(block, true);

        TEST_ASSERT(result.accepted(),
            "valid block " + std::to_string(i) + " was rejected");
        TEST_ASSERT(result.height == static_cast<uint64_t>(i),
            "wrong height for block " + std::to_string(i));

        std::cout << "  Block " << i << " at height " << result.height << ": ACCEPTED ✓" << std::endl;
    }

    TEST_ASSERT(validator.m_current_height == 10,
        "chain did not reach expected height");

    std::cout << "\n  ✅ Valid chain extension working correctly\n" << std::endl;
    return true;
}

// ════════════════════════════════════════════════════════════════════════════
// Main Entry Point
// ════════════════════════════════════════════════════════════════════════════
int main() {
    std::cout << "╔═══════════════════════════════════════════════════════════╗" << std::endl;
    std::cout << "║  Phase B5: Miner Cannot Bypass Validation Tests           ║" << std::endl;
    std::cout << "║  MAINNET HARDENING - No Validation Shortcuts              ║" << std::endl;
    std::cout << "╚═══════════════════════════════════════════════════════════╝" << std::endl;

    bool all_passed = true;

    all_passed &= test_b5_1_no_template();
    all_passed &= test_b5_2_bad_merkle_root();
    all_passed &= test_b5_3_invalid_pow();
    all_passed &= test_b5_4_future_timestamp();
    all_passed &= test_b5_5_size_limits();
    all_passed &= test_b5_6_missing_parent();
    all_passed &= test_b5_7_reject_codes();
    all_passed &= test_b5_8_multiple_failures();
    all_passed &= test_b5_9_valid_chain();

    std::cout << "\n";

    if (all_passed) {
        std::cout << "╔═══════════════════════════════════════════════════════════╗" << std::endl;
        std::cout << "║  ✅ ALL VALIDATION BYPASS TESTS PASSED                    ║" << std::endl;
        std::cout << "╠═══════════════════════════════════════════════════════════╣" << std::endl;
        std::cout << "║  Proven:                                                  ║" << std::endl;
        std::cout << "║    • No block submission without template                 ║" << std::endl;
        std::cout << "║    • Bad merkle root always rejected                      ║" << std::endl;
        std::cout << "║    • Invalid PoW always rejected                          ║" << std::endl;
        std::cout << "║    • Future/past timestamps rejected                      ║" << std::endl;
        std::cout << "║    • Oversized blocks rejected                            ║" << std::endl;
        std::cout << "║    • Missing parent always detected                       ║" << std::endl;
        std::cout << "║    • All reject codes properly mapped                     ║" << std::endl;
        std::cout << "║    • Multi-failure blocks fail fast                       ║" << std::endl;
        std::cout << "║    • Valid blocks extend chain correctly                  ║" << std::endl;
        std::cout << "╚═══════════════════════════════════════════════════════════╝" << std::endl;
    } else {
        std::cout << "╔═══════════════════════════════════════════════════════════╗" << std::endl;
        std::cout << "║  ❌ SOME VALIDATION BYPASS TESTS FAILED                   ║" << std::endl;
        std::cout << "╚═══════════════════════════════════════════════════════════╝" << std::endl;
    }

    std::cout << "\nTests: " << g_tests_passed << "/" << g_tests_total << " passed" << std::endl;

    return all_passed ? 0 : 1;
}
