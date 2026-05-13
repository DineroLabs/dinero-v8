/**
 * Phase 10d: Utreexo Height-2 Root Lock Test
 *
 * CRITICAL CONSENSUS TEST
 *
 * This test locks down the Utreexo root at height 2 (second PoW block after
 * the fair-launch genesis).
 * If this test fails, consensus has changed and a chain fork is imminent.
 *
 * What This Test Protects:
 * - Utreexo accumulator determinism
 * - UTXO ordering stability
 * - Hash encoding correctness
 * - Coinbase format consistency
 * - UTXO leaf hash computation
 *
 * WARNING:
 * Changing the expected root WILL fork the chain.
 * Only modify if you are making an intentional consensus change.
 */

#include <gtest/gtest.h>
#include "consensus/chainparams.h"
#include "consensus/utreexo_accumulator.h"
#include "consensus/subsidy.h"
#include "primitives/block.h"
#include "primitives/transaction.h"
#include "primitives/amount.h"
#include "common/sha256d.h"

using namespace dinero;
using namespace dinero::consensus;

// Helper: Convert hex string to bytes
std::vector<uint8_t> hexToBytes(const std::string& hex) {
    std::vector<uint8_t> bytes;
    bytes.reserve(hex.length() / 2);
    for (size_t i = 0; i < hex.length(); i += 2) {
        uint8_t byte = static_cast<uint8_t>(std::stoul(hex.substr(i, 2), nullptr, 16));
        bytes.push_back(byte);
    }
    return bytes;
}

/**
 * Height-2 Root Lock Test
 *
 * This test ensures that adding UTXOs in the canonical order
 * (genesis -> height 1 PoW -> height 2 PoW) always produces the same
 * deterministic Utreexo root.
 *
 * Chain Structure:
 * - Height 0: Genesis block burns 100 DIN via OP_RETURN (unspendable)
 * - Height 1: First PoW block creates UTXO #1
 * - Height 2: Second PoW block creates UTXO #2
 *
 * The root after adding these 2 UTXOs is consensus-critical and must never change.
 */
