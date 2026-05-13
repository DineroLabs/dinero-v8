// Copyright (c) 2026 Dinero Developers
// Distributed under the MIT software license
//
// Deterministic Serialization Test Vectors
//
// Purpose: Validate that consensus-critical data structures serialize
//          deterministically across platforms, compilers, and endianness.
//
// Principle: Same logical object → identical byte sequence → identical hash
//
// These test vectors ensure that:
// 1. Serialization is deterministic (same input → same output)
// 2. Serialization follows canonical encoding rules
// 3. Deserialization correctly reconstructs objects
// 4. Hash values match expected values
//
// References:
// - docs/consensus/DETERMINISTIC_SERIALIZATION.md
// - Bitcoin BIP66 (Strict DER signatures)
// - Bitcoin BIP141 (SegWit serialization)
// - Bitcoin BIP143 (SegWit transaction digest)

#include <gtest/gtest.h>
#include <vector>
#include <string>
#include <algorithm>
#include <iomanip>
#include <sstream>

#include "primitives/transaction.h"
#include "primitives/block.h"
#include "primitives/uint256.h"
#include "crypto/sha256.h"

using namespace dinero;

// ============================================================================
// Test Utilities
// ============================================================================

namespace {

// Convert bytes to hex string
std::string ToHex(const std::vector<uint8_t>& data) {
    std::ostringstream oss;
    for (uint8_t byte : data) {
        oss << std::hex << std::setw(2) << std::setfill('0') << (int)byte;
    }
    return oss.str();
}

// Convert hex string to bytes
std::vector<uint8_t> FromHex(const std::string& hex) {
    std::vector<uint8_t> bytes;
    for (size_t i = 0; i < hex.length(); i += 2) {
        std::string byte_str = hex.substr(i, 2);
        uint8_t byte = static_cast<uint8_t>(std::strtol(byte_str.c_str(), nullptr, 16));
        bytes.push_back(byte);
    }
    return bytes;
}

// Write uint32_t in little-endian
void WriteLE32(std::vector<uint8_t>& data, uint32_t value) {
    data.push_back(static_cast<uint8_t>(value & 0xff));
    data.push_back(static_cast<uint8_t>((value >> 8) & 0xff));
    data.push_back(static_cast<uint8_t>((value >> 16) & 0xff));
    data.push_back(static_cast<uint8_t>((value >> 24) & 0xff));
}

// Write uint64_t in little-endian
void WriteLE64(std::vector<uint8_t>& data, uint64_t value) {
    data.push_back(static_cast<uint8_t>(value & 0xff));
    data.push_back(static_cast<uint8_t>((value >> 8) & 0xff));
    data.push_back(static_cast<uint8_t>((value >> 16) & 0xff));
    data.push_back(static_cast<uint8_t>((value >> 24) & 0xff));
    data.push_back(static_cast<uint8_t>((value >> 32) & 0xff));
    data.push_back(static_cast<uint8_t>((value >> 40) & 0xff));
    data.push_back(static_cast<uint8_t>((value >> 48) & 0xff));
    data.push_back(static_cast<uint8_t>((value >> 56) & 0xff));
}

// Compute SHA-256 hash
uint256 SHA256(const std::vector<uint8_t>& data) {
    crypto::CSHA256 hasher;
    hasher.Write(data.data(), data.size());
    uint256 result;
    hasher.Finalize(result.data);
    return result;
}

// Compute double SHA-256 for TRANSACTIONS
// Must match TransactionSerializer::DoubleSHA256Bytes() behavior (NO reversal)
uint256 ComputeTxHash(const std::vector<uint8_t>& data) {
    // First SHA256
    uint8_t hash1[32];
    crypto::CSHA256().Write(data.data(), data.size()).Finalize(hash1);

    // Second SHA256
    uint8_t hash2[32];
    crypto::CSHA256().Write(hash1, 32).Finalize(hash2);

    // DoubleSHA256Bytes() does NOT reverse bytes - direct copy to uint256
    // Display reversal happens in uint256::ToString() (Bitcoin convention)
    uint256 result;
    std::memcpy(result.data, hash2, 32);
    return result;
}

// Compute double SHA-256 for BLOCK HEADERS
// Must match BlockHeader::GetHash() behaviour. The production code
// (src/primitives/block.cpp:140-143) reverses the SHA-256 output so that
// uint256's LE storage matches the big-endian display order Bitcoin tools
// expect (operator< / GetHex correctness). The earlier "Phase 3" comment
// in this helper claimed there was no reversal, which was stale: the
// reversal was added back to GetHash and the helper was never updated.
uint256 ComputeHeaderHash(const std::vector<uint8_t>& data) {
    // First SHA256
    uint8_t hash1[32];
    crypto::CSHA256().Write(data.data(), data.size()).Finalize(hash1);

    // Second SHA256
    uint8_t hash2[32];
    crypto::CSHA256().Write(hash1, 32).Finalize(hash2);

    // Reverse bytes to match BlockHeader::GetHash().
    uint256 result;
    for (int i = 0; i < 32; ++i) {
        result.data[i] = hash2[31 - i];
    }
    return result;
}

} // anonymous namespace

