/**
 * Phase 2.2: Stateless Block Verification Tests
 *
 * Tests the pure stateless verification function.
 * No database required - pure consensus testing.
 *
 * These tests verify:
 * 1. Valid blocks pass verification
 * 2. Double-spend detection
 * 3. Invalid amount detection
 * 4. Coinbase validation
 * 5. GetBlockSubsidy correctness
 */

#include "consensus/stateless_verification.h"
#include "consensus/chainparams.h"
#include "consensus/utxo_snapshot_state.h"
#include "consensus/utreexo_accumulator.h"
#include "consensus/outpoint.h"
#include "consensus/utxo_entry.h"
#include "primitives/block.h"
#include "primitives/transaction.h"
#include "primitives/uint256.h"
#include "primitives/hash_domains.h"
#include "primitives/amount.h"
#include <iostream>
#include <random>
#include <ctime>

using namespace dinero;
using namespace dinero::consensus;

// Random number generator
static std::mt19937 g_rng(static_cast<unsigned>(std::time(nullptr)));

// Generate random hash
uint256 RandomHash() {
    uint256 hash;
    for (int i = 0; i < 32; i++) {
        hash.data[i] = static_cast<uint8_t>(g_rng() & 0xff);
    }
    return hash;
}

TxId RandomTxId() {
    return TxId(RandomHash());
}

// Generate random P2WPKH scriptPubKey
std::vector<uint8_t> RandomScriptPubKey() {
    std::vector<uint8_t> script(22);
    script[0] = 0x00;  // OP_0
    script[1] = 0x14;  // Push 20 bytes
    for (int i = 2; i < 22; i++) {
        script[i] = static_cast<uint8_t>(g_rng() & 0xff);
    }
    return script;
}

// =============================================================================
// Test 1: GetBlockSubsidy correctness
// =============================================================================
// Canonical schedule per ConsensusSubsidy in include/consensus/subsidy.h
// (Fair Launch v3 — no premine):
//   pow_blocks = height - 1 (PoW emission starts at height 1)
//   halvings   = pow_blocks / HALVING_INTERVAL (1,314,000)
//   subsidy    = max(INITIAL_SUBSIDY >> halvings, TAIL_EMISSION_UNA)
//
// Therefore the boundary heights are:
//   Height 0           : 0 (genesis OP_RETURN burn)
//   Height 1           : INITIAL_SUBSIDY = 100 DIN (first PoW block)
//   Height 1,314,000   : 100 DIN (last block before first halving)
//   Height 1,314,001   : 50 DIN (first halving)
//   Height 2,628,001   : 25 DIN (second halving)
bool TestGetBlockSubsidy() {
    std::cout << "\n[Test 1] GetBlockSubsidy Correctness" << std::endl;
    std::cout << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━" << std::endl;

    constexpr uint64_t UNA_PER_DIN = 100'000'000ULL;

    // Height 0: Genesis (0, unspendable)
    if (GetBlockSubsidy(0) != 0) {
        std::cout << "  [FAIL] Height 0 should be 0" << std::endl;
        return false;
    }

    // Height 1: First PoW block at INITIAL_SUBSIDY (100 DIN). No premine.
    if (GetBlockSubsidy(1) != 100ULL * UNA_PER_DIN) {
        std::cout << "  [FAIL] Height 1 should be 100 DIN (got "
                  << GetBlockSubsidy(1) / UNA_PER_DIN << ")" << std::endl;
        return false;
    }

    // Height 2: still INITIAL_SUBSIDY (100 DIN, halvings=0)
    if (GetBlockSubsidy(2) != 100ULL * UNA_PER_DIN) {
        std::cout << "  [FAIL] Height 2 should be 100 DIN" << std::endl;
        return false;
    }

    // Height 1,314,000: last block before first halving (100 DIN)
    if (GetBlockSubsidy(1'314'000) != 100ULL * UNA_PER_DIN) {
        std::cout << "  [FAIL] Height 1,314,000 should be 100 DIN (got "
                  << GetBlockSubsidy(1'314'000) / UNA_PER_DIN << ")" << std::endl;
        return false;
    }

    // Height 1,314,001: first halving (50 DIN)
    if (GetBlockSubsidy(1'314'001) != 50ULL * UNA_PER_DIN) {
        std::cout << "  [FAIL] First halving at 1,314,001 should be 50 DIN, got "
                  << GetBlockSubsidy(1'314'001) / UNA_PER_DIN << std::endl;
        return false;
    }

    // Height 2,628,001: second halving (25 DIN)
    if (GetBlockSubsidy(2'628'001) != 25ULL * UNA_PER_DIN) {
        std::cout << "  [FAIL] Second halving at 2,628,001 should be 25 DIN, got "
                  << GetBlockSubsidy(2'628'001) / UNA_PER_DIN << std::endl;
        return false;
    }

    std::cout << "  [PASS] GetBlockSubsidy verified for all key heights" << std::endl;
    return true;
}