TEST(UtreexoConsensus, Height2RootLock_Mainnet) {
    // 1. Load MAINNET params
    SelectParams(Chain::MAINNET);
    const ChainParams& params = Params();

    // 2. Initialize Utreexo forest
    UtreexoForest forest;

    // 3. Create UTXO #1: first PoW coinbase output (height 1, output 0)
    //
    // In a real blockchain, this would be created by:
    // - Height 1 coinbase transaction
    // - Output 0
    // - Amount: Block subsidy for height 1
    // - ScriptPubKey: Genesis address (placeholder for this test)

    std::vector<uint8_t> placeholder_script = {
        0x76, 0xa9, 0x14,  // OP_DUP OP_HASH160 <20 bytes>
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x88, 0xac  // OP_EQUALVERIFY OP_CHECKSIG
    };

    // Create height 1 transaction to get its TxId
    Transaction height1_tx;

    // Coinbase input
    TxInput coinbase_input;
    coinbase_input.prevout.txid = TxId();  // Null
    coinbase_input.prevout.vout = 0xFFFFFFFF;
    coinbase_input.sequence = 0xFFFFFFFF;
    coinbase_input.scriptSig = {0x01, 0x01};  // BIP34: Height = 1

    height1_tx.vin.push_back(coinbase_input);

    TxOutput height1_output;
    height1_output.value = ConsensusSubsidy::GetBlockSubsidy(1);
    height1_output.scriptPubKey = placeholder_script;
    ASSERT_EQ(height1_output.value.GetUna(), ConsensusSubsidy::INITIAL_SUBSIDY)
        << "Fair-launch height 1 must pay the initial PoW subsidy";

    height1_tx.vout.push_back(height1_output);

    // Compute height 1 TxId
    TxId height1_txid = height1_tx.GetTxid();

    // Create UTXO leaf for height 1 output
    // Utreexo leaf hash = Hash(outpoint || amount || scriptPubKey)
    std::vector<uint8_t> utxo1_data;

    // Outpoint: txid (32 bytes) + vout (4 bytes LE)
    std::vector<uint8_t> txid_bytes = hexToBytes(height1_txid.AsUint256().GetHex());
    utxo1_data.insert(utxo1_data.end(), txid_bytes.begin(), txid_bytes.end());
    uint32_t vout1 = 0;
    utxo1_data.push_back(vout1 & 0xFF);
    utxo1_data.push_back((vout1 >> 8) & 0xFF);
    utxo1_data.push_back((vout1 >> 16) & 0xFF);
    utxo1_data.push_back((vout1 >> 24) & 0xFF);

    // Amount (8 bytes LE)
    uint64_t amount1 = height1_output.value.GetUna();
    for (int i = 0; i < 8; i++) {
        utxo1_data.push_back((amount1 >> (i * 8)) & 0xFF);
    }

    // ScriptPubKey
    utxo1_data.insert(utxo1_data.end(), placeholder_script.begin(), placeholder_script.end());

    // Hash to get leaf hash
    std::vector<uint8_t> utxo1_hash = Dinero::Common::double_sha256_raw(utxo1_data);

    // Add UTXO #1 to forest
    forest.add(utxo1_hash);

    // 4. Create UTXO #2: Height 2 coinbase output (height 2, output 0)

    Transaction cb_tx;

    // Coinbase input
    TxInput cb_input;
    cb_input.prevout.txid = TxId();
    cb_input.prevout.vout = 0xFFFFFFFF;
    cb_input.sequence = 0xFFFFFFFF;
    cb_input.scriptSig = {0x01, 0x02};  // BIP34: Height = 2

    cb_tx.vin.push_back(cb_input);

    // Coinbase output
    TxOutput cb_output;
    cb_output.value = ConsensusSubsidy::GetBlockSubsidy(2);
    cb_output.scriptPubKey = placeholder_script;
    ASSERT_EQ(cb_output.value.GetUna(), ConsensusSubsidy::INITIAL_SUBSIDY)
        << "Fair-launch height 2 must pay the initial PoW subsidy";

    cb_tx.vout.push_back(cb_output);

    // Compute height 2 TxId
    TxId cb_txid = cb_tx.GetTxid();

    // Create UTXO leaf for height 2 output
    std::vector<uint8_t> utxo2_data;

    // Outpoint
    std::vector<uint8_t> cb_txid_bytes = hexToBytes(cb_txid.AsUint256().GetHex());
    utxo2_data.insert(utxo2_data.end(), cb_txid_bytes.begin(), cb_txid_bytes.end());
    uint32_t vout2 = 0;
    utxo2_data.push_back(vout2 & 0xFF);
    utxo2_data.push_back((vout2 >> 8) & 0xFF);
    utxo2_data.push_back((vout2 >> 16) & 0xFF);
    utxo2_data.push_back((vout2 >> 24) & 0xFF);

    // Amount
    uint64_t amount2 = cb_output.value.GetUna();
    for (int i = 0; i < 8; i++) {
        utxo2_data.push_back((amount2 >> (i * 8)) & 0xFF);
    }

    // ScriptPubKey
    utxo2_data.insert(utxo2_data.end(), placeholder_script.begin(), placeholder_script.end());

    // Hash to get leaf hash
    std::vector<uint8_t> utxo2_hash = Dinero::Common::double_sha256_raw(utxo2_data);

    // Add UTXO #2 to forest
    forest.add(utxo2_hash);

    // 5. Get the Utreexo root after adding both UTXOs
    uint256 computed_root;
    auto commitment = forest.getCommitment();
    if (commitment.size() == 32) {
        memcpy(computed_root.data, commitment.data(), 32);
    }

    // 6. LOCKED EXPECTED ROOT
    //
    // ═══════════════════════════════════════════════════════════════════════
    // 🔒 CONSENSUS LOCK (MAINNET)
    // ═══════════════════════════════════════════════════════════════════════
    // Utreexo root at height 2 is FROZEN.
    // Any change here WILL cause a hard fork.
    // Verified by test_utreexo_height2_root.cpp
    //
    // This is the Utreexo root after applying:
    // - Genesis (height 0)
    // - Height 1 PoW -> creates UTXO #1
    // - Height 2 PoW -> creates UTXO #2
    //
    // The root represents the accumulator state with:
    // - 1 UTXO from height 1 coinbase (height 1, output 0)
    // - 1 UTXO from height 2 coinbase (height 2, output 0)
    //
    // Frozen on: 2026-01-17.
    // Updated 2026-02-02 after Phase M.0 byte order fix.
    // Updated 2026-05-11 after v7 fair-launch chain identity/subsidy model:
    //   height 1 is the first PoW block, not a premine.
    // Previous: 3f6e1a6c9ee4f52c27584e5d2bd83a711b4f1fab91c55ae4014f99b47f6a3ed7
    // ═══════════════════════════════════════════════════════════════════════

    const uint256 EXPECTED_HEIGHT2_ROOT = uint256::FromHexUnsafe(
        "6c0b7fe869b22758be2c9169061044095a9d4fbb112eda2be313d6e9b3f485bb"
    );

    ASSERT_EQ(computed_root, EXPECTED_HEIGHT2_ROOT)
        << "Utreexo height-2 root mismatch: consensus change detected!\n"
        << "Expected: " << EXPECTED_HEIGHT2_ROOT.GetHex() << "\n"
        << "Got:      " << computed_root.GetHex() << "\n"
        << "\n"
        << "This means one of the following changed:\n"
        << "  - UTXO ordering\n"
        << "  - Hash encoding\n"
        << "  - Coinbase format\n"
        << "  - Accumulator logic\n"
        << "  - Block subsidy calculation\n"
        << "\n"
        << "If this is intentional, you MUST:\n"
        << "  1. Update EXPECTED_HEIGHT2_ROOT\n"
        << "  2. Coordinate a hard fork\n"
        << "  3. Update all documentation\n";
}

/**
 * Additional Test: Verify height-2 root changes with different inputs
 *
 * This test verifies that the root is NOT constant - it should change
 * if we modify the block content. This proves the test is actually checking
 * something meaningful.
 *
 * TODO: Implement this after basic test works
 */
// TEST(UtreexoConsensus, Height2RootChangesWithDifferentBlock) {
//     // Implementation deferred
// }

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