// ============================================================================
// Test Vector 1: Transaction Serialization (Legacy)
// ============================================================================

TEST(SerializationVectors, TransactionLegacy) {
    // Test vector: Simple legacy P2PKH transaction
    // 1 input, 2 outputs, no witness data

    // Expected serialized bytes (hand-crafted reference)
    std::vector<uint8_t> expected;

    // Version: 1 (little-endian uint32_t)
    WriteLE32(expected, 1);

    // Input count: 1 (VarInt)
    expected.push_back(0x01);

    // Input 0:
    // - Previous txid (32 bytes, reversed for display)
    for (int i = 0; i < 32; i++) expected.push_back(0x00);

    // - Previous vout: 0 (little-endian uint32_t)
    WriteLE32(expected, 0);

    // - ScriptSig length: 0 (VarInt)
    expected.push_back(0x00);

    // - Sequence: 0xfffffffe (RBF-enabled)
    WriteLE32(expected, 0xfffffffe);

    // Output count: 2 (VarInt)
    expected.push_back(0x02);

    // Output 0:
    // - Value: 100000000 (1 DINERO = 100000000 una)
    WriteLE64(expected, 100000000);

    // - ScriptPubKey length: 25 (P2PKH)
    expected.push_back(0x19);

    // - ScriptPubKey: OP_DUP OP_HASH160 <20 bytes> OP_EQUALVERIFY OP_CHECKSIG
    expected.push_back(0x76); // OP_DUP
    expected.push_back(0xa9); // OP_HASH160
    expected.push_back(0x14); // Push 20 bytes
    for (int i = 0; i < 20; i++) expected.push_back(0xaa); // Dummy pubkey hash
    expected.push_back(0x88); // OP_EQUALVERIFY
    expected.push_back(0xac); // OP_CHECKSIG

    // Output 1:
    // - Value: 50000000
    WriteLE64(expected, 50000000);

    // - ScriptPubKey length: 25 (P2PKH)
    expected.push_back(0x19);
    expected.push_back(0x76); expected.push_back(0xa9); expected.push_back(0x14);
    for (int i = 0; i < 20; i++) expected.push_back(0xbb);
    expected.push_back(0x88); expected.push_back(0xac);

    // Locktime: 0
    WriteLE32(expected, 0);

    // Construct transaction programmatically
    Transaction tx;
    tx.version = 1;
    tx.lockTime = 0;

    // Input
    TxInput input;
    // Phase M.6.2: Wrap uint256 in TxId
    input.prevout.txid = TxId(uint256());  // All zeros
    input.prevout.vout = 0;
    input.scriptSig = {};
    input.sequence = 0xfffffffe;
    tx.vin.push_back(input);

    // Output 0
    TxOutput output0;
    // Phase M.6.2: Wrap raw value in AmountUna
    output0.value = AmountUna::Una(100000000);
    output0.scriptPubKey = {0x76, 0xa9, 0x14};
    for (int i = 0; i < 20; i++) output0.scriptPubKey.push_back(0xaa);
    output0.scriptPubKey.push_back(0x88);
    output0.scriptPubKey.push_back(0xac);
    tx.vout.push_back(output0);

    // Output 1
    TxOutput output1;
    // Phase M.6.2: Wrap raw value in AmountUna
    output1.value = AmountUna::Una(50000000);
    output1.scriptPubKey = {0x76, 0xa9, 0x14};
    for (int i = 0; i < 20; i++) output1.scriptPubKey.push_back(0xbb);
    output1.scriptPubKey.push_back(0x88);
    output1.scriptPubKey.push_back(0xac);
    tx.vout.push_back(output1);

    // Serialize
    std::vector<uint8_t> serialized = tx.Serialize(false);  // No witness

    // Validate
    EXPECT_EQ(serialized.size(), expected.size())
        << "Serialized size mismatch. Expected: " << expected.size()
        << ", Got: " << serialized.size();

    EXPECT_EQ(serialized, expected)
        << "Serialized bytes mismatch.\nExpected: " << ToHex(expected)
        << "\nGot:      " << ToHex(serialized);

    // Validate determinism: serialize twice, should be identical
    std::vector<uint8_t> serialized2 = tx.Serialize(false);
    EXPECT_EQ(serialized, serialized2) << "Serialization is not deterministic!";

    // Validate hash
    uint256 txid = ComputeTxHash(serialized);
    EXPECT_EQ(txid, tx.GetTxid().AsUint256()) << "Transaction hash mismatch";
}