// =============================================================================
// Test 2: Valid Block Verification
// =============================================================================
bool TestValidBlockVerification() {
    std::cout << "\n[Test 2] Valid Block Verification" << std::endl;
    std::cout << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━" << std::endl;

    // Create a simple block with coinbase only
    Block block;
    block.header.version = 1;
    block.header.prev_block_hash = RandomHash();
    block.header.timestamp = static_cast<uint64_t>(std::time(nullptr));
    block.header.difficulty = 0x1d00ffff;
    block.header.nonce = 12345;
    block.header.ZeroReserved();

    // Coinbase transaction
    Transaction coinbase;
    coinbase.version = 2;
    coinbase.lockTime = 0;
    coinbase.witness_version = 0;

    TxInput coinbase_input;
    coinbase_input.prevout.txid = TxId();  // Null
    coinbase_input.prevout.vout = 0xffffffff;
    coinbase_input.sequence = 0xffffffff;
    coinbase_input.scriptSig = {0x03, 0x0a, 0x00, 0x00, 0x00};  // Height 10
    coinbase.vin.push_back(coinbase_input);

    // Coinbase output - subsidy for height 10
    TxOutput coinbase_output;
    coinbase_output.value = AmountUna::Una(GetBlockSubsidy(10));
    coinbase_output.scriptPubKey = RandomScriptPubKey();
    coinbase.vout.push_back(coinbase_output);

    block.vtx.push_back(coinbase);

    // Create minimal context (no spent outputs for coinbase-only block)
    StatelessContext ctx;
    ctx.spent_outputs = nullptr;
    ctx.spent_count = 0;
    ctx.roots = nullptr;
    ctx.num_roots = 0;
    ctx.num_leaves = 0;
    ctx.height = 10;

    // Empty proof (no spends)
    BlockUtreexoProof proof;

    // Verify
    VerifyResult result = VerifyBlockStateless(block, ctx, proof);

    if (!result.valid()) {
        std::cout << "  [FAIL] Valid block rejected with error "
                  << static_cast<int>(result.error) << std::endl;
        return false;
    }

    if (result.outputs_created != 1) {
        std::cout << "  [FAIL] Expected 1 output, got " << result.outputs_created << std::endl;
        return false;
    }

    std::cout << "  [PASS] Valid coinbase-only block verified" << std::endl;
    return true;
}

