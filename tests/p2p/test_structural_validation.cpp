/**
 * Phase G.3.2: Structural Validation - Pure Unit Tests
 *
 * Test Scope:
 * - Valid block passes structural checks
 * - Invalid merkle root rejected
 * - Oversized block rejected
 * - Empty tx list rejected
 * - Coinbase not first rejected
 * - Valid tx passes structural checks
 * - Oversized tx rejected
 * - Duplicate inputs rejected
 * - Malformed serialization rejected
 *
 * Test Constraints:
 * - NO script execution
 * - NO UTXO lookups
 * - NO locktime/sequence semantics
 * - NO coinbase subsidy checks
 * - NO chainstate access
 * - NO disk writes
 */

#include "../../include/p2p/structural_validator.h"

#include "consensus/utreexo_accumulator.h"
#include "consensus/merkle_root.h"
#include "primitives/block.h"
#include "primitives/transaction.h"

#include <cassert>
#include <chrono>
#include <iostream>
#include <vector>

using namespace dinero;
using namespace dinero::p2p;

namespace {

TxInput MakeCoinbaseInput() {
    TxInput in;
    in.prevout = TxOutPoint(TxId(), 0xffffffffU);
    in.scriptSig = {0x51};
    in.sequence = 0xffffffffU;
    return in;
}

TxInput MakeSpendInput(uint8_t seed, uint32_t vout = 0) {
    TxInput in;
    uint256 txid;
    txid.data[0] = seed;
    txid.data[1] = static_cast<uint8_t>(seed + 1);
    in.prevout = TxOutPoint(TxId(txid), vout);
    in.scriptSig = {0x51};
    in.sequence = 0xfffffffdU;
    return in;
}

TxOutput MakeOutput(AmountUna amount) {
    TxOutput out(amount, {0x51});
    return out;
}

Transaction MakeCoinbaseTx() {
    Transaction tx;
    tx.witness_version = 0xFF;
    tx.vin = {MakeCoinbaseInput()};
    tx.vout = {MakeOutput(AmountUna::DIN(50))};
    tx.lockTime = 0;
    return tx;
}

Transaction MakeStandardTx() {
    Transaction tx;
    tx.witness_version = 0xFF;
    tx.vin = {MakeSpendInput(0x11)};
    tx.vout = {MakeOutput(AmountUna::DIN(1))};
    tx.lockTime = 0;
    return tx;
}

std::vector<uint8_t> SerializeTx(const Transaction& tx) {
    return tx.Serialize(TxSerializationMode::WithWitness);
}

std::vector<uint8_t> SerializeBlock(const std::vector<Transaction>& txs) {
    Block block;
    block.header.version = 1;
    block.header.prev_block_hash = uint256();
    block.header.utreexo_root = uint256();
    block.header.timestamp = 1;
    block.header.difficulty = 0x1d00ffffU;
    block.header.nonce = 0;
    block.header.ZeroReserved();
    block.vtx = txs;
    block.header.merkle_root = consensus::ComputeMerkleRoot(block.vtx);

    const std::string bytes = block.Serialize();
    return std::vector<uint8_t>(bytes.begin(), bytes.end());
}

std::vector<uint8_t> SerializeBlockWithUtreexo(
    const std::vector<Transaction>& txs,
    const consensus::BlockUtreexoData& utreexo) {
    Block block;
    block.header.version = 1;
    block.header.prev_block_hash = uint256();
    block.header.utreexo_root = uint256();
    block.header.timestamp = 1;
    block.header.difficulty = 0x1d00ffffU;
    block.header.nonce = 0;
    block.header.ZeroReserved();
    block.vtx = txs;
    block.utreexo = utreexo;
    block.header.merkle_root = consensus::ComputeMerkleRoot(block.vtx);

    const std::string bytes = block.Serialize();
    return std::vector<uint8_t>(bytes.begin(), bytes.end());
}

std::vector<uint8_t> CreateValidBlock() {
    return SerializeBlock({MakeCoinbaseTx()});
}

std::vector<uint8_t> CreateValidTx() {
    return SerializeTx(MakeStandardTx());
}

} // namespace

void test_valid_block_passes() {
    std::cout << "\n[Test 1] Valid block passes structural validation" << std::endl;
    StructuralValidator validator;
    auto result = validator.validateBlock(CreateValidBlock());
    assert(result.ok && "Valid block should pass");
    std::cout << "  [✓] Valid block passes!" << std::endl;
}

void test_invalid_merkle_rejected() {
    std::cout << "\n[Test 2] Invalid merkle root rejected" << std::endl;
    StructuralValidator validator;
    auto block = CreateValidBlock();
    block[0x24] ^= 0x01;
    auto result = validator.validateBlock(block);
    assert(!result.ok && "Invalid merkle root should be rejected");
    assert(result.error.find("merkle") != std::string::npos && "Error should mention merkle root");
    std::cout << "  [✓] Invalid merkle root rejected!" << std::endl;
}

