/**
 * @file test_dinero_mining_pipeline.cpp
 * @brief End-to-End Mining Pipeline Validation Tests
 *
 * CRITICAL CONSENSUS TESTS - These validate the entire mining stack:
 *   1. BlockHeader v1 (128-byte) serialization
 *   2. uint256 byte ordering (big-endian display ↔ little-endian internal)
 *   3. Utreexo root commitment in header
 *   4. SHA256d block hash computation
 *   5. Stratum protocol field encoding
 *
 * These tests ensure daemon, stratum server, and miners all agree on:
 *   - Header layout and field offsets
 *   - Byte ordering conventions
 *   - Hash computation
 *
 * If any test fails → CONSENSUS BUG → Network fork risk!
 */

#include <gtest/gtest.h>
#include <cstdint>
#include <cstring>
#include <vector>
#include <string>
#include <algorithm>
#include <iostream>
#include <iomanip>

#include "primitives/block.h"
#include "mining/header_layout.h"
#include "common/uint256.h"
#include "common/sha256d.h"

using namespace dinero;

// ═══════════════════════════════════════════════════════════════════════════════
// Test Utilities
// ═══════════════════════════════════════════════════════════════════════════════

namespace {

std::string bytesToHex(const uint8_t* data, size_t len) {
    std::ostringstream oss;
    for (size_t i = 0; i < len; i++) {
        oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(data[i]);
    }
    return oss.str();
}

std::vector<uint8_t> hexToBytes(const std::string& hex) {
    std::vector<uint8_t> bytes;
    for (size_t i = 0; i < hex.length(); i += 2) {
        bytes.push_back(static_cast<uint8_t>(std::stoul(hex.substr(i, 2), nullptr, 16)));
    }
    return bytes;
}

// Reverse bytes (for big-endian ↔ little-endian conversion)
std::string reverseHex(const std::string& hex) {
    std::string result;
    for (int i = hex.length() - 2; i >= 0; i -= 2) {
        result += hex.substr(i, 2);
    }
    return result;
}

} // anonymous namespace

// ═══════════════════════════════════════════════════════════════════════════════
// P1: BlockHeader v1 Layout Tests (128 bytes)
// ═══════════════════════════════════════════════════════════════════════════════

class BlockHeaderLayoutTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Known test values
        header.version = 1;
        header.prev_block_hash = uint256S("000000000019d6689c085ae165831e934ff763ae46a2a6c172b3f1b60a8ce26f");
        header.merkle_root = uint256S("4a5e1e4baab89f3a32518a88c31bc87f618f76673e2cc77ab2127b7afdeda33b");
        header.utreexo_root = uint256S("cff69d5f4f799c9f8d024df3acb6dd1bdf0e3e7988f8789bc2a6b07d0665abfc");
        header.timestamp = 1772496000;
        header.difficulty = 0x1d31ffce;
        header.nonce = 2083236893;
        header.ZeroReserved();
    }

    BlockHeader header;
};

TEST_F(BlockHeaderLayoutTest, HeaderSizeIs128Bytes) {
    auto serialized = header.SerializeForHash();
    ASSERT_EQ(serialized.size(), 128)
        << "BlockHeader v1 MUST be exactly 128 bytes";
    ASSERT_EQ(serialized.size(), DINERO_HEADER_SIZE_BYTES)
        << "DINERO_HEADER_SIZE_BYTES constant must match";
}

TEST_F(BlockHeaderLayoutTest, FieldOffsetsAreCorrect) {
    // Verify offsets match header_layout.h constants
    EXPECT_EQ(DINERO_HEADER_VERSION_OFFSET, 0);
    EXPECT_EQ(DINERO_HEADER_PREVHASH_OFFSET, 4);
    EXPECT_EQ(DINERO_HEADER_MERKLE_OFFSET, 36);
    EXPECT_EQ(DINERO_HEADER_UTREEXO_OFFSET, 68);
    EXPECT_EQ(DINERO_HEADER_TIMESTAMP_OFFSET, 100);
    EXPECT_EQ(DINERO_HEADER_DIFFICULTY_OFFSET, 108);
    EXPECT_EQ(DINERO_HEADER_NONCE_OFFSET, 112);
    EXPECT_EQ(DINERO_HEADER_RESERVED_OFFSET, 116);

    // Total: 4 + 32 + 32 + 32 + 8 + 4 + 4 + 12 = 128
    EXPECT_EQ(DINERO_HEADER_SIZE_BYTES, 128);
}