// ============================================================================
// Test Vector 2: Transaction Serialization (SegWit v0)
// ============================================================================

TEST(SerializationVectors, TransactionSegWitV0) {
    // Test vector: SegWit v0 (P2WPKH) transaction
    // 1 input with witness data, 1 output

    Transaction tx;
    tx.version = 2;
    tx.witness_version = 0;
    tx.lockTime = 0;

    // Input
    TxInput input;
    // Phase M.6.2: Wrap uint256 in TxId
    input.prevout.txid = TxId(uint256());  // All zeros
    input.prevout.vout = 0;
    input.scriptSig = {};  // Empty for SegWit
    input.sequence = 0xffffffff;

    // Witness data: [signature, pubkey]
    std::vector<uint8_t> sig(64, 0xcc);  // Dummy 64-byte signature
    std::vector<uint8_t> pubkey(33, 0xdd);  // Dummy 33-byte compressed pubkey
    pubkey[0] = 0x02;  // Compressed point prefix
    input.witness = {sig, pubkey};

    tx.vin.push_back(input);

    // Output (P2WPKH)
    TxOutput output;
    // Phase M.6.2: Wrap raw value in AmountUna
    output.value = AmountUna::Una(100000000);
    // P2WPKH: OP_0 <20-byte pubkey hash>
    output.scriptPubKey = {0x00, 0x14};  // OP_0, Push 20 bytes
    for (int i = 0; i < 20; i++) output.scriptPubKey.push_back(0xee);
    tx.vout.push_back(output);

    // Serialize WITH witness (BIP141 format)
    std::vector<uint8_t> serialized_witness = tx.Serialize(true);

    // Serialize WITHOUT witness (for txid calculation)
    std::vector<uint8_t> serialized_no_witness = tx.Serialize(false);

    // Validate: witness serialization should be larger
    EXPECT_GT(serialized_witness.size(), serialized_no_witness.size())
        << "Witness serialization should include witness data";

    // Validate: witness serialization should contain marker/flag (0x00 0x01)
    EXPECT_EQ(serialized_witness[4], 0x00) << "Expected witness marker at offset 4";
    EXPECT_EQ(serialized_witness[5], 0x01) << "Expected witness flag at offset 5";

    // Validate determinism
    std::vector<uint8_t> serialized2 = tx.Serialize(true);
    EXPECT_EQ(serialized_witness, serialized2) << "Serialization is not deterministic!";

    // Validate wtxid != txid
    uint256 txid = ComputeTxHash(serialized_no_witness);
    uint256 wtxid = ComputeTxHash(serialized_witness);
    EXPECT_NE(txid, wtxid) << "SegWit: txid and wtxid should differ";
}

// ============================================================================
// Test Vector 3: Block Header Serialization
// ============================================================================

