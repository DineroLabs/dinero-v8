// Phase 22.0 B6: Integration tests for block deserialization
// Tests SegWit, legacy, and Taproot block parsing

#include "primitives/block.h"
#include "common/serialization.h"
#include <iostream>
#include <cassert>
#include <vector>
#include <cstring>

using namespace dinero;

// Helper to create test block data
std::vector<uint8_t> createTestBlockHeader(uint32_t version, uint32_t nonce) {
    std::vector<uint8_t> data;

    // Version (4 bytes, little-endian)
    data.push_back(version & 0xFF);
    data.push_back((version >> 8) & 0xFF);
    data.push_back((version >> 16) & 0xFF);
    data.push_back((version >> 24) & 0xFF);

    // Previous block hash (32 bytes of zeros)
    for (int i = 0; i < 32; i++) {
        data.push_back(0);
    }

    // Merkle root (32 bytes of zeros)
    for (int i = 0; i < 32; i++) {
        data.push_back(0);
    }

    // Timestamp (4 bytes)
    uint32_t timestamp = 1609459200; // 2021-01-01
    data.push_back(timestamp & 0xFF);
    data.push_back((timestamp >> 8) & 0xFF);
    data.push_back((timestamp >> 16) & 0xFF);
    data.push_back((timestamp >> 24) & 0xFF);

    // Bits (4 bytes)
    uint32_t bits = 0x1e0fffff;
    data.push_back(bits & 0xFF);
    data.push_back((bits >> 8) & 0xFF);
    data.push_back((bits >> 16) & 0xFF);
    data.push_back((bits >> 24) & 0xFF);

    // Nonce (4 bytes)
    data.push_back(nonce & 0xFF);
    data.push_back((nonce >> 8) & 0xFF);
    data.push_back((nonce >> 16) & 0xFF);
    data.push_back((nonce >> 24) & 0xFF);

    return data;
}

// Test 1: Legacy (non-SegWit) coinbase transaction
bool test_legacy_coinbase_block() {
    std::cout << "Test 1: Legacy coinbase block... ";

    std::vector<uint8_t> block_data = createTestBlockHeader(1, 12345);

    // Transaction count: 1
    block_data.push_back(0x01);

    // Legacy coinbase transaction
    // Version
    block_data.push_back(0x01); block_data.push_back(0x00);
    block_data.push_back(0x00); block_data.push_back(0x00);

    // Input count: 1
    block_data.push_back(0x01);

    // Coinbase input: null hash (32 bytes of 0x00)
    for (int i = 0; i < 32; i++) block_data.push_back(0x00);

    // Coinbase input: vout 0xFFFFFFFF
    block_data.push_back(0xFF); block_data.push_back(0xFF);
    block_data.push_back(0xFF); block_data.push_back(0xFF);

    // ScriptSig: length 4, data [0x03, height, extra, extra]
    block_data.push_back(0x04);
    block_data.push_back(0x03); block_data.push_back(0x01);
    block_data.push_back(0x00); block_data.push_back(0x00);

    // Sequence
    block_data.push_back(0xFF); block_data.push_back(0xFF);
    block_data.push_back(0xFF); block_data.push_back(0xFF);

    // Output count: 1
    block_data.push_back(0x01);

    // Output value: 10000000000 una (100 DIN)
    uint64_t value = 10000000000ULL;
    for (int i = 0; i < 8; i++) {
        block_data.push_back((value >> (i * 8)) & 0xFF);
    }

    // ScriptPubKey: length 25 (P2PKH)
    block_data.push_back(0x19);
    block_data.push_back(0x76); // OP_DUP
    block_data.push_back(0xA9); // OP_HASH160
    block_data.push_back(0x14); // Push 20 bytes
    for (int i = 0; i < 20; i++) block_data.push_back(0xAA); // Dummy pubkey hash
    block_data.push_back(0x88); // OP_EQUALVERIFY
    block_data.push_back(0xAC); // OP_CHECKSIG

    // Locktime
    block_data.push_back(0x00); block_data.push_back(0x00);
    block_data.push_back(0x00); block_data.push_back(0x00);

    // Deserialize
    try {
        Block block;
        if (!DeserializeBlock(block_data, block)) {
            std::cout << "FAILED (deserialization failed)\n";
            return false;
        }

        // Verify
        if (block.vtx.size() != 1) {
            std::cout << "FAILED (expected 1 tx, got " << block.vtx.size() << ")\n";
            return false;
        }

        if (block.vtx[0].vin.size() != 1 || block.vtx[0].vout.size() != 1) {
            std::cout << "FAILED (wrong input/output count)\n";
            return false;
        }

        // Verify no witness data in legacy transaction
        if (!block.vtx[0].vin[0].witness.empty()) {
            std::cout << "FAILED (unexpected witness data in legacy tx)\n";
            return false;
        }

        std::cout << "PASSED\n";
        return true;
    } catch (const std::exception& e) {
        std::cout << "FAILED (exception: " << e.what() << ")\n";
        return false;
    }
}