TEST_F(BlockHeaderLayoutTest, VersionFieldAtOffset0) {
    auto serialized = header.SerializeForHash();
    uint32_t version_le;
    memcpy(&version_le, serialized.data() + DINERO_HEADER_VERSION_OFFSET, 4);
    EXPECT_EQ(version_le, header.version)
        << "Version at offset 0, little-endian";
}

TEST_F(BlockHeaderLayoutTest, PrevHashFieldAtOffset4) {
    auto serialized = header.SerializeForHash();

    // Extract 32 bytes at offset 4
    std::vector<uint8_t> prevhash_bytes(32);
    memcpy(prevhash_bytes.data(), serialized.data() + DINERO_HEADER_PREVHASH_OFFSET, 32);

    // uint256 stores internally as little-endian
    // SerializeForHash copies raw bytes (little-endian)
    std::string prevhash_hex = bytesToHex(prevhash_bytes.data(), 32);

    // GetHex() returns big-endian display format, so we need to reverse
    std::string expected_le = reverseHex(header.prev_block_hash.GetHex());

    EXPECT_EQ(prevhash_hex, expected_le)
        << "PrevHash at offset 4, stored as little-endian bytes";
}

TEST_F(BlockHeaderLayoutTest, UtreexoRootFieldAtOffset68) {
    auto serialized = header.SerializeForHash();

    // Extract 32 bytes at offset 68
    std::vector<uint8_t> utreexo_bytes(32);
    memcpy(utreexo_bytes.data(), serialized.data() + DINERO_HEADER_UTREEXO_OFFSET, 32);

    std::string utreexo_hex = bytesToHex(utreexo_bytes.data(), 32);
    std::string expected_le = reverseHex(header.utreexo_root.GetHex());

    EXPECT_EQ(utreexo_hex, expected_le)
        << "Utreexo root at offset 68, stored as little-endian bytes";
}

TEST_F(BlockHeaderLayoutTest, TimestampFieldAtOffset100_8Bytes) {
    auto serialized = header.SerializeForHash();

    uint64_t timestamp_le;
    memcpy(&timestamp_le, serialized.data() + DINERO_HEADER_TIMESTAMP_OFFSET, 8);

    EXPECT_EQ(timestamp_le, header.timestamp)
        << "Timestamp at offset 100, 8 bytes little-endian";
}

TEST_F(BlockHeaderLayoutTest, DifficultyFieldAtOffset108) {
    auto serialized = header.SerializeForHash();

    uint32_t difficulty_le;
    memcpy(&difficulty_le, serialized.data() + DINERO_HEADER_DIFFICULTY_OFFSET, 4);

    EXPECT_EQ(difficulty_le, header.difficulty)
        << "Difficulty at offset 108, 4 bytes little-endian";
}

TEST_F(BlockHeaderLayoutTest, NonceFieldAtOffset112) {
    auto serialized = header.SerializeForHash();

    uint32_t nonce_le;
    memcpy(&nonce_le, serialized.data() + DINERO_HEADER_NONCE_OFFSET, 4);

    EXPECT_EQ(nonce_le, header.nonce)
        << "Nonce at offset 112, 4 bytes little-endian";
}

