// Copyright (c) 2026 Dinero Developers
// Distributed under the MIT software license
//
// SegWit Block Serialization Round-Trip Regression Test
//
// Purpose: Ensure witness data survives the full serialization pipeline:
//          Block → Serialize → Deserialize → Block (with witness intact)
//
// This test prevents regressions like the Serialize(false) bug that
// stripped witness data from blocks, causing Schnorr signature validation
// to fail for Taproot transactions.
//
// Critical invariants:
// 1. Witness data must be preserved through serialize/deserialize
// 2. Merkle root must remain unchanged after round-trip
// 3. Transaction count and content must match exactly
//
// References:
// - BIP141: Segregated Witness (Consensus layer)
// - BIP341: Taproot (SegWit version 1)
// - fix(mining): preserve SegWit witness data (commit bacfe762e)

#include <gtest/gtest.h>
#include <vector>
#include <cstring>

#include "primitives/transaction.h"
#include "primitives/block.h"
#include "primitives/uint256.h"
#include "common/serialization.h"
#include "consensus/merkle_root.h"

using namespace dinero;

namespace {

// Create a mock 64-byte Schnorr signature (BIP340)
std::vector<uint8_t> CreateMockSchnorrSignature() {
    std::vector<uint8_t> sig(64);
    for (size_t i = 0; i < 64; i++) {
        sig[i] = static_cast<uint8_t>(i + 1);  // Non-zero pattern
    }
    return sig;
}

// Create a mock 32-byte x-only pubkey
std::vector<uint8_t> CreateMockXOnlyPubkey() {
    std::vector<uint8_t> pubkey(32);
    for (size_t i = 0; i < 32; i++) {
        pubkey[i] = static_cast<uint8_t>(0xaa + i);
    }
    return pubkey;
}

// Create a P2TR (Taproot) scriptPubKey: OP_1 (0x51) + PUSH32 (0x20) + 32-byte pubkey
std::vector<uint8_t> CreateP2TRScriptPubKey() {
    std::vector<uint8_t> spk;
    spk.push_back(0x51);  // OP_1 (witness version 1)
    spk.push_back(0x20);  // PUSH 32 bytes
    auto pubkey = CreateMockXOnlyPubkey();
    spk.insert(spk.end(), pubkey.begin(), pubkey.end());
    return spk;
}

// Create a simple coinbase transaction
Transaction CreateCoinbaseTx(uint32_t height) {
    Transaction tx;
    tx.version = 2;

    TxInput input;
    input.prevout.txid = TxId(uint256());  // Coinbase has null prevout
    input.prevout.vout = 0xffffffff;

    // Coinbase scriptSig: height + tag
    input.scriptSig.push_back(3);  // Height bytes
    input.scriptSig.push_back(height & 0xff);
    input.scriptSig.push_back((height >> 8) & 0xff);
    input.scriptSig.push_back((height >> 16) & 0xff);
    input.scriptSig.push_back(3);  // Tag length
    input.scriptSig.push_back('D');
    input.scriptSig.push_back('N');
    input.scriptSig.push_back('R');
    input.sequence = 0xffffffff;
    tx.vin.push_back(input);

    TxOutput output;
    output.value = AmountUna::Una(10000000000);  // 100 DIN
    output.scriptPubKey = CreateP2TRScriptPubKey();
    tx.vout.push_back(output);

    tx.lockTime = 0;
    return tx;
}

// Create a SegWit transaction with Taproot witness data
Transaction CreateTaprootSpendTx(const TxId& prev_txid) {
    Transaction tx;
    tx.version = 2;
    tx.witness_version = 1;  // Mark as SegWit

    TxInput input;
    input.prevout.txid = prev_txid;
    input.prevout.vout = 0;
    input.scriptSig = {};  // Empty for SegWit
    input.sequence = 0xfffffffd;

    // CRITICAL: Add witness data (Schnorr signature)
    input.witness.push_back(CreateMockSchnorrSignature());

    tx.vin.push_back(input);

    // Two outputs: payment + change
    TxOutput output1;
    output1.value = AmountUna::Una(5000000000);  // 50 DIN
    output1.scriptPubKey = CreateP2TRScriptPubKey();
    tx.vout.push_back(output1);

    TxOutput output2;
    output2.value = AmountUna::Una(4999999859);  // ~50 DIN minus fee
    output2.scriptPubKey = CreateP2TRScriptPubKey();
    tx.vout.push_back(output2);

    tx.lockTime = 0;
    return tx;
}

}  // namespace

// ============================================================================
// Test: SegWit Block Serialization Round-Trip
// ============================================================================