void test_oversized_block_rejected() {
    std::cout << "\n[Test 3] Oversized block rejected" << std::endl;
    StructuralValidator validator;
    std::vector<uint8_t> huge_block(5 * 1024 * 1024, 0xFF);
    auto result = validator.validateBlock(huge_block);
    assert(!result.ok && "Oversized block should be rejected");
    assert(result.error.find("size") != std::string::npos && "Error should mention size");
    std::cout << "  [✓] Oversized block rejected!" << std::endl;
}

void test_empty_tx_list_rejected() {
    std::cout << "\n[Test 4] Empty tx list rejected" << std::endl;
    StructuralValidator validator;
    BlockHeader header{};
    header.version = 1;
    header.ZeroReserved();
    header.timestamp = 1;
    header.difficulty = 0x1d00ffffU;
    std::string bytes = header.Serialize();
    bytes.push_back('\x00');
    auto block = std::vector<uint8_t>(bytes.begin(), bytes.end());
    auto result = validator.validateBlock(block);
    assert(!result.ok && "Empty tx list should be rejected");
    assert(result.error.find("at least one transaction") != std::string::npos);
    std::cout << "  [✓] Empty tx list rejected!" << std::endl;
}

void test_coinbase_not_first_rejected() {
    std::cout << "\n[Test 5] Coinbase not first rejected" << std::endl;
    StructuralValidator validator;
    auto block = SerializeBlock({MakeStandardTx(), MakeCoinbaseTx()});
    auto result = validator.validateBlock(block);
    assert(!result.ok && "Coinbase-not-first block should be rejected");
    assert(result.error.find("coinbase") != std::string::npos);
    std::cout << "  [✓] Coinbase not first rejected!" << std::endl;
}

void test_valid_tx_passes() {
    std::cout << "\n[Test 6] Valid tx passes structural validation" << std::endl;
    StructuralValidator validator;
    auto result = validator.validateTx(CreateValidTx());
    assert(result.ok && "Valid tx should pass");
    std::cout << "  [✓] Valid tx passes!" << std::endl;
}

void test_oversized_tx_rejected() {
    std::cout << "\n[Test 7] Oversized tx rejected" << std::endl;
    StructuralValidator validator;
    std::vector<uint8_t> huge_tx(150 * 1024, 0xFF);
    auto result = validator.validateTx(huge_tx);
    assert(!result.ok && "Oversized tx should be rejected");
    assert(result.error.find("size") != std::string::npos && "Error should mention size");
    std::cout << "  [✓] Oversized tx rejected!" << std::endl;
}

void test_duplicate_inputs_rejected() {
    std::cout << "\n[Test 8] Duplicate inputs rejected" << std::endl;
    StructuralValidator validator;
    Transaction tx;
    tx.witness_version = 0xFF;
    TxInput shared = MakeSpendInput(0x22, 7);
    tx.vin = {shared, shared};
    tx.vout = {MakeOutput(AmountUna::DIN(1))};
    auto result = validator.validateTx(SerializeTx(tx));
    assert(!result.ok && "Duplicate-input transaction should be rejected");
    assert(result.error.find("duplicate inputs") != std::string::npos);
    std::cout << "  [✓] Duplicate inputs rejected!" << std::endl;
}

void test_malformed_payloads_rejected() {
    std::cout << "\n[Test 9] Malformed payloads rejected" << std::endl;
    StructuralValidator validator;

    auto malformed_block = CreateValidBlock();
    malformed_block.resize(64);
    auto block_result = validator.validateBlock(malformed_block);
    assert(!block_result.ok && "Malformed block should be rejected");

    auto malformed_tx = CreateValidTx();
    malformed_tx.pop_back();
    auto tx_result = validator.validateTx(malformed_tx);
    assert(!tx_result.ok && "Malformed tx should be rejected");

    std::cout << "  [✓] Malformed payloads rejected!" << std::endl;
}

void test_empty_payload_rejected() {
    std::cout << "\n[Test 10] Empty payload rejected" << std::endl;
    StructuralValidator validator;
    std::vector<uint8_t> empty;
    auto block_result = validator.validateBlock(empty);
    assert(!block_result.ok && "Empty block should be rejected");
    auto tx_result = validator.validateTx(empty);
    assert(!tx_result.ok && "Empty tx should be rejected");
    std::cout << "  [✓] Empty payloads rejected!" << std::endl;
}