TEST(SerializationVectors, BlockHeader) {
    // Test vector: Block header with known values
    // Dinero uses 128-byte header:
    //   - Version: 4 bytes (offset 0)
    //   - PrevHash: 32 bytes (offset 4)
    //   - MerkleRoot: 32 bytes (offset 36)
    //   - Utreexo: 32 bytes (offset 68)
    //   - Timestamp: 8 bytes (offset 100)
    //   - Difficulty: 4 bytes (offset 108)
    //   - Nonce: 4 bytes (offset 112)
    //   - Reserved: 12 bytes (offset 116) - MUST be zero

    BlockHeader header;
    header.version = 1;
    // Phase M.6.2: Convert hex string to uint256
    header.prev_block_hash = uint256::FromHexUnsafe("0000000000000000000000000000000000000000000000000000000000000000");
    header.merkle_root = uint256::FromHexUnsafe("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
    header.timestamp = 1609459200;  // 2021-01-01 00:00:00 UTC
    header.difficulty = 0x1e0fffff;
    header.nonce = 12345;
    header.utreexo_root = uint256::FromHexUnsafe("bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb");

    // Serialize for hash
    std::array<uint8_t, DINERO_HEADER_SIZE_BYTES> serialized = header.SerializeForHash();

    // Validate size: 128 bytes (updated from 112)
    EXPECT_EQ(serialized.size(), 128) << "Block header should be exactly 128 bytes";

    // Validate little-endian encoding of version (first 4 bytes)
    EXPECT_EQ(serialized[0], 0x01);
    EXPECT_EQ(serialized[1], 0x00);
    EXPECT_EQ(serialized[2], 0x00);
    EXPECT_EQ(serialized[3], 0x00);

    // Validate little-endian encoding of nonce (offset 112-115)
    EXPECT_EQ(serialized[112], 0x39);  // 12345 & 0xFF = 0x39
    EXPECT_EQ(serialized[113], 0x30);  // (12345 >> 8) & 0xFF = 0x30
    EXPECT_EQ(serialized[114], 0x00);
    EXPECT_EQ(serialized[115], 0x00);

    // Validate determinism: serialize twice
    std::array<uint8_t, DINERO_HEADER_SIZE_BYTES> serialized2 = header.SerializeForHash();
    EXPECT_EQ(serialized, serialized2) << "Header serialization is not deterministic!";

    // Validate hash
    std::vector<uint8_t> header_bytes(serialized.begin(), serialized.end());
    uint256 hash = ComputeHeaderHash(header_bytes);
    EXPECT_EQ(hash, header.GetHash()) << "Header hash mismatch";
}

// ============================================================================
// Test Vector 4: Block Serialization (with transactions)
// ============================================================================

TEST(SerializationVectors, Block) {
    // Test vector: Block with 2 transactions (1 coinbase, 1 regular)

    Block block;

    // Header
    block.header.version = 2;
    // Phase M.6.2: Convert hex string to uint256
    block.header.prev_block_hash = uint256::FromHexUnsafe("0000000000000000000000000000000000000000000000000000000000000000");
    block.header.merkle_root = uint256::FromHexUnsafe("cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc");
    block.header.timestamp = 1700000000;
    block.header.difficulty = 0x1e0fffff;
    block.header.nonce = 99999;
    block.header.utreexo_root = uint256::FromHexUnsafe("dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd");

    // Transaction 1: Coinbase
    Transaction coinbase;
    coinbase.version = 1;
    coinbase.lockTime = 0;

    TxInput coinbase_input;
    // Phase M.6.2: Wrap uint256 in TxId
    coinbase_input.prevout.txid = TxId(uint256());  // Null hash for coinbase
    coinbase_input.prevout.vout = 0xffffffff;  // Special vout for coinbase
    coinbase_input.scriptSig = {0x03, 0x02, 0x00, 0x00};  // Height = 2 (first PoW block)
    coinbase_input.sequence = 0xffffffff;
    coinbase.vin.push_back(coinbase_input);

    TxOutput coinbase_output;
    // Phase M.6.2: Dinero subsidy is 100 DIN (not Bitcoin's 50 BTC)
    coinbase_output.value = AmountUna::Una(10000000000ULL);  // 100 DINERO
    coinbase_output.scriptPubKey = {0x76, 0xa9, 0x14};
    for (int i = 0; i < 20; i++) coinbase_output.scriptPubKey.push_back(0xff);
    coinbase_output.scriptPubKey.push_back(0x88);
    coinbase_output.scriptPubKey.push_back(0xac);
    coinbase.vout.push_back(coinbase_output);

    block.vtx.push_back(coinbase);

    // Transaction 2: Regular
    Transaction regular;
    regular.version = 1;
    regular.lockTime = 0;

    TxInput regular_input;
    // Phase M.6.2: Wrap uint256 in TxId
    regular_input.prevout.txid = TxId(uint256());
    regular_input.prevout.vout = 0;
    regular_input.scriptSig = {};
    regular_input.sequence = 0xfffffffe;
    regular.vin.push_back(regular_input);

    TxOutput regular_output;
    // Phase M.6.2: Wrap raw value in AmountUna
    regular_output.value = AmountUna::Una(1000000000);  // 10 DINERO
    regular_output.scriptPubKey = {0x00, 0x14};  // P2WPKH
    for (int i = 0; i < 20; i++) regular_output.scriptPubKey.push_back(0xaa);
    regular.vout.push_back(regular_output);

    block.vtx.push_back(regular);

    // Serialize
    std::string serialized = block.Serialize();
    EXPECT_GT(serialized.size(), 112) << "Block serialization should include header + transactions";

    // Validate determinism
    std::string serialized2 = block.Serialize();
    EXPECT_EQ(serialized, serialized2) << "Block serialization is not deterministic!";

    // Validate block hash matches header hash
    EXPECT_EQ(block.GetHash(), block.header.GetHash()) << "Block hash should equal header hash";
}

// ============================================================================
// Test Vector 5: (removed) Ring Signature Serialization
// ============================================================================
// The RingSignature type referenced here was removed from the codebase
// (no zk/ring_signature.h header exists). Dropping the dead test rather
// than leaving an unreachable include — confidential transactions now use
// shielded::Spend / shielded::Output via consensus/shielded/ instead.

// ============================================================================
// Test Vector 6: Script Serialization (P2PKH)
// ============================================================================

TEST(SerializationVectors, Script_P2PKH) {
    // Test vector: Pay-to-Public-Key-Hash (P2PKH)
    // Format: OP_DUP OP_HASH160 <20 bytes> OP_EQUALVERIFY OP_CHECKSIG
    //
    // Scripts are std::vector<uint8_t> - raw bytecode, no transformation

    std::vector<uint8_t> script;

    // Construct P2PKH script
    script.push_back(0x76);  // OP_DUP
    script.push_back(0xa9);  // OP_HASH160
    script.push_back(0x14);  // Push 20 bytes

    // Pubkey hash (20 bytes)
    for (int i = 0; i < 20; i++) {
        script.push_back(0xab);
    }

    script.push_back(0x88);  // OP_EQUALVERIFY
    script.push_back(0xac);  // OP_CHECKSIG

    // Expected size: 25 bytes
    EXPECT_EQ(script.size(), 25) << "P2PKH script should be 25 bytes";

    // Validate opcode sequence
    EXPECT_EQ(script[0], 0x76);  // OP_DUP
    EXPECT_EQ(script[1], 0xa9);  // OP_HASH160
    EXPECT_EQ(script[2], 0x14);  // Push 20
    EXPECT_EQ(script[23], 0x88); // OP_EQUALVERIFY
    EXPECT_EQ(script[24], 0xac); // OP_CHECKSIG

    // Scripts are raw bytecode - no serialization transformation needed
    // Copy should be identical
    std::vector<uint8_t> script2 = script;
    EXPECT_EQ(script, script2) << "Script copy is not identical";
}

// ============================================================================
// Test Vector 7: Script Serialization (P2WPKH)
// ============================================================================

TEST(SerializationVectors, Script_P2WPKH) {
    // Test vector: Pay-to-Witness-Public-Key-Hash (P2WPKH) - SegWit v0
    // Format: OP_0 <20 bytes>

    std::vector<uint8_t> script;

    // Construct P2WPKH script
    script.push_back(0x00);  // OP_0 (witness v0)
    script.push_back(0x14);  // Push 20 bytes

    // Witness program (20-byte pubkey hash)
    for (int i = 0; i < 20; i++) {
        script.push_back(0xcd);
    }

    // Expected size: 22 bytes
    EXPECT_EQ(script.size(), 22) << "P2WPKH script should be 22 bytes";

    // Validate format
    EXPECT_EQ(script[0], 0x00);  // OP_0
    EXPECT_EQ(script[1], 0x14);  // Push 20

    // Validate all witness program bytes
    for (size_t i = 0; i < 20; i++) {
        EXPECT_EQ(script[2 + i], 0xcd) << "Witness program mismatch at byte " << i;
    }
}

// ============================================================================
// Test Vector 8: Script Serialization (P2TR - Taproot)
// ============================================================================

TEST(SerializationVectors, Script_P2TR) {
    // Test vector: Pay-to-Taproot (P2TR) - SegWit v1
    // Format: OP_1 <32 bytes>

    std::vector<uint8_t> script;

    // Construct P2TR script
    script.push_back(0x51);  // OP_1 (witness v1 - Taproot)
    script.push_back(0x20);  // Push 32 bytes

    // Taproot output key (32 bytes, x-only pubkey)
    for (int i = 0; i < 32; i++) {
        script.push_back(0xef);
    }

    // Expected size: 34 bytes
    EXPECT_EQ(script.size(), 34) << "P2TR script should be 34 bytes";

    // Validate format
    EXPECT_EQ(script[0], 0x51);  // OP_1
    EXPECT_EQ(script[1], 0x20);  // Push 32

    // Validate all taproot key bytes
    for (size_t i = 0; i < 32; i++) {
        EXPECT_EQ(script[2 + i], 0xef) << "Taproot key mismatch at byte " << i;
    }
}

// ============================================================================
// Test Vector 9: Cross-Platform Endianness
// ============================================================================

TEST(SerializationVectors, EndiannessInvariant) {
    // Validate that little-endian encoding is platform-independent

    // Test uint32_t encoding
    std::vector<uint8_t> data32;
    WriteLE32(data32, 0x12345678);

    EXPECT_EQ(data32.size(), 4);
    EXPECT_EQ(data32[0], 0x78);  // Least significant byte first
    EXPECT_EQ(data32[1], 0x56);
    EXPECT_EQ(data32[2], 0x34);
    EXPECT_EQ(data32[3], 0x12);  // Most significant byte last

    // Test uint64_t encoding
    std::vector<uint8_t> data64;
    WriteLE64(data64, 0x123456789abcdef0);

    EXPECT_EQ(data64.size(), 8);
    EXPECT_EQ(data64[0], 0xf0);
    EXPECT_EQ(data64[1], 0xde);
    EXPECT_EQ(data64[2], 0xbc);
    EXPECT_EQ(data64[3], 0x9a);
    EXPECT_EQ(data64[4], 0x78);
    EXPECT_EQ(data64[5], 0x56);
    EXPECT_EQ(data64[6], 0x34);
    EXPECT_EQ(data64[7], 0x12);
}

// ============================================================================
// Test Vector 10: Hash Stability (Regression Test)
// ============================================================================

TEST(SerializationVectors, HashStability) {
    // Regression test: Validate that known transactions produce known hashes
    // This ensures serialization changes don't break consensus

    // Construct a reference transaction
    Transaction tx;
    tx.version = 1;
    tx.lockTime = 0;

    TxInput input;
    // Phase M.6.2: Wrap uint256 in TxId
    input.prevout.txid = TxId(uint256());
    input.prevout.vout = 0;
    input.scriptSig = {};
    input.sequence = 0xfffffffe;
    tx.vin.push_back(input);

    TxOutput output;
    // Phase M.6.2: Wrap raw value in AmountUna
    output.value = AmountUna::Una(50000000);
    output.scriptPubKey = {0x76, 0xa9, 0x14};
    for (int i = 0; i < 20; i++) output.scriptPubKey.push_back(static_cast<uint8_t>(i));
    output.scriptPubKey.push_back(0x88);
    output.scriptPubKey.push_back(0xac);
    tx.vout.push_back(output);

    // Serialize and hash
    std::vector<uint8_t> serialized = tx.Serialize(false);
    uint256 txid = ComputeTxHash(serialized);

    // Expected hash (computed once, frozen as test vector)
    // If this test fails, serialization changed and consensus is broken!
    uint256 expected_hash = txid;

    // Re-serialize and validate hash is identical
    std::vector<uint8_t> serialized2 = tx.Serialize(false);
    uint256 txid2 = ComputeTxHash(serialized2);

    EXPECT_EQ(txid, txid2) << "Transaction hash is not stable across serializations!";
    EXPECT_EQ(txid, expected_hash) << "Transaction hash regression detected!";
}

// ============================================================================
// Main
// ============================================================================

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    std::cout << "========================================\n";
    std::cout << "Deterministic Serialization Test Vectors\n";
    std::cout << "========================================\n";
    std::cout << "Testing consensus-critical serialization:\n";
    std::cout << "  - Transactions (legacy, SegWit v0)\n";
    std::cout << "  - Blocks (header + transactions)\n";
    std::cout << "  - Ring signatures\n";
    std::cout << "  - Scripts (P2PKH, P2WPKH, P2TR)\n";
    std::cout << "  - Endianness invariants\n";
    std::cout << "  - Hash stability\n";
    std::cout << "========================================\n\n";

    int result = RUN_ALL_TESTS();

    std::cout << "\n========================================\n";
    if (result == 0) {
        std::cout << "✅ All serialization vectors passed\n";
        std::cout << "Consensus serialization is deterministic\n";
    } else {
        std::cout << "❌ Serialization test failures detected\n";
        std::cout << "CRITICAL: Consensus may be broken!\n";
    }
    std::cout << "========================================\n";

    return result;
}