// =============================================================================
// Test 3: Double-Spend Detection
// =============================================================================
bool TestDoubleSpendDetection() {
    std::cout << "\n[Test 3] Double-Spend Detection" << std::endl;
    std::cout << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━" << std::endl;

    // Create a block with two transactions spending the same input
    Block block;
    block.header.version = 1;
    block.header.prev_block_hash = RandomHash();
    block.header.timestamp = static_cast<uint64_t>(std::time(nullptr));
    block.header.difficulty = 0x1d00ffff;
    block.header.nonce = 12345;
    block.header.ZeroReserved();

    // Coinbase
    Transaction coinbase;
    coinbase.version = 2;
    TxInput coinbase_input;
    coinbase_input.prevout.txid = TxId();
    coinbase_input.prevout.vout = 0xffffffff;
    coinbase_input.sequence = 0xffffffff;
    coinbase.vin.push_back(coinbase_input);
    TxOutput coinbase_output;
    coinbase_output.value = AmountUna::Una(GetBlockSubsidy(10) + 2000);  // subsidy + fees
    coinbase_output.scriptPubKey = RandomScriptPubKey();
    coinbase.vout.push_back(coinbase_output);
    block.vtx.push_back(coinbase);

    // Same outpoint for both transactions
    TxId shared_txid = RandomTxId();

    // Transaction 1: spends shared outpoint
    Transaction tx1;
    tx1.version = 2;
    TxInput input1;
    input1.prevout.txid = shared_txid;
    input1.prevout.vout = 0;
    input1.sequence = 0xfffffffe;
    tx1.vin.push_back(input1);
    TxOutput output1;
    output1.value = AmountUna::Una(999000);
    output1.scriptPubKey = RandomScriptPubKey();
    tx1.vout.push_back(output1);
    block.vtx.push_back(tx1);

    // Transaction 2: also spends same outpoint (DOUBLE SPEND)
    Transaction tx2;
    tx2.version = 2;
    TxInput input2;
    input2.prevout.txid = shared_txid;  // Same as tx1!
    input2.prevout.vout = 0;            // Same as tx1!
    input2.sequence = 0xfffffffe;
    tx2.vin.push_back(input2);
    TxOutput output2;
    output2.value = AmountUna::Una(999000);
    output2.scriptPubKey = RandomScriptPubKey();
    tx2.vout.push_back(output2);
    block.vtx.push_back(tx2);

    // Create spent outputs (need 2, one for each tx)
    SpentOutputData spent1(1000000, RandomScriptPubKey());
    SpentOutputData spent2(1000000, RandomScriptPubKey());
    std::vector<SpentOutputData> spent_outputs = {spent1, spent2};

    std::vector<UtreexoHash> targets;
    targets.reserve(2);
    targets.push_back(HashUTXOLegacy(shared_txid.AsUint256(), 0, spent1.value, spent1.scriptPubKey));
    targets.push_back(HashUTXOLegacy(shared_txid.AsUint256(), 0, spent2.value, spent2.scriptPubKey));

    UtreexoForest forest;
    for (const auto& target : targets) {
        forest.add(target);
    }
    std::vector<UtreexoHash> roots = forest.getRoots();

    StatelessContext ctx;
    ctx.spent_outputs = spent_outputs.data();
    ctx.spent_count = static_cast<uint32_t>(spent_outputs.size());
    ctx.roots = roots.data();
    ctx.num_roots = static_cast<uint8_t>(roots.size());
    ctx.num_leaves = forest.getNumLeaves();
    ctx.height = 10;

    BlockUtreexoProof proof = forest.generateBlockProof(targets);

    // Verify - should fail with DOUBLE_SPEND
    VerifyResult result = VerifyBlockStateless(block, ctx, proof);

    if (result.valid()) {
        std::cout << "  [FAIL] Double-spend block should be rejected" << std::endl;
        return false;
    }

    if (result.error != VerifyError::DOUBLE_SPEND) {
        std::cout << "  [FAIL] Expected DOUBLE_SPEND error, got "
                  << static_cast<int>(result.error) << std::endl;
        return false;
    }

    std::cout << "  [PASS] Double-spend correctly detected" << std::endl;
    return true;
}

// =============================================================================
// Test 4: Invalid Amount Detection
// =============================================================================
bool TestInvalidAmountDetection() {
    std::cout << "\n[Test 4] Invalid Amount Detection" << std::endl;
    std::cout << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━" << std::endl;

    // Create a block where outputs exceed inputs + subsidy
    Block block;
    block.header.version = 1;
    block.header.prev_block_hash = RandomHash();
    block.header.timestamp = static_cast<uint64_t>(std::time(nullptr));
    block.header.difficulty = 0x1d00ffff;
    block.header.nonce = 12345;
    block.header.ZeroReserved();

    // Coinbase - claiming more than allowed
    Transaction coinbase;
    coinbase.version = 2;
    TxInput coinbase_input;
    coinbase_input.prevout.txid = TxId();
    coinbase_input.prevout.vout = 0xffffffff;
    coinbase_input.sequence = 0xffffffff;
    coinbase.vin.push_back(coinbase_input);
    TxOutput coinbase_output;
    // Claim way more than subsidy
    coinbase_output.value = AmountUna::Una(GetBlockSubsidy(10) * 2);
    coinbase_output.scriptPubKey = RandomScriptPubKey();
    coinbase.vout.push_back(coinbase_output);
    block.vtx.push_back(coinbase);

    StatelessContext ctx;
    ctx.spent_outputs = nullptr;
    ctx.spent_count = 0;
    ctx.roots = nullptr;
    ctx.num_roots = 0;
    ctx.num_leaves = 0;
    ctx.height = 10;

    BlockUtreexoProof proof;

    // Verify - should fail with INVALID_AMOUNT (outputs exceed inputs + subsidy)
    VerifyResult result = VerifyBlockStateless(block, ctx, proof);

    if (result.valid()) {
        std::cout << "  [FAIL] Invalid amount block should be rejected" << std::endl;
        return false;
    }

    // For coinbase-only block, excess outputs trigger INVALID_AMOUNT
    // (the general "outputs > inputs + subsidy" check)
    if (result.error != VerifyError::INVALID_AMOUNT) {
        std::cout << "  [FAIL] Expected INVALID_AMOUNT error, got "
                  << static_cast<int>(result.error) << std::endl;
        return false;
    }

    std::cout << "  [PASS] Invalid amount correctly detected" << std::endl;
    return true;
}