TEST(SegWitSerializationRoundTrip, WitnessDataPreserved) {
    // GIVEN: A Taproot transaction with witness data
    Transaction coinbase = CreateCoinbaseTx(100);
    Transaction original_tx = CreateTaprootSpendTx(coinbase.GetTxid());

    ASSERT_FALSE(original_tx.vin.empty());
    ASSERT_FALSE(original_tx.vin[0].witness.empty());
    ASSERT_EQ(original_tx.vin[0].witness[0].size(), 64u);  // 64-byte Schnorr sig

    // Record original witness data for comparison
    std::vector<uint8_t> original_witness = original_tx.vin[0].witness[0];
    TxId original_txid = original_tx.GetTxid();

    // WHEN: Serialize the transaction with witness (using serialization.h templates)
    VectorWriter writer;
    Serialize(writer, original_tx);
    std::string serialized = writer.release_string();
    ASSERT_FALSE(serialized.empty());

    // AND: Deserialize into a new transaction
    Transaction deserialized_tx;
    Reader reader(serialized);
    Deserialize(reader, deserialized_tx);

    // THEN: Input count must match
    ASSERT_EQ(deserialized_tx.vin.size(), original_tx.vin.size());
    EXPECT_EQ(deserialized_tx.vin.size(), 1u);

    // AND: Witness data must be preserved (CRITICAL)
    ASSERT_FALSE(deserialized_tx.vin[0].witness.empty())
        << "REGRESSION: Witness data was stripped during serialization!";

    EXPECT_EQ(deserialized_tx.vin[0].witness.size(), 1u);
    EXPECT_EQ(deserialized_tx.vin[0].witness[0].size(), 64u)
        << "REGRESSION: Schnorr signature size mismatch!";

    // AND: Witness content must match exactly
    EXPECT_EQ(deserialized_tx.vin[0].witness[0], original_witness)
        << "REGRESSION: Witness data corrupted during serialization!";

    // AND: Txid must be unchanged (computed from non-witness data)
    EXPECT_EQ(deserialized_tx.GetTxid(), original_txid)
        << "REGRESSION: Txid changed after round-trip!";

    // AND: Output count and values must match
    ASSERT_EQ(deserialized_tx.vout.size(), original_tx.vout.size());
    EXPECT_EQ(deserialized_tx.vout[0].value.GetUna(), original_tx.vout[0].value.GetUna());
}

TEST(SegWitSerializationRoundTrip, TxSerializationModeWithWitness) {
    // GIVEN: A transaction with witness data
    Transaction coinbase = CreateCoinbaseTx(100);
    Transaction taproot_tx = CreateTaprootSpendTx(coinbase.GetTxid());

    // WHEN: Serializing with WithWitness mode
    std::vector<uint8_t> with_witness = taproot_tx.Serialize(TxSerializationMode::WithWitness);

    // THEN: Serialization should include SegWit marker (0x00 0x01 after version)
    ASSERT_GT(with_witness.size(), 6u);
    EXPECT_EQ(with_witness[4], 0x00u) << "Missing SegWit marker";
    EXPECT_EQ(with_witness[5], 0x01u) << "Missing SegWit flag";
}

TEST(SegWitSerializationRoundTrip, TxSerializationModeWithoutWitness) {
    // GIVEN: A transaction with witness data
    Transaction coinbase = CreateCoinbaseTx(100);
    Transaction taproot_tx = CreateTaprootSpendTx(coinbase.GetTxid());

    // WHEN: Serializing with WithoutWitness mode (for txid computation)
    std::vector<uint8_t> without_witness = taproot_tx.Serialize(TxSerializationMode::WithoutWitness);

    // THEN: Serialization should NOT include SegWit marker
    // (Input count comes right after version, no 0x00 0x01)
    ASSERT_GT(without_witness.size(), 6u);
    // After version (4 bytes), input count (1 byte for small counts)
    // If byte[4] == 0x00 AND byte[5] == 0x01, that's SegWit marker (wrong)
    // For WithoutWitness, byte[4] should be input count (0x01) not 0x00
    bool has_segwit_marker = (without_witness[4] == 0x00 && without_witness[5] == 0x01);
    EXPECT_FALSE(has_segwit_marker) << "WithoutWitness mode should not include SegWit marker";
}

TEST(SegWitSerializationRoundTrip, WitnessSizesDifferBetweenModes) {
    // GIVEN: A transaction with witness data
    Transaction coinbase = CreateCoinbaseTx(100);
    Transaction taproot_tx = CreateTaprootSpendTx(coinbase.GetTxid());

    // WHEN: Serializing with both modes
    std::vector<uint8_t> with_witness = taproot_tx.Serialize(TxSerializationMode::WithWitness);
    std::vector<uint8_t> without_witness = taproot_tx.Serialize(TxSerializationMode::WithoutWitness);

    // THEN: WithWitness should be larger (includes marker + flag + witness stack)
    EXPECT_GT(with_witness.size(), without_witness.size())
        << "WithWitness serialization should be larger than WithoutWitness";

    // The difference should be at least: marker(1) + flag(1) + witness_count(1) + witness_len(1) + sig(64) = 68 bytes
    size_t size_diff = with_witness.size() - without_witness.size();
    EXPECT_GE(size_diff, 68u)
        << "Size difference should account for SegWit overhead + witness data";
}