void test_impossible_tx_count_rejected() {
    std::cout << "\n[Test 11] Impossible transaction count rejected early" << std::endl;
    StructuralValidator validator;

    BlockHeader header{};
    header.version = 1;
    header.ZeroReserved();
    header.timestamp = 1;
    header.difficulty = 0x1d00ffffU;

    std::string bytes = header.Serialize();
    bytes.push_back('\xfd');      // CompactSize 16-bit
    bytes.push_back('\xc8');      // 200 txs
    bytes.push_back('\x00');
    bytes.append(8, '\x00');      // Far too little room for 200 txs

    auto block = std::vector<uint8_t>(bytes.begin(), bytes.end());
    auto result = validator.validateBlock(block);
    assert(!result.ok && "Impossible tx count should be rejected");
    assert(result.error.find("impossible") != std::string::npos);
    std::cout << "  [✓] Impossible transaction count rejected!" << std::endl;
}

void test_utreexo_spent_output_mismatch_rejected() {
    std::cout << "\n[Test 12] Utreexo spent-output mismatch rejected" << std::endl;
    StructuralValidator validator;

    consensus::BlockUtreexoData utreexo;
    utreexo.accumulator_root_before.assign(32, 0x11);
    utreexo.spend_proof.numLeaves = 42;
    utreexo.spend_proof.targets.push_back(consensus::UtreexoHash(32, 0x22));
    utreexo.spend_proof.positions.push_back(7);
    utreexo.spend_proof.proof_hashes.push_back(consensus::UtreexoHash(32, 0x33));
    // Intentionally omit spent_outputs for the one non-coinbase input.

    auto block = SerializeBlockWithUtreexo({MakeCoinbaseTx(), MakeStandardTx()}, utreexo);
    auto result = validator.validateBlock(block);
    assert(!result.ok && "Utreexo spent-output mismatch should be rejected");
    assert(result.error.find("spent-output count") != std::string::npos);
    std::cout << "  [✓] Utreexo spent-output mismatch rejected!" << std::endl;
}

void test_utreexo_trailing_bytes_rejected() {
    std::cout << "\n[Test 13] Utreexo trailing bytes rejected" << std::endl;
    StructuralValidator validator;

    consensus::BlockUtreexoData utreexo;
    utreexo.accumulator_root_before.assign(32, 0x44);
    auto block = SerializeBlockWithUtreexo({MakeCoinbaseTx()}, utreexo);
    block.push_back(0x99);  // Trailing garbage after canonical payload

    auto result = validator.validateBlock(block);
    assert(!result.ok && "Utreexo trailing bytes should be rejected");
    assert(result.error.find("Trailing bytes") != std::string::npos);
    std::cout << "  [✓] Utreexo trailing bytes rejected!" << std::endl;
}

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "G.3.2: Structural Validation Tests" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << "\nPure structural validation tests" << std::endl;
    std::cout << "Deserialize + internal consistency ONLY" << std::endl;
    std::cout << "NO scripts | NO UTXO | NO chainstate" << std::endl;

    auto start = std::chrono::steady_clock::now();

    try {
        test_valid_block_passes();
        test_invalid_merkle_rejected();
        test_oversized_block_rejected();
        test_empty_tx_list_rejected();
        test_coinbase_not_first_rejected();
        test_valid_tx_passes();
        test_oversized_tx_rejected();
        test_duplicate_inputs_rejected();
        test_malformed_payloads_rejected();
        test_empty_payload_rejected();
        test_impossible_tx_count_rejected();
        test_utreexo_spent_output_mismatch_rejected();
        test_utreexo_trailing_bytes_rejected();

        auto end = std::chrono::steady_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

        std::cout << "\n========================================" << std::endl;
        std::cout << "✅ All Structural Validation Tests Passed!" << std::endl;
        std::cout << "========================================" << std::endl;
        std::cout << "\nRuntime: " << duration.count() << " ms" << std::endl;
        std::cout << "\nSummary:" << std::endl;
        std::cout << "  [✓] Valid block passes" << std::endl;
        std::cout << "  [✓] Invalid merkle root rejected" << std::endl;
        std::cout << "  [✓] Oversized block rejected" << std::endl;
        std::cout << "  [✓] Empty tx list rejected" << std::endl;
        std::cout << "  [✓] Coinbase not first rejected" << std::endl;
        std::cout << "  [✓] Valid tx passes" << std::endl;
        std::cout << "  [✓] Oversized tx rejected" << std::endl;
        std::cout << "  [✓] Duplicate inputs rejected" << std::endl;
        std::cout << "  [✓] Malformed payloads rejected" << std::endl;
        std::cout << "  [✓] Empty payloads rejected" << std::endl;
        std::cout << "  [✓] Impossible transaction count rejected" << std::endl;
        std::cout << "  [✓] Utreexo spent-output mismatch rejected" << std::endl;
        std::cout << "  [✓] Utreexo trailing bytes rejected" << std::endl;
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "\n❌ Test failed with exception: " << e.what() << std::endl;
        return 1;
    } catch (...) {
        std::cerr << "\n❌ Test failed with unknown exception" << std::endl;
        return 1;
    }
}