// =============================================================================
// Test 5: Empty Block Rejection
// =============================================================================
bool TestEmptyBlockRejection() {
    std::cout << "\n[Test 5] Empty Block Rejection" << std::endl;
    std::cout << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━" << std::endl;

    // Create empty block (no transactions)
    Block block;
    block.header.version = 1;
    block.header.prev_block_hash = RandomHash();
    block.header.timestamp = static_cast<uint64_t>(std::time(nullptr));
    block.header.difficulty = 0x1d00ffff;
    block.header.nonce = 12345;
    block.header.ZeroReserved();
    // No transactions!

    StatelessContext ctx;
    ctx.spent_outputs = nullptr;
    ctx.spent_count = 0;
    ctx.roots = nullptr;
    ctx.num_roots = 0;
    ctx.num_leaves = 0;
    ctx.height = 10;

    BlockUtreexoProof proof;

    VerifyResult result = VerifyBlockStateless(block, ctx, proof);

    if (result.valid()) {
        std::cout << "  [FAIL] Empty block should be rejected" << std::endl;
        return false;
    }

    if (result.error != VerifyError::INVALID_COINBASE) {
        std::cout << "  [FAIL] Expected INVALID_COINBASE error, got "
                  << static_cast<int>(result.error) << std::endl;
        return false;
    }

    std::cout << "  [PASS] Empty block correctly rejected" << std::endl;
    return true;
}

// =============================================================================
// Test 6: VerifyResult POD Properties
// =============================================================================
bool TestVerifyResultPOD() {
    std::cout << "\n[Test 6] VerifyResult POD Properties" << std::endl;
    std::cout << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━" << std::endl;

    // Verify VerifyResult is trivially copyable (POD-like)
    static_assert(std::is_trivially_copyable_v<VerifyResult>,
                  "VerifyResult must be trivially copyable");

    // Test constexpr construction
    constexpr VerifyResult ok = VerifyResult::Ok(100, 5, 3);
    static_assert(ok.valid(), "Ok result must be valid");
    static_assert(ok.fees == 100, "Fees must match");

    constexpr VerifyResult fail = VerifyResult::Fail(VerifyError::DOUBLE_SPEND);
    static_assert(!fail.valid(), "Fail result must not be valid");

    std::cout << "  [PASS] VerifyResult is POD-like (trivially copyable, constexpr)" << std::endl;
    return true;
}

// =============================================================================
// Main
// =============================================================================
int main() {
    SelectParams(Chain::MAINNET);

    std::cout << "════════════════════════════════════════════════════════════════" << std::endl;
    std::cout << "Phase 2.2: Stateless Block Verification Tests" << std::endl;
    std::cout << "════════════════════════════════════════════════════════════════" << std::endl;
    std::cout << "Pure function testing - no database required" << std::endl;
    std::cout << "Seed: " << static_cast<unsigned>(std::time(nullptr)) << std::endl;

    int passed = 0;
    int failed = 0;

    if (TestGetBlockSubsidy()) passed++; else failed++;
    if (TestValidBlockVerification()) passed++; else failed++;
    if (TestDoubleSpendDetection()) passed++; else failed++;
    if (TestInvalidAmountDetection()) passed++; else failed++;
    if (TestEmptyBlockRejection()) passed++; else failed++;
    if (TestVerifyResultPOD()) passed++; else failed++;

    std::cout << "\n════════════════════════════════════════════════════════════════" << std::endl;
    std::cout << "Results: " << passed << " passed, " << failed << " failed" << std::endl;
    std::cout << "════════════════════════════════════════════════════════════════" << std::endl;

    if (failed > 0) {
        std::cout << "FAILED: Some tests did not pass" << std::endl;
        return 1;
    }

    std::cout << "SUCCESS: All stateless verification tests passed" << std::endl;
    return 0;
}
