/**
 * @file test_block_serialization.cpp
 * @brief Block serialization round-trip tests (RocksDB sanity guard)
 *
 * Ensures:
 * 1. Block::Serialize() → parse → Serialize() is identity
 * 2. Header and tx count survive round-trip
 * 3. Transactions are parsed with canonical deserializer
 */

#include "primitives/block.h"
#include "primitives/transaction.h"
#include "primitives/amount.h"
#include "primitives/hash_domains.h"
#include <cassert>
#include <iostream>
#include <vector>

using namespace dinero;

// ═══════════════════════════════════════════════════════════════════════════
// Test Utilities
// ═══════════════════════════════════════════════════════════════════════════

#define TEST(name) \
    void test_##name(); \
    struct TestRunner_##name { \
        TestRunner_##name() { \
            std::cout << "Running: " << #name << "..." << std::flush; \
            test_##name(); \
            std::cout << " OK" << std::endl; \
        } \
    } test_runner_##name; \
    void test_##name()

#define ASSERT_TRUE(cond) \
    do { \
        if (!(cond)) { \
            std::cerr << "\nASSERT_TRUE failed: " #cond << std::endl; \
            std::abort(); \
        } \
    } while (0)

static Transaction makeCoinbaseTx() {
    Transaction tx;
    tx.version = 2;

    TxInput in;
    in.prevout.txid = TxId(uint256());   // coinbase
    in.prevout.vout = 0xffffffff;
    in.scriptSig = {0x03, 0x01, 0x00, 0x00};  // dummy height push
    in.sequence = 0xffffffff;
    tx.vin.push_back(in);

    TxOutput out;
    out.value = AmountUna::Una(50);
    // P2WPKH-like dummy script: OP_0 <20 bytes>
    out.scriptPubKey = {0x00, 0x14};
    out.scriptPubKey.insert(out.scriptPubKey.end(), 20, 0x00);
    tx.vout.push_back(out);

    // Ensure witness flag matches actual data (none)
    tx.DetectWitnessVersion();

    return tx;
}

TEST(BlockRoundTrip_SerializeDeserialize) {
    Transaction coinbase = makeCoinbaseTx();

    Block block;
    block.header.version = 1;
    block.header.prev_block_hash = uint256();
    block.header.merkle_root = coinbase.GetTxid().AsUint256();
    block.header.utreexo_root = uint256();
    block.header.timestamp = 123456789;
    block.header.difficulty = 0x207fffff;
    block.header.nonce = 0;
    block.header.ZeroReserved();
    block.vtx.push_back(coinbase);
    block.utreexo.reset();

    std::string serialized = block.Serialize();
    std::vector<uint8_t> bytes(serialized.begin(), serialized.end());

    auto decoded_opt = Block::Deserialize(bytes);
    ASSERT_TRUE(decoded_opt.has_value());
    const Block& decoded = decoded_opt.value();

    // Header and tx count preserved
    ASSERT_TRUE(decoded.vtx.size() == 1);
    ASSERT_TRUE(decoded.header.merkle_root == block.header.merkle_root);

    // Full byte-for-byte round-trip identity
    std::string reserialized = decoded.Serialize();
    ASSERT_TRUE(reserialized == serialized);
}

int main() {
    std::cout << "\n===========================================================\n";
    std::cout << "BLOCK SERIALIZATION ROUND-TRIP TESTS\n";
    std::cout << "===========================================================\n\n";

    // Tests run via static initialization

    std::cout << "\nALL BLOCK ROUND-TRIP TESTS PASSED!" << std::endl;
    std::cout << "===========================================================\n" << std::endl;
    return 0;
}