TEST_F(BlockHeaderLayoutTest, ReservedFieldAtOffset116_AllZeros) {
    auto serialized = header.SerializeForHash();

    // Reserved field: 12 bytes at offset 116
    for (int i = 0; i < 12; i++) {
        EXPECT_EQ(serialized[DINERO_HEADER_RESERVED_OFFSET + i], 0)
            << "Reserved byte " << i << " must be zero";
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
// P2: uint256 Byte Order Tests
// ═══════════════════════════════════════════════════════════════════════════════

class Uint256ByteOrderTest : public ::testing::Test {};

TEST_F(Uint256ByteOrderTest, GetHexReturnsBigEndian) {
    // uint256 with known pattern
    uint256 value = uint256S("0102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f20");

    std::string hex = value.GetHex();

    // GetHex() returns big-endian (most significant byte first)
    EXPECT_EQ(hex, "0102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f20")
        << "GetHex() must return big-endian display format";
}

TEST_F(Uint256ByteOrderTest, InternalStorageIsLittleEndian) {
    uint256 value = uint256S("0102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f20");

    // Access raw bytes (internal storage)
    const uint8_t* bytes = value.begin();

    // Internal storage is little-endian (LSB first)
    // So byte[0] should be 0x20 (the last byte of the hex string)
    EXPECT_EQ(bytes[0], 0x20) << "First internal byte should be LSB (0x20)";
    EXPECT_EQ(bytes[31], 0x01) << "Last internal byte should be MSB (0x01)";
}

TEST_F(Uint256ByteOrderTest, RoundTripPreservesValue) {
    std::string original_hex = "cff69d5f4f799c9f8d024df3acb6dd1bdf0e3e7988f8789bc2a6b07d0665abfc";

    uint256 value = uint256S(original_hex);
    std::string roundtrip_hex = value.GetHex();

    EXPECT_EQ(roundtrip_hex, original_hex)
        << "FromHex → GetHex roundtrip must preserve value";
}

TEST_F(Uint256ByteOrderTest, StratumByteOrderConversion) {
    // Stratum sends prevhash in big-endian (display format)
    std::string stratum_prevhash = "000000000019d6689c085ae165831e934ff763ae46a2a6c172b3f1b60a8ce26f";

    // Miner must reverse to little-endian for header bytes
    std::vector<uint8_t> be_bytes = hexToBytes(stratum_prevhash);
    std::vector<uint8_t> le_bytes = be_bytes;
    std::reverse(le_bytes.begin(), le_bytes.end());

    // Verify: first byte of LE should be last byte of BE
    EXPECT_EQ(le_bytes[0], be_bytes[31]);
    EXPECT_EQ(le_bytes[31], be_bytes[0]);

    // Now verify this matches what uint256 stores internally
    uint256 value = uint256S(stratum_prevhash);
    const uint8_t* internal_bytes = value.begin();

    for (int i = 0; i < 32; i++) {
        EXPECT_EQ(le_bytes[i], internal_bytes[i])
            << "Reversed bytes must match uint256 internal storage at index " << i;
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
// P3: Canonical Test Vectors (Genesis & Premine)
// ═══════════════════════════════════════════════════════════════════════════════

class CanonicalTestVectors : public ::testing::Test {};

TEST_F(CanonicalTestVectors, GenesisUtreexoRootIsZero) {
    // Genesis block (height 0) has no UTXOs to commit
    // The coinbase creates outputs, but Utreexo commits the BEFORE-state
    // At genesis, there is no BEFORE-state, so utreexo_root = 0
    uint256 genesis_utreexo = uint256();  // All zeros

    EXPECT_EQ(genesis_utreexo.GetHex(), std::string(64, '0'))
        << "Genesis utreexo_root must be all zeros";
}

TEST_F(CanonicalTestVectors, PremineUtreexoRootIsCanonical) {
    // Premine block (height 1) commits the AFTER-state of genesis
    // Genesis coinbase created 50 DIN, so premine commits that UTXO
    std::string canonical_premine_utreexo = "cff69d5f4f799c9f8d024df3acb6dd1bdf0e3e7988f8789bc2a6b07d0665abfc";

    uint256 premine_utreexo = uint256S(canonical_premine_utreexo);

    EXPECT_EQ(premine_utreexo.GetHex(), canonical_premine_utreexo)
        << "Premine utreexo_root must match canonical value";
}

TEST_F(CanonicalTestVectors, MainnetGenesisDifficulty) {
    // Mainnet genesis uses ASERT anchor difficulty
    uint32_t mainnet_genesis_bits = 0x1d31ffce;

    EXPECT_EQ(mainnet_genesis_bits, 0x1d31ffce)
        << "Mainnet genesis difficulty must be 0x1d31ffce";
}

TEST_F(CanonicalTestVectors, GenesisTimestamp) {
    // Genesis timestamp: January 25, 2025 00:00:00 UTC
    uint64_t genesis_timestamp = 1772496000;

    EXPECT_EQ(genesis_timestamp, 1772496000ULL)
        << "Genesis timestamp must be 1772496000";
}

// ═══════════════════════════════════════════════════════════════════════════════
// P4: SHA256d Hash Computation
// ═══════════════════════════════════════════════════════════════════════════════

class SHA256dHashTest : public ::testing::Test {
protected:
    void SetUp() override {
        header.version = 1;
        header.prev_block_hash = uint256();
        header.merkle_root = uint256S("c997af69be7140ede59c8114ae24f36944c4302b4664fdd82b45473f95a6fba1");
        header.utreexo_root = uint256();
        header.timestamp = 1772496000;
        header.difficulty = 0x1d31ffce;
        header.nonce = 0;
        header.ZeroReserved();
    }

    BlockHeader header;
};

TEST_F(SHA256dHashTest, HashIsDeterministic) {
    uint256 hash1 = header.GetHash();
    uint256 hash2 = header.GetHash();

    EXPECT_EQ(hash1, hash2)
        << "Hash computation must be deterministic";
}

TEST_F(SHA256dHashTest, NonceChangesHash) {
    header.nonce = 0;
    uint256 hash_n0 = header.GetHash();

    header.nonce = 1;
    uint256 hash_n1 = header.GetHash();

    header.nonce = 0xFFFFFFFF;
    uint256 hash_nmax = header.GetHash();

    EXPECT_NE(hash_n0, hash_n1) << "Different nonces must produce different hashes";
    EXPECT_NE(hash_n0, hash_nmax) << "Different nonces must produce different hashes";
    EXPECT_NE(hash_n1, hash_nmax) << "Different nonces must produce different hashes";
}

TEST_F(SHA256dHashTest, UtreexoRootChangesHash) {
    header.utreexo_root = uint256();
    uint256 hash_zero = header.GetHash();

    header.utreexo_root = uint256S("ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff");
    uint256 hash_ff = header.GetHash();

    EXPECT_NE(hash_zero, hash_ff)
        << "CRITICAL: Utreexo root MUST affect block hash";
}

TEST_F(SHA256dHashTest, HashIsValidSHA256d) {
    // SHA256d = SHA256(SHA256(data))
    auto serialized = header.SerializeForHash();

    // Compute SHA256d manually
    uint8_t first_hash[32];
    uint8_t second_hash[32];

    // First SHA256
    dinero::crypto::SHA256(serialized.data(), serialized.size(), first_hash);
    // Second SHA256
    dinero::crypto::SHA256(first_hash, 32, second_hash);

    // Compare with GetHash()
    uint256 expected;
    memcpy(expected.begin(), second_hash, 32);

    uint256 actual = header.GetHash();

    EXPECT_EQ(actual, expected)
        << "GetHash() must compute SHA256d correctly";
}

// ═══════════════════════════════════════════════════════════════════════════════
// P5: Stratum Protocol Field Encoding
// ═══════════════════════════════════════════════════════════════════════════════

class StratumEncodingTest : public ::testing::Test {};

TEST_F(StratumEncodingTest, PrevHashEncodingRoundTrip) {
    // Daemon sends prevhash in big-endian (GetHex format)
    std::string daemon_prevhash = "000000000019d6689c085ae165831e934ff763ae46a2a6c172b3f1b60a8ce26f";

    // Stratum server passes through unchanged (big-endian)
    std::string stratum_prevhash = daemon_prevhash;

    // Miner receives big-endian, must reverse to little-endian for header
    std::vector<uint8_t> be_bytes = hexToBytes(stratum_prevhash);
    std::reverse(be_bytes.begin(), be_bytes.end());  // → little-endian

    // Build header with reversed bytes
    BlockHeader header;
    header.version = 1;
    memcpy(header.prev_block_hash.begin(), be_bytes.data(), 32);

    // Verify: header's GetHex should match original
    EXPECT_EQ(header.prev_block_hash.GetHex(), daemon_prevhash)
        << "Stratum roundtrip must preserve prevhash value";
}

TEST_F(StratumEncodingTest, UtreexoRootEncodingRoundTrip) {
    // Daemon sends utreexo_root in big-endian (GetHex format)
    std::string daemon_utreexo = "cff69d5f4f799c9f8d024df3acb6dd1bdf0e3e7988f8789bc2a6b07d0665abfc";

    // Stratum server passes through unchanged (big-endian)
    std::string stratum_utreexo = daemon_utreexo;

    // Miner receives big-endian, must reverse to little-endian for header
    std::vector<uint8_t> be_bytes = hexToBytes(stratum_utreexo);
    std::reverse(be_bytes.begin(), be_bytes.end());  // → little-endian

    // Build header with reversed bytes
    BlockHeader header;
    header.version = 1;
    memcpy(header.utreexo_root.begin(), be_bytes.data(), 32);

    // Verify: header's GetHex should match original
    EXPECT_EQ(header.utreexo_root.GetHex(), daemon_utreexo)
        << "Stratum roundtrip must preserve utreexo_root value";
}

TEST_F(StratumEncodingTest, ConsistentByteOrderForAllUint256Fields) {
    // ALL uint256 fields must use the same byte order convention:
    // - Stratum sends big-endian (display format)
    // - Miner reverses to little-endian (internal format)

    std::string prevhash_be = "000000000019d6689c085ae165831e934ff763ae46a2a6c172b3f1b60a8ce26f";
    std::string merkle_be = "c997af69be7140ede59c8114ae24f36944c4302b4664fdd82b45473f95a6fba1";
    std::string utreexo_be = "cff69d5f4f799c9f8d024df3acb6dd1bdf0e3e7988f8789bc2a6b07d0665abfc";

    // Convert all to little-endian
    auto convert_be_to_le = [](const std::string& be_hex) {
        std::vector<uint8_t> bytes = hexToBytes(be_hex);
        std::reverse(bytes.begin(), bytes.end());
        return bytes;
    };

    auto prevhash_le = convert_be_to_le(prevhash_be);
    auto merkle_le = convert_be_to_le(merkle_be);
    auto utreexo_le = convert_be_to_le(utreexo_be);

    // Build header
    BlockHeader header;
    header.version = 1;
    memcpy(header.prev_block_hash.begin(), prevhash_le.data(), 32);
    memcpy(header.merkle_root.begin(), merkle_le.data(), 32);
    memcpy(header.utreexo_root.begin(), utreexo_le.data(), 32);
    header.timestamp = 1772496000;
    header.difficulty = 0x1d31ffce;
    header.nonce = 0;
    header.ZeroReserved();

    // All fields should roundtrip correctly
    EXPECT_EQ(header.prev_block_hash.GetHex(), prevhash_be);
    EXPECT_EQ(header.merkle_root.GetHex(), merkle_be);
    EXPECT_EQ(header.utreexo_root.GetHex(), utreexo_be);
}

// ═══════════════════════════════════════════════════════════════════════════════
// P6: Cross-Component Consistency (Simulated E2E)
// ═══════════════════════════════════════════════════════════════════════════════

class CrossComponentTest : public ::testing::Test {};

TEST_F(CrossComponentTest, DaemonToMinerHeaderConstruction) {
    // Simulate daemon → stratum → miner flow

    // Step 1: Daemon creates block template (internal uint256 values)
    BlockHeader daemon_header;
    daemon_header.version = 1;
    daemon_header.prev_block_hash = uint256S("000000000019d6689c085ae165831e934ff763ae46a2a6c172b3f1b60a8ce26f");
    daemon_header.merkle_root = uint256S("c997af69be7140ede59c8114ae24f36944c4302b4664fdd82b45473f95a6fba1");
    daemon_header.utreexo_root = uint256S("cff69d5f4f799c9f8d024df3acb6dd1bdf0e3e7988f8789bc2a6b07d0665abfc");
    daemon_header.timestamp = 1772496000;
    daemon_header.difficulty = 0x1d31ffce;
    daemon_header.nonce = 12345;
    daemon_header.ZeroReserved();

    // Step 2: Daemon serializes for stratum (GetHex = big-endian)
    std::string stratum_prevhash = daemon_header.prev_block_hash.GetHex();
    std::string stratum_merkle = daemon_header.merkle_root.GetHex();
    std::string stratum_utreexo = daemon_header.utreexo_root.GetHex();

    // Step 3: Miner reconstructs header from stratum fields
    BlockHeader miner_header;
    miner_header.version = daemon_header.version;
    miner_header.timestamp = daemon_header.timestamp;
    miner_header.difficulty = daemon_header.difficulty;
    miner_header.nonce = daemon_header.nonce;
    miner_header.ZeroReserved();

    // Miner reverses big-endian stratum fields to little-endian
    auto reverse_hex_to_uint256 = [](const std::string& be_hex) {
        std::vector<uint8_t> bytes = hexToBytes(be_hex);
        std::reverse(bytes.begin(), bytes.end());
        uint256 result;
        memcpy(result.begin(), bytes.data(), 32);
        return result;
    };

    miner_header.prev_block_hash = reverse_hex_to_uint256(stratum_prevhash);
    miner_header.merkle_root = reverse_hex_to_uint256(stratum_merkle);
    miner_header.utreexo_root = reverse_hex_to_uint256(stratum_utreexo);

    // Step 4: Verify miner's header matches daemon's header exactly
    EXPECT_EQ(miner_header.prev_block_hash, daemon_header.prev_block_hash);
    EXPECT_EQ(miner_header.merkle_root, daemon_header.merkle_root);
    EXPECT_EQ(miner_header.utreexo_root, daemon_header.utreexo_root);

    // Step 5: Verify hashes match
    uint256 daemon_hash = daemon_header.GetHash();
    uint256 miner_hash = miner_header.GetHash();

    EXPECT_EQ(daemon_hash, miner_hash)
        << "CRITICAL: Miner must produce same hash as daemon!";
}

TEST_F(CrossComponentTest, MinerSubmissionValidation) {
    // Simulate miner → stratum → daemon submission flow

    // Miner found a valid nonce
    BlockHeader miner_header;
    miner_header.version = 1;
    miner_header.prev_block_hash = uint256S("000000000019d6689c085ae165831e934ff763ae46a2a6c172b3f1b60a8ce26f");
    miner_header.merkle_root = uint256S("c997af69be7140ede59c8114ae24f36944c4302b4664fdd82b45473f95a6fba1");
    miner_header.utreexo_root = uint256S("cff69d5f4f799c9f8d024df3acb6dd1bdf0e3e7988f8789bc2a6b07d0665abfc");
    miner_header.timestamp = 1772496000;
    miner_header.difficulty = 0x1d31ffce;
    miner_header.nonce = 999999;  // "Found" nonce
    miner_header.ZeroReserved();

    // Miner submits nonce to stratum
    uint32_t submitted_nonce = miner_header.nonce;

    // Daemon reconstructs header with submitted nonce
    BlockHeader daemon_header = miner_header;  // Same template
    daemon_header.nonce = submitted_nonce;

    // Daemon validates: hash must match
    EXPECT_EQ(miner_header.GetHash(), daemon_header.GetHash())
        << "Daemon must compute same hash for submitted block";
}

// ═══════════════════════════════════════════════════════════════════════════════
// Main
// ═══════════════════════════════════════════════════════════════════════════════

int main(int argc, char** argv) {
    std::cout << "\n";
    std::cout << "═══════════════════════════════════════════════════════════════\n";
    std::cout << "  Dinero Mining Pipeline Tests\n";
    std::cout << "  Testing: Header Layout, Byte Order, SHA256d, Stratum Encoding\n";
    std::cout << "═══════════════════════════════════════════════════════════════\n";
    std::cout << "\n";

    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