// Test 2: SegWit v0 coinbase transaction
bool test_segwit_coinbase_block() {
    std::cout << "Test 2: SegWit coinbase block... ";

    std::vector<uint8_t> block_data = createTestBlockHeader(2, 67890);

    // Transaction count: 1
    block_data.push_back(0x01);

    // SegWit coinbase transaction
    // Version
    block_data.push_back(0x01); block_data.push_back(0x00);
    block_data.push_back(0x00); block_data.push_back(0x00);

    // SegWit marker and flag
    block_data.push_back(0x00); // Marker
    block_data.push_back(0x01); // Flag (v0)

    // Input count: 1
    block_data.push_back(0x01);

    // Coinbase input: null hash
    for (int i = 0; i < 32; i++) block_data.push_back(0x00);

    // Coinbase input: vout 0xFFFFFFFF
    block_data.push_back(0xFF); block_data.push_back(0xFF);
    block_data.push_back(0xFF); block_data.push_back(0xFF);

    // ScriptSig: length 4
    block_data.push_back(0x04);
    block_data.push_back(0x03); block_data.push_back(0x02);
    block_data.push_back(0x00); block_data.push_back(0x00);

    // Sequence
    block_data.push_back(0xFF); block_data.push_back(0xFF);
    block_data.push_back(0xFF); block_data.push_back(0xFF);

    // Output count: 1
    block_data.push_back(0x01);

    // Output value: 10000000000 una (100 DIN)
    uint64_t value = 10000000000ULL;
    for (int i = 0; i < 8; i++) {
        block_data.push_back((value >> (i * 8)) & 0xFF);
    }

    // ScriptPubKey: length 22 (P2WPKH)
    block_data.push_back(0x16);
    block_data.push_back(0x00); // OP_0 (witness v0)
    block_data.push_back(0x14); // Push 20 bytes
    for (int i = 0; i < 20; i++) block_data.push_back(0xBB); // Dummy witness program

    // Witness data for coinbase (1 item: witness reserved value)
    block_data.push_back(0x01); // Witness stack count
    block_data.push_back(0x20); // 32 bytes
    for (int i = 0; i < 32; i++) block_data.push_back(0x00); // Witness reserved value

    // Locktime
    block_data.push_back(0x00); block_data.push_back(0x00);
    block_data.push_back(0x00); block_data.push_back(0x00);

    // Deserialize
    try {
        Block block;
        if (!DeserializeBlock(block_data, block)) {
            std::cout << "FAILED (deserialization failed)\n";
            return false;
        }

        // Verify
        if (block.vtx.size() != 1) {
            std::cout << "FAILED (expected 1 tx, got " << block.vtx.size() << ")\n";
            return false;
        }

        if (block.vtx[0].vin.size() != 1 || block.vtx[0].vout.size() != 1) {
            std::cout << "FAILED (wrong input/output count)\n";
            return false;
        }

        // Verify witness data exists
        if (block.vtx[0].vin[0].witness.empty()) {
            std::cout << "FAILED (missing witness data)\n";
            return false;
        }

        if (block.vtx[0].vin[0].witness.size() != 1) {
            std::cout << "FAILED (expected 1 witness item, got " << block.vtx[0].vin[0].witness.size() << ")\n";
            return false;
        }

        if (block.vtx[0].vin[0].witness[0].size() != 32) {
            std::cout << "FAILED (expected 32-byte witness reserved value)\n";
            return false;
        }

        std::cout << "PASSED\n";
        return true;
    } catch (const std::exception& e) {
        std::cout << "FAILED (exception: " << e.what() << ")\n";
        return false;
    }
}

// Test 3: Taproot (witness v1) handling - should reject gracefully
bool test_taproot_block_handling() {
    std::cout << "Test 3: Taproot witness v1 handling... ";

    std::vector<uint8_t> block_data = createTestBlockHeader(2, 99999);

    // Transaction count: 1
    block_data.push_back(0x01);

    // Taproot transaction (witness v1)
    // Version
    block_data.push_back(0x02); block_data.push_back(0x00);
    block_data.push_back(0x00); block_data.push_back(0x00);

    // SegWit marker and flag - NOTE: This is still 0x00 0x01 (same as SegWit v0)
    // Taproot uses witness v1 in the scriptPubKey, not in the transaction format
    block_data.push_back(0x00); // Marker
    block_data.push_back(0x01); // Flag

    // Input count: 1
    block_data.push_back(0x01);

    // Input: prev tx (32 bytes)
    for (int i = 0; i < 32; i++) block_data.push_back(0xCC);

    // Input: vout 0
    block_data.push_back(0x00); block_data.push_back(0x00);
    block_data.push_back(0x00); block_data.push_back(0x00);

    // ScriptSig: empty for P2TR
    block_data.push_back(0x00);

    // Sequence
    block_data.push_back(0xFD); block_data.push_back(0xFF);
    block_data.push_back(0xFF); block_data.push_back(0xFF);

    // Output count: 1
    block_data.push_back(0x01);

    // Output value: 1000000000 una
    uint64_t value = 1000000000ULL;
    for (int i = 0; i < 8; i++) {
        block_data.push_back((value >> (i * 8)) & 0xFF);
    }

    // ScriptPubKey: P2TR (witness v1)
    block_data.push_back(0x22); // 34 bytes
    block_data.push_back(0x51); // OP_1 (witness v1)
    block_data.push_back(0x20); // Push 32 bytes
    for (int i = 0; i < 32; i++) block_data.push_back(0xDD); // Taproot output key

    // Witness data (Taproot signature - 64 or 65 bytes)
    block_data.push_back(0x01); // 1 witness item
    block_data.push_back(0x40); // 64 bytes
    for (int i = 0; i < 64; i++) block_data.push_back(0xEE); // Schnorr signature

    // Locktime
    block_data.push_back(0x00); block_data.push_back(0x00);
    block_data.push_back(0x00); block_data.push_back(0x00);

    // Deserialize - should parse successfully
    // (Taproot is just SegWit v1, parsing should work)
    try {
        Block block;
        if (!DeserializeBlock(block_data, block)) {
            std::cout << "FAILED (deserialization failed)\n";
            return false;
        }

        // Verify basic structure
        if (block.vtx.size() != 1) {
            std::cout << "FAILED (expected 1 tx)\n";
            return false;
        }

        // Verify witness data parsed
        if (block.vtx[0].vin[0].witness.empty()) {
            std::cout << "FAILED (missing witness data)\n";
            return false;
        }

        if (block.vtx[0].vin[0].witness[0].size() != 64) {
            std::cout << "FAILED (expected 64-byte Schnorr signature)\n";
            return false;
        }

        std::cout << "PASSED (Taproot parsed as SegWit v1)\n";
        return true;
    } catch (const std::exception& e) {
        std::cout << "FAILED (exception: " << e.what() << ")\n";
        return false;
    }
}

// Test 4: Mixed block with SegWit and legacy transactions
bool test_mixed_block() {
    std::cout << "Test 4: Mixed SegWit + legacy block... ";

    std::vector<uint8_t> block_data = createTestBlockHeader(2, 11111);

    // Transaction count: 2
    block_data.push_back(0x02);

    // TX 1: Legacy transaction
    block_data.push_back(0x01); block_data.push_back(0x00);
    block_data.push_back(0x00); block_data.push_back(0x00);
    block_data.push_back(0x01); // 1 input
    for (int i = 0; i < 32; i++) block_data.push_back(0x01);
    block_data.push_back(0x00); block_data.push_back(0x00);
    block_data.push_back(0x00); block_data.push_back(0x00);
    block_data.push_back(0x00); // Empty scriptSig
    block_data.push_back(0xFF); block_data.push_back(0xFF);
    block_data.push_back(0xFF); block_data.push_back(0xFF);
    block_data.push_back(0x01); // 1 output
    uint64_t val1 = 1000000000ULL;
    for (int i = 0; i < 8; i++) block_data.push_back((val1 >> (i * 8)) & 0xFF);
    block_data.push_back(0x00); // Empty scriptPubKey
    block_data.push_back(0x00); block_data.push_back(0x00);
    block_data.push_back(0x00); block_data.push_back(0x00);

    // TX 2: SegWit transaction
    block_data.push_back(0x02); block_data.push_back(0x00);
    block_data.push_back(0x00); block_data.push_back(0x00);
    block_data.push_back(0x00); block_data.push_back(0x01); // Marker + flag
    block_data.push_back(0x01); // 1 input
    for (int i = 0; i < 32; i++) block_data.push_back(0x02);
    block_data.push_back(0x00); block_data.push_back(0x00);
    block_data.push_back(0x00); block_data.push_back(0x00);
    block_data.push_back(0x00); // Empty scriptSig
    block_data.push_back(0xFF); block_data.push_back(0xFF);
    block_data.push_back(0xFF); block_data.push_back(0xFF);
    block_data.push_back(0x01); // 1 output
    uint64_t val2 = 2000000000ULL;
    for (int i = 0; i < 8; i++) block_data.push_back((val2 >> (i * 8)) & 0xFF);
    block_data.push_back(0x00); // Empty scriptPubKey
    block_data.push_back(0x01); // 1 witness item
    block_data.push_back(0x02); // 2 bytes
    block_data.push_back(0xAA); block_data.push_back(0xBB);
    block_data.push_back(0x00); block_data.push_back(0x00);
    block_data.push_back(0x00); block_data.push_back(0x00);

    try {
        Block block;
        if (!DeserializeBlock(block_data, block)) {
            std::cout << "FAILED (deserialization failed)\n";
            return false;
        }

        if (block.vtx.size() != 2) {
            std::cout << "FAILED (expected 2 txs, got " << block.vtx.size() << ")\n";
            return false;
        }

        // TX 1 should have no witness
        if (!block.vtx[0].vin[0].witness.empty()) {
            std::cout << "FAILED (tx1 should have no witness)\n";
            return false;
        }

        // TX 2 should have witness
        if (block.vtx[1].vin[0].witness.empty()) {
            std::cout << "FAILED (tx2 should have witness)\n";
            return false;
        }

        std::cout << "PASSED\n";
        return true;
    } catch (const std::exception& e) {
        std::cout << "FAILED (exception: " << e.what() << ")\n";
        return false;
    }
}

int main() {
    std::cout << "========================================\n";
    std::cout << "Phase 22.0 B6: Block Deserialization Tests\n";
    std::cout << "========================================\n\n";

    int passed = 0;
    int total = 4;

    if (test_legacy_coinbase_block()) passed++;
    if (test_segwit_coinbase_block()) passed++;
    if (test_taproot_block_handling()) passed++;
    if (test_mixed_block()) passed++;

    std::cout << "\n========================================\n";
    std::cout << "Results: " << passed << "/" << total << " tests passed\n";
    std::cout << "========================================\n";

    return (passed == total) ? 0 : 1;
}
