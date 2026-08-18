/**
 * E2E Integration Test: Bridge→CSN Block Sync + TX Relay
 *
 * Wires the full pipeline:
 *   Bridge generates block proofs → CSN syncs via transition proofs
 *   → Bridge generates TX proofs → CSN validates relayed TX
 *
 * Phases:
 *   A. Bridge builds 5 blocks (addition-only), generates transition proofs
 *   B. CSN syncs all 5 blocks via ValidateWithTransitionProof
 *   C. Bridge generates per-input proofs for a spending TX, CSN validates
 *   D. Negative cases: tampered proof, stale root, proof count mismatch
 */

#include "consensus/utreexo_accumulator.h"
#include "consensus/interfaces/iutxo_provider.h"
#include "network/bridge_node.h"
#include "network/stateless_node.h"
#include "network/utreexo_messages.h"
#include "primitives/block.h"
#include "primitives/transaction.h"
#include "primitives/hash_domains.h"
#include "consensus/chainparams.h"
#include "storage/chain_db.h"
#include "storage/chain_write_token.h"
#include <iostream>
#include <optional>
#include <cassert>
#include <vector>
#include <cstring>
#include <unordered_map>
#include <filesystem>

using namespace dinero;
using namespace dinero::consensus;
using namespace dinero::network;

// ============================================================================
// Test Infrastructure
// ============================================================================

static int tests_passed = 0;
static int tests_total = 0;

#define TEST_ASSERT(cond, msg) do { \
    tests_total++; \
    if (!(cond)) { \
        std::cerr << "FAIL: " << msg << " (" << __FILE__ << ":" << __LINE__ << ")" << std::endl; \
        assert(false); \
    } else { \
        tests_passed++; \
    } \
} while(0)

// ============================================================================
// Mock UTXO Provider
// ============================================================================

class MockUTXOProvider : public IUTXOProvider {
public:
    void AddTestUTXO(const TxId& txid, uint32_t vout,
                     uint64_t value, const std::vector<uint8_t>& script,
                     uint32_t height = 1) {
        OutPoint op(txid, vout);
        UTXOEntry entry(AmountUna::Una(value), script, height, false);
        utxos_[op] = entry;
    }

    std::optional<UTXOEntry> GetUTXO(const OutPoint& outpoint) const override {
        auto it = utxos_.find(outpoint);
        if (it == utxos_.end()) return std::nullopt;
        return it->second;
    }

    bool AddUTXO(const OutPoint& outpoint, const UTXOEntry& entry) override {
        utxos_[outpoint] = entry;
        return true;
    }

    bool SpendUTXO(const OutPoint& outpoint, uint32_t) override {
        return utxos_.erase(outpoint) > 0;
    }

    bool DeleteUTXO(const OutPoint& outpoint) override {
        utxos_.erase(outpoint);
        return true;
    }

    bool HasUTXO(const OutPoint& outpoint) const override {
        return utxos_.count(outpoint) > 0;
    }

private:
    std::unordered_map<OutPoint, UTXOEntry> utxos_;
};

// ============================================================================
// Helpers
// ============================================================================

static std::vector<uint8_t> makeScript() {
    std::vector<uint8_t> script = {0x51, 0x20};  // P2TR prefix
    script.resize(34, 0x00);
    return script;
}

// Build a coinbase-only block with the given outputs.
// Each output becomes a UTXO in the forest.
static Block makeCoinbaseBlock(uint32_t height,
                               const std::vector<std::pair<uint64_t, std::vector<uint8_t>>>& outputs,
                               const uint256& prev_hash = uint256()) {
    Block block;
    std::memset(&block.header, 0, sizeof(BlockHeader));
    block.header.version = 1;
    block.header.prev_block_hash = prev_hash;
    block.header.timestamp = 1700000000 + height * 600;
    block.header.difficulty = 0x1d00ffff;
    block.header.nonce = height;  // Unique per block

    Transaction coinbase;
    coinbase.version = 2;
    coinbase.witness_version = 0xFF;  // Legacy
    TxInput cb_in;
    // Null txid + 0xFFFFFFFF = coinbase
    cb_in.prevout.vout = 0xFFFFFFFF;
    // Unique scriptSig per block so txid differs
    cb_in.scriptSig.resize(4);
    std::memcpy(cb_in.scriptSig.data(), &height, 4);
    coinbase.vin.push_back(cb_in);

    for (const auto& [value, script] : outputs) {
        TxOutput out;
        out.value = AmountUna::Una(value);
        out.scriptPubKey = script;
        coinbase.vout.push_back(out);
    }

    block.vtx.push_back(coinbase);
    return block;
}

// Track a UTXO created by a block's coinbase tx
struct TrackedUTXO {
    TxId txid;
    uint32_t vout;
    uint64_t value;
    std::vector<uint8_t> scriptPubKey;
    UtreexoHash leafHash;
};

// After building a block, extract the UTXOs that computeAdditionHashes will produce.
// This ensures our tracking matches what the forest will contain.
static std::vector<TrackedUTXO> extractUTXOsFromBlock(const Block& block) {
    std::vector<TrackedUTXO> result;
    for (const auto& tx : block.vtx) {
        TxId txid = tx.GetTxid();
        for (size_t n = 0; n < tx.vout.size(); n++) {
            TrackedUTXO u;
            u.txid = txid;
            u.vout = static_cast<uint32_t>(n);
            u.value = tx.vout[n].value.GetUna();
            u.scriptPubKey = tx.vout[n].scriptPubKey;
            u.leafHash = HashUTXOLegacy(txid.AsUint256(), u.vout, u.value, u.scriptPubKey);
            result.push_back(u);
        }
    }
    return result;
}

// ============================================================================
// E2E Test: Full Pipeline
// ============================================================================

static void test_e2e_block_sync_then_tx_relay() {
    std::cout << "E2E Test: Block sync (5 blocks) + TX relay..." << std::endl;

    // ========================================================================
    // Setup: Bridge forest + CSN forest (cloned at genesis)
    // ========================================================================
    UtreexoForest bridge_forest;
    auto mock_utxo = std::make_shared<MockUTXOProvider>();
    BridgeNode bridge(mock_utxo, &bridge_forest);

    // CSN starts with a clone of the empty forest
    UtreexoForest csn_forest;
    StatelessNode csn(&csn_forest);

    const int NUM_BLOCKS = 5;
    auto script = makeScript();

    // Storage for generated proofs (indexed by block number 1..5)
    struct BlockData {
        Block block;
        UtreexoTransitionProof tp;
        UtreexoProofMessage proof_msg;
        std::vector<TrackedUTXO> utxos;
    };
    std::vector<BlockData> blocks;

    // ========================================================================
    // Phase A: Bridge builds 5 blocks, generates transition proofs
    // ========================================================================
    std::cout << "  Phase A: Bridge builds blocks..." << std::endl;

    uint256 prev_hash;
    for (int b = 1; b <= NUM_BLOCKS; b++) {
        uint64_t val1 = 1000 * b;
        uint64_t val2 = 1000 * b + 500;

        Block block = makeCoinbaseBlock(b, {{val1, script}, {val2, script}}, prev_hash);

        // Capture pre-block state
        UtreexoHash root_before = bridge_forest.getCommitment();

        // Generate block proof (against pre-block forest)
        // For addition-only blocks, spend_proof is empty
        BlockUtreexoData proof_data = bridge.GenerateProofForBlock(block, b);

        // Generate transition proof (against pre-block forest)
        UtreexoTransitionProof tp = UtreexoTransitionProof::generate(
            bridge_forest, block, proof_data.spend_proof);

        // Now apply block to bridge forest: add all output UTXOs
        auto utxos = extractUTXOsFromBlock(block);
        for (const auto& u : utxos) {
            bridge_forest.add(u.leafHash);
            mock_utxo->AddTestUTXO(u.txid, u.vout, u.value, u.scriptPubKey, b);
        }

        UtreexoHash root_after = bridge_forest.getCommitment();

        // Build proof message for CSN
        UtreexoProofMessage proof_msg;
        proof_msg.block_hash = block.GetHash();
        proof_msg.block_height = b;
        proof_msg.accumulator_root_before = root_before;
        proof_msg.accumulator_root_after = root_after;
        proof_msg.proof_data = proof_data;

        // Verify transition proof self-consistency
        TEST_ASSERT(tp.verify(), "TP should verify for block " + std::to_string(b));
        TEST_ASSERT(tp.commitment_after == root_after,
                    "TP commitment_after must match forest root after block " + std::to_string(b));

        blocks.push_back({block, tp, proof_msg, utxos});
        prev_hash = block.GetHash();
    }

    TEST_ASSERT(blocks.size() == NUM_BLOCKS, "Should have 5 blocks");
    std::cout << "    Built " << NUM_BLOCKS << " blocks, "
              << NUM_BLOCKS * 2 << " UTXOs in forest" << std::endl;

    // ========================================================================
    // Phase B: CSN syncs all 5 blocks via transition proofs
    // ========================================================================
    std::cout << "  Phase B: CSN syncs blocks..." << std::endl;

    for (int i = 0; i < NUM_BLOCKS; i++) {
        const auto& bd = blocks[i];
        bool ok = csn.ValidateWithTransitionProof(
            bd.block, bd.proof_msg, bd.tp, /*peer_id=*/0);
        TEST_ASSERT(ok, "CSN must accept block " + std::to_string(i + 1));
    }

    // CSN should now be at the same commitment as bridge
    UtreexoHash csn_root = csn.GetCurrentAccumulatorRoot();
    UtreexoHash bridge_root = bridge_forest.getCommitment();
    TEST_ASSERT(csn_root == bridge_root,
                "CSN root must match bridge root after sync");

    std::cout << "    CSN synced " << NUM_BLOCKS << " blocks, roots match" << std::endl;

    // ========================================================================
    // Phase C: TX relay — bridge generates proofs, CSN validates
    // ========================================================================
    std::cout << "  Phase C: TX relay..." << std::endl;

    // Spend UTXOs from block 3 (outputs 0 and 1)
    const auto& block3_utxos = blocks[2].utxos;
    TEST_ASSERT(block3_utxos.size() == 2, "Block 3 should have 2 UTXOs");

    // Build spending transaction
    Transaction spending_tx;
    spending_tx.version = 2;
    spending_tx.witness_version = 0xFF;
    for (const auto& u : block3_utxos) {
        TxInput inp;
        inp.prevout.txid = u.txid;
        inp.prevout.vout = u.vout;
        spending_tx.vin.push_back(inp);
    }
    TxOutput out;
    out.value = AmountUna::Una(0);
    out.scriptPubKey = {0x6a};  // OP_RETURN
    spending_tx.vout.push_back(out);

    // Bridge generates per-input proofs
    auto tx_proofs = bridge.GenerateProofsForTransaction(spending_tx);
    TEST_ASSERT(tx_proofs.has_value(), "Bridge must generate TX proofs");
    TEST_ASSERT(tx_proofs->size() == 2, "Should have 2 proofs for 2 inputs");

    // CSN validates the relayed TX
    bool valid = csn.ValidateUtreexoTx(spending_tx, *tx_proofs, bridge_root);
    TEST_ASSERT(valid, "CSN must accept TX with valid proofs after sync");

    std::cout << "    TX relay validated (2 inputs, 2 proofs)" << std::endl;

    // ========================================================================
    // Phase D: Negative cases
    // ========================================================================
    std::cout << "  Phase D: Negative cases..." << std::endl;

    // D1: Tampered proof sibling
    {
        auto tampered = *tx_proofs;
        if (!tampered[0].first.siblings.empty()) {
            tampered[0].first.siblings[0][0] ^= 0xFF;
        }
        bool rejected = !csn.ValidateUtreexoTx(spending_tx, tampered, bridge_root);
        TEST_ASSERT(rejected, "D1: CSN must reject tampered proof");
    }

    // D2: Stale root (use root from after block 2, not block 5)
    {
        UtreexoHash stale_root = blocks[1].proof_msg.accumulator_root_after;
        bool rejected = !csn.ValidateUtreexoTx(spending_tx, *tx_proofs, stale_root);
        TEST_ASSERT(rejected, "D2: CSN must reject stale accumulator root");
    }

    // D3: Proof count mismatch (remove one proof)
    {
        auto short_proofs = *tx_proofs;
        short_proofs.pop_back();
        bool rejected = !csn.ValidateUtreexoTx(spending_tx, short_proofs, bridge_root);
        TEST_ASSERT(rejected, "D3: CSN must reject proof count mismatch");
    }

    // D4: Tampered SpentOutputData value
    {
        auto bad_spent = *tx_proofs;
        bad_spent[0].second.value = 99999999;
        bool rejected = !csn.ValidateUtreexoTx(spending_tx, bad_spent, bridge_root);
        TEST_ASSERT(rejected, "D4: CSN must reject tampered spent output value");
    }

    std::cout << "    All negative cases rejected" << std::endl;
    std::cout << "  PASSED" << std::endl;
}

// ============================================================================
// Test 2: Chained blocks with spends (deletion path)
// ============================================================================

static void test_e2e_block_with_spends() {
    std::cout << "E2E Test: Block with spends (deletion in transition proof)..." << std::endl;

    UtreexoForest bridge_forest;
    auto mock_utxo = std::make_shared<MockUTXOProvider>();
    BridgeNode bridge(mock_utxo, &bridge_forest);

    UtreexoForest csn_forest;
    StatelessNode csn(&csn_forest);

    auto script = makeScript();

    // Block 1: Create 4 UTXOs
    Block block1 = makeCoinbaseBlock(1, {
        {1000, script}, {2000, script}, {3000, script}, {4000, script}
    });

    UtreexoHash root_before_1 = bridge_forest.getCommitment();
    BlockUtreexoData proof_data_1 = bridge.GenerateProofForBlock(block1, 1);
    UtreexoTransitionProof tp1 = UtreexoTransitionProof::generate(
        bridge_forest, block1, proof_data_1.spend_proof);

    auto utxos_1 = extractUTXOsFromBlock(block1);
    for (const auto& u : utxos_1) {
        bridge_forest.add(u.leafHash);
        mock_utxo->AddTestUTXO(u.txid, u.vout, u.value, u.scriptPubKey, 1);
    }

    UtreexoHash root_after_1 = bridge_forest.getCommitment();
    TEST_ASSERT(tp1.verify(), "TP1 must verify");

    UtreexoProofMessage pm1;
    pm1.block_hash = block1.GetHash();
    pm1.block_height = 1;
    pm1.accumulator_root_before = root_before_1;
    pm1.accumulator_root_after = root_after_1;
    pm1.proof_data = proof_data_1;

    // Block 2: Spend UTXOs 0 and 2 from block 1, create 2 new UTXOs
    // Build a block with a spending tx + coinbase
    Block block2;
    std::memset(&block2.header, 0, sizeof(BlockHeader));
    block2.header.version = 1;
    block2.header.prev_block_hash = block1.GetHash();
    block2.header.timestamp = 1700001200;
    block2.header.difficulty = 0x1d00ffff;
    block2.header.nonce = 2;

    // Coinbase tx (must be first)
    Transaction coinbase2;
    coinbase2.version = 2;
    coinbase2.witness_version = 0xFF;
    TxInput cb_in2;
    cb_in2.prevout.vout = 0xFFFFFFFF;
    uint32_t h2 = 2;
    cb_in2.scriptSig.resize(4);
    std::memcpy(cb_in2.scriptSig.data(), &h2, 4);
    coinbase2.vin.push_back(cb_in2);
    coinbase2.vout.push_back(TxOutput(AmountUna::Una(5000), script));
    coinbase2.vout.push_back(TxOutput(AmountUna::Una(6000), script));
    block2.vtx.push_back(coinbase2);

    // Spending tx
    Transaction spend_tx;
    spend_tx.version = 2;
    spend_tx.witness_version = 0xFF;
    TxInput in0;
    in0.prevout.txid = utxos_1[0].txid;
    in0.prevout.vout = utxos_1[0].vout;
    spend_tx.vin.push_back(in0);
    TxInput in2;
    in2.prevout.txid = utxos_1[2].txid;
    in2.prevout.vout = utxos_1[2].vout;
    spend_tx.vin.push_back(in2);
    spend_tx.vout.push_back(TxOutput(AmountUna::Una(100), script));
    block2.vtx.push_back(spend_tx);

    // Set utreexo data on block2 for the spend proof
    // Generate proof for block 2 — this needs the spent UTXOs in the forest
    UtreexoHash root_before_2 = bridge_forest.getCommitment();
    BlockUtreexoData proof_data_2 = bridge.GenerateProofForBlock(block2, 2);
    UtreexoTransitionProof tp2 = UtreexoTransitionProof::generate(
        bridge_forest, block2, proof_data_2.spend_proof);

    TEST_ASSERT(tp2.deletion_targets.size() == 2,
                "TP2 must have 2 deletion targets");

    // Apply block 2 to bridge forest: remove spent, add new
    // Remove spent UTXOs
    for (const auto& target : proof_data_2.spend_proof.targets) {
        auto pos = bridge_forest.findLeafPosition(target);
        if (pos.has_value()) {
            auto proof = bridge_forest.prove(*pos);
            if (proof.has_value()) {
                bridge_forest.remove(target, *proof);
            }
        }
    }
    // Add new UTXOs
    auto utxos_2 = extractUTXOsFromBlock(block2);
    for (const auto& u : utxos_2) {
        bridge_forest.add(u.leafHash);
        mock_utxo->AddTestUTXO(u.txid, u.vout, u.value, u.scriptPubKey, 2);
    }

    UtreexoHash root_after_2 = bridge_forest.getCommitment();
    TEST_ASSERT(tp2.verify(), "TP2 must verify");
    TEST_ASSERT(tp2.commitment_after == root_after_2,
                "TP2 commitment_after must match bridge root");

    UtreexoProofMessage pm2;
    pm2.block_hash = block2.GetHash();
    pm2.block_height = 2;
    pm2.accumulator_root_before = root_before_2;
    pm2.accumulator_root_after = root_after_2;
    pm2.proof_data = proof_data_2;

    // CSN syncs both blocks
    bool ok1 = csn.ValidateWithTransitionProof(block1, pm1, tp1, 0);
    TEST_ASSERT(ok1, "CSN must accept block 1");

    bool ok2 = csn.ValidateWithTransitionProof(block2, pm2, tp2, 0);
    TEST_ASSERT(ok2, "CSN must accept block 2 (with spends)");

    TEST_ASSERT(csn.GetCurrentAccumulatorRoot() == bridge_forest.getCommitment(),
                "CSN root must match bridge after block 2");

    // TX relay: spend one of block 2's new UTXOs
    // utxos_2 contains outputs from coinbase + spending tx
    // Pick the spending tx output (last in utxos_2)
    const auto& spend_utxo = utxos_2.back();
    Transaction relay_tx;
    relay_tx.version = 2;
    relay_tx.witness_version = 0xFF;
    TxInput relay_in;
    relay_in.prevout.txid = spend_utxo.txid;
    relay_in.prevout.vout = spend_utxo.vout;
    relay_tx.vin.push_back(relay_in);
    relay_tx.vout.push_back(TxOutput(AmountUna::Una(0), {0x6a}));

    auto relay_proofs = bridge.GenerateProofsForTransaction(relay_tx);
    TEST_ASSERT(relay_proofs.has_value(), "Bridge must generate TX proofs");
    TEST_ASSERT(relay_proofs->size() == 1, "1 proof for 1 input");

    bool relay_valid = csn.ValidateUtreexoTx(
        relay_tx, *relay_proofs, bridge_forest.getCommitment());
    TEST_ASSERT(relay_valid, "CSN must accept TX spending block 2 output");

    std::cout << "  PASSED" << std::endl;
}

// ============================================================================
// Test 2b: Block 1 proof generation tolerates missing genesis checkpoint
// ============================================================================

static void test_block1_missing_genesis_checkpoint_fallback() {
    std::cout << "E2E Test: Block 1 proof generation without genesis checkpoint..." << std::endl;

    const auto temp_root = std::filesystem::temp_directory_path() /
                           std::filesystem::path("dinero_bridge_block1_no_ckpt");
    std::filesystem::remove_all(temp_root);
    std::filesystem::create_directories(temp_root);

    ChainDB chain_db;
    TEST_ASSERT(chain_db.init(temp_root) == Status::Ok, "ChainDB temp init must succeed");

    UtreexoForest bridge_forest;
    auto mock_utxo = std::make_shared<MockUTXOProvider>();
    BridgeNode bridge(mock_utxo, &bridge_forest, nullptr, &chain_db);

    const auto script = makeScript();
    Block block1 = makeCoinbaseBlock(1, {{1000, script}});

    bool threw = false;
    try {
        auto proof_data = bridge.GenerateProofForBlock(block1, 1);
        TEST_ASSERT(proof_data.spend_proof.targets.empty(),
                    "Block 1 coinbase-only proof should have no spend targets");
    } catch (const std::exception& e) {
        threw = true;
        std::cerr << "Unexpected exception: " << e.what() << std::endl;
    }

    TEST_ASSERT(!threw, "Bridge must not throw when height-0 checkpoint is absent");

    chain_db.close();
    std::filesystem::remove_all(temp_root);
    std::cout << "  PASSED" << std::endl;
}

// ============================================================================
// Test 3: Batch proof with intra-block spend
// ============================================================================

static void test_batch_proof_with_intra_block_spend() {
    std::cout << "E2E Test: Batch proof with intra-block spend..." << std::endl;

    UtreexoForest bridge_forest;
    auto mock_utxo = std::make_shared<MockUTXOProvider>();
    BridgeNode bridge(mock_utxo, &bridge_forest);

    UtreexoForest csn_forest;
    StatelessNode csn(&csn_forest);

    auto script = makeScript();

    // Block 1: seed one spendable UTXO into bridge state.
    Block block1 = makeCoinbaseBlock(1, {{5000, script}});
    auto utxos_1 = extractUTXOsFromBlock(block1);
    for (const auto& u : utxos_1) {
        bridge_forest.add(u.leafHash);
        mock_utxo->AddTestUTXO(u.txid, u.vout, u.value, u.scriptPubKey, 1);
    }

    // Seed the CSN directly from the canonical pre-block-2 forest so this
    // regression isolates batch-proof handling instead of the unrelated
    // transition-proof harness failures in older tests.
    csn.RewindToCheckpoint(1, bridge_forest);

    // Block 2:
    //   tx1 spends block1 output
    //   tx2 spends tx1 output 1 in the SAME block
    Block block2;
    std::memset(&block2.header, 0, sizeof(BlockHeader));
    block2.header.version = 1;
    block2.header.prev_block_hash = block1.GetHash();
    block2.header.timestamp = 1700001200;
    block2.header.difficulty = 0x1d00ffff;
    block2.header.nonce = 2;

    Transaction coinbase2;
    coinbase2.version = 2;
    coinbase2.witness_version = 0xFF;
    TxInput cb_in2;
    cb_in2.prevout.vout = 0xFFFFFFFF;
    uint32_t h2 = 2;
    cb_in2.scriptSig.resize(4);
    std::memcpy(cb_in2.scriptSig.data(), &h2, 4);
    coinbase2.vin.push_back(cb_in2);
    coinbase2.vout.push_back(TxOutput(AmountUna::Una(1000), script));
    block2.vtx.push_back(coinbase2);

    Transaction tx1;
    tx1.version = 2;
    tx1.witness_version = 0xFF;
    TxInput tx1_in;
    tx1_in.prevout.txid = utxos_1[0].txid;
    tx1_in.prevout.vout = utxos_1[0].vout;
    tx1.vin.push_back(tx1_in);
    tx1.vout.push_back(TxOutput(AmountUna::Una(4200), script));
    tx1.vout.push_back(TxOutput(AmountUna::Una(700), script));  // Ephemeral
    block2.vtx.push_back(tx1);

    Transaction tx2;
    tx2.version = 2;
    tx2.witness_version = 0xFF;
    TxInput tx2_in;
    tx2_in.prevout.txid = tx1.GetTxid();
    tx2_in.prevout.vout = 1;
    tx2.vin.push_back(tx2_in);
    tx2.vout.push_back(TxOutput(AmountUna::Una(650), script));
    block2.vtx.push_back(tx2);

    UtreexoHash root_before_2 = bridge_forest.getCommitment();
    BlockUtreexoData proof_data_2 = bridge.GenerateProofForBlock(block2, 2);

    TEST_ASSERT(proof_data_2.spent_outputs.size() == 2,
                "Spent-output metadata must cover both inputs");
    TEST_ASSERT(proof_data_2.spend_proof.targets.size() == 1,
                "Only pre-block spend should appear in the batch proof");

    for (const auto& target : proof_data_2.spend_proof.targets) {
        auto pos = bridge_forest.findLeafPosition(target);
        TEST_ASSERT(pos.has_value(), "Bridge target must exist in forest");
        auto proof = bridge_forest.prove(*pos);
        TEST_ASSERT(proof.has_value(), "Bridge must prove non-ephemeral target");
        TEST_ASSERT(bridge_forest.remove(target, *proof), "Bridge must remove spend target");
    }
    for (const auto& addition : UtreexoTransitionProof::computeAdditionHashes(block2)) {
        TEST_ASSERT(bridge_forest.add(addition) != UINT64_MAX,
                    "Bridge must add canonical non-ephemeral outputs");
    }

    UtreexoProofMessage pm2;
    pm2.block_hash = block2.GetHash();
    pm2.block_height = 2;
    pm2.accumulator_root_before = root_before_2;
    pm2.accumulator_root_after = bridge_forest.getCommitment();
    pm2.proof_data = proof_data_2;

    TEST_ASSERT(pm2.accumulator_root_after == bridge_forest.getCommitment(),
                "Bridge root_after must match manual intra-block-spend application");
    TEST_ASSERT(csn.ValidateUtreexoProof(block2, pm2, 0),
                "CSN must accept batch proof with intra-block spend");

    std::cout << "  PASSED" << std::endl;
}

// ============================================================================
// Test 3: CSN-to-CSN TX relay (#3)
// ============================================================================

static void test_csn_to_csn_tx_relay() {
    std::cout << "E2E Test: CSN-to-CSN TX relay (cached proof forwarding)..." << std::endl;

    // ========================================================================
    // Setup: Bridge + CSN1 + CSN2, all start at genesis
    // ========================================================================
    UtreexoForest bridge_forest;
    auto mock_utxo = std::make_shared<MockUTXOProvider>();
    BridgeNode bridge(mock_utxo, &bridge_forest);

    UtreexoForest csn1_forest;
    StatelessNode csn1(&csn1_forest);

    UtreexoForest csn2_forest;
    StatelessNode csn2(&csn2_forest);

    auto script = makeScript();

    // ========================================================================
    // Phase A: Build 3 blocks and sync all nodes
    // ========================================================================
    std::cout << "  Phase A: Sync 3 blocks to Bridge + CSN1 + CSN2..." << std::endl;

    struct BlockInfo {
        Block block;
        UtreexoTransitionProof tp;
        UtreexoProofMessage proof_msg;
        std::vector<TrackedUTXO> utxos;
    };
    std::vector<BlockInfo> block_infos;

    uint256 prev_hash;
    for (int b = 1; b <= 3; b++) {
        Block block = makeCoinbaseBlock(b,
            {{static_cast<uint64_t>(1000 * b), script},
             {static_cast<uint64_t>(1000 * b + 500), script}}, prev_hash);

        UtreexoHash root_before = bridge_forest.getCommitment();
        BlockUtreexoData proof_data = bridge.GenerateProofForBlock(block, b);
        UtreexoTransitionProof tp = UtreexoTransitionProof::generate(
            bridge_forest, block, proof_data.spend_proof);

        auto utxos = extractUTXOsFromBlock(block);
        for (const auto& u : utxos) {
            bridge_forest.add(u.leafHash);
            mock_utxo->AddTestUTXO(u.txid, u.vout, u.value, u.scriptPubKey, b);
        }

        UtreexoHash root_after = bridge_forest.getCommitment();
        TEST_ASSERT(tp.verify(), "TP must verify for block " + std::to_string(b));

        UtreexoProofMessage pm;
        pm.block_hash = block.GetHash();
        pm.block_height = b;
        pm.accumulator_root_before = root_before;
        pm.accumulator_root_after = root_after;
        pm.proof_data = proof_data;

        block_infos.push_back({block, tp, pm, utxos});
        prev_hash = block.GetHash();
    }

    // Sync CSN1 and CSN2
    for (int i = 0; i < 3; i++) {
        const auto& bi = block_infos[i];
        bool ok1 = csn1.ValidateWithTransitionProof(bi.block, bi.proof_msg, bi.tp, 0);
        TEST_ASSERT(ok1, "CSN1 must accept block " + std::to_string(i + 1));
        bool ok2 = csn2.ValidateWithTransitionProof(bi.block, bi.proof_msg, bi.tp, 0);
        TEST_ASSERT(ok2, "CSN2 must accept block " + std::to_string(i + 1));
    }

    UtreexoHash sync_root = bridge_forest.getCommitment();
    TEST_ASSERT(csn1.GetCurrentAccumulatorRoot() == sync_root, "CSN1 root matches bridge");
    TEST_ASSERT(csn2.GetCurrentAccumulatorRoot() == sync_root, "CSN2 root matches bridge");

    std::cout << "    All 3 nodes synced, roots match" << std::endl;

    // ========================================================================
    // Phase B: Bridge→CSN1 TX relay
    // ========================================================================
    std::cout << "  Phase B: Bridge→CSN1 TX relay..." << std::endl;

    // Spend UTXO from block 2 (output 0)
    const auto& b2_utxos = block_infos[1].utxos;
    Transaction spending_tx;
    spending_tx.version = 2;
    spending_tx.witness_version = 0xFF;
    TxInput inp;
    inp.prevout.txid = b2_utxos[0].txid;
    inp.prevout.vout = b2_utxos[0].vout;
    spending_tx.vin.push_back(inp);
    spending_tx.vout.push_back(TxOutput(AmountUna::Una(0), {0x6a}));

    // Bridge generates per-input proofs
    auto bridge_proofs = bridge.GenerateProofsForTransaction(spending_tx);
    TEST_ASSERT(bridge_proofs.has_value(), "Bridge must generate TX proofs");
    TEST_ASSERT(bridge_proofs->size() == 1, "1 proof for 1 input");

    // CSN1 validates
    bool csn1_valid = csn1.ValidateUtreexoTx(spending_tx, *bridge_proofs, sync_root);
    TEST_ASSERT(csn1_valid, "CSN1 must accept TX with valid proofs");

    std::cout << "    CSN1 accepted TX from bridge" << std::endl;

    // ========================================================================
    // Phase C: CSN1→CSN2 relay (same proofs, simulating cached payload)
    // ========================================================================
    std::cout << "  Phase C: CSN1→CSN2 relay (cached proofs)..." << std::endl;

    // CSN2 validates using the SAME proofs that CSN1 received
    // This simulates the cached utxotx payload being forwarded
    bool csn2_valid = csn2.ValidateUtreexoTx(spending_tx, *bridge_proofs, sync_root);
    TEST_ASSERT(csn2_valid, "CSN2 must accept TX with same proofs (CSN-to-CSN relay)");

    std::cout << "    CSN2 accepted TX relayed from CSN1" << std::endl;

    // ========================================================================
    // Phase D: Stale proof rejection after new block
    // ========================================================================
    std::cout << "  Phase D: Stale proof rejection..." << std::endl;

    // Build block 4, advance only CSN2
    Block block4 = makeCoinbaseBlock(4,
        {{7000, script}, {7500, script}}, prev_hash);

    UtreexoHash root_before_4 = bridge_forest.getCommitment();
    BlockUtreexoData proof_data_4 = bridge.GenerateProofForBlock(block4, 4);
    UtreexoTransitionProof tp4 = UtreexoTransitionProof::generate(
        bridge_forest, block4, proof_data_4.spend_proof);

    auto utxos_4 = extractUTXOsFromBlock(block4);
    for (const auto& u : utxos_4) {
        bridge_forest.add(u.leafHash);
        mock_utxo->AddTestUTXO(u.txid, u.vout, u.value, u.scriptPubKey, 4);
    }
    UtreexoHash root_after_4 = bridge_forest.getCommitment();
    TEST_ASSERT(tp4.verify(), "TP4 must verify");

    UtreexoProofMessage pm4;
    pm4.block_hash = block4.GetHash();
    pm4.block_height = 4;
    pm4.accumulator_root_before = root_before_4;
    pm4.accumulator_root_after = root_after_4;
    pm4.proof_data = proof_data_4;

    bool ok4 = csn2.ValidateWithTransitionProof(block4, pm4, tp4, 0);
    TEST_ASSERT(ok4, "CSN2 must accept block 4");

    UtreexoHash new_root = csn2.GetCurrentAccumulatorRoot();
    TEST_ASSERT(new_root != sync_root, "CSN2 root must differ after block 4");

    // Old proofs against new root must fail
    bool stale_rejected = !csn2.ValidateUtreexoTx(spending_tx, *bridge_proofs, new_root);
    TEST_ASSERT(stale_rejected, "CSN2 must reject stale proofs against new root");

    // Old proofs with old root should also be rejected (root mismatch with CSN2's state)
    bool old_root_rejected = !csn2.ValidateUtreexoTx(spending_tx, *bridge_proofs, sync_root);
    TEST_ASSERT(old_root_rejected, "CSN2 must reject proofs with stale accumulator root");

    std::cout << "    Stale proofs correctly rejected" << std::endl;
    std::cout << "  PASSED" << std::endl;
}

// ============================================================================
// Main
// ============================================================================

// ============================================================================
// #583 regression: getheaders from a peer stranded on a DEAD branch must serve
// the fork block, not an empty / fork-skipping reply.
// ============================================================================
static void test_getheaders_dead_branch_serves_fork_block() {
    std::cout << "E2E Test: #583 getheaders dead-branch locator must serve the fork block..."
              << std::endl;

    const auto temp_root = std::filesystem::temp_directory_path() /
                           std::filesystem::path("dinero_bridge_583_getheaders");
    std::filesystem::remove_all(temp_root);
    std::filesystem::create_directories(temp_root);

    ChainDB chain_db;
    TEST_ASSERT(chain_db.init(temp_root) == Status::Ok, "ChainDB temp init must succeed");

    // Reorg geometry: fork at height 83. The requester's tip is the DEAD block at
    // 83; the ACTIVE chain has a DIFFERENT block at 83 and extends to 85. Height 82
    // is the shared common ancestor.
    auto make_header = [](const uint256& prev, uint32_t nonce) {
        BlockHeader h;
        std::memset(&h, 0, sizeof(BlockHeader));
        h.version = 1;
        h.prev_block_hash = prev;
        h.timestamp = 1700000000 + nonce;
        h.difficulty = 0x1d00ffff;
        h.nonce = nonce;
        return h;
    };
    BlockHeader h82    = make_header(uint256(), 82);
    BlockHeader hdead  = make_header(h82.GetHash(), 8300);    // dead-83 (prev = 82)
    BlockHeader hnew83 = make_header(h82.GetHash(), 8301);    // new-83  (prev = 82, distinct)
    BlockHeader hnew84 = make_header(hnew83.GetHash(), 8400);
    BlockHeader hnew85 = make_header(hnew84.GetHash(), 8500);

    // getBlockHeight resolves BOTH 83s (both putHeader'd — the bridge indexes the
    // competing branch), but the canonical height index points to the ACTIVE chain.
    auto token = ChainWriteToken::CreateForTesting();
    arith_uint256 w;  // work value is not consulted by FindCommonAncestor
    TEST_ASSERT(chain_db.putHeader(token, h82.GetHash(),    h82,    82, w) == Status::Ok, "putHeader 82");
    TEST_ASSERT(chain_db.putHeader(token, hdead.GetHash(),  hdead,  83, w) == Status::Ok, "putHeader dead-83");
    TEST_ASSERT(chain_db.putHeader(token, hnew83.GetHash(), hnew83, 83, w) == Status::Ok, "putHeader new-83");
    TEST_ASSERT(chain_db.putHeightIndex(token, 82, h82.GetHash())    == Status::Ok, "heightIndex 82");
    TEST_ASSERT(chain_db.putHeightIndex(token, 83, hnew83.GetHash()) == Status::Ok, "heightIndex new-83");
    TEST_ASSERT(chain_db.putHeightIndex(token, 84, hnew84.GetHash()) == Status::Ok, "heightIndex new-84");
    TEST_ASSERT(chain_db.putHeightIndex(token, 85, hnew85.GetHash()) == Status::Ok, "heightIndex new-85");

    UtreexoForest bridge_forest;
    auto mock_utxo = std::make_shared<MockUTXOProvider>();
    BridgeNode bridge(mock_utxo, &bridge_forest, nullptr, &chain_db);

    // Stranded-CSN locator: densest entry is its dead tip; next is the shared 82.
    GetUtreexoHeadersMessage request;
    request.locator_hashes = { hdead.GetHash(), h82.GetHash() };

    // Serve the ACTIVE chain forward from start_height + 1.
    std::unordered_map<uint32_t, BlockHeader> active_by_height =
        { {83, hnew83}, {84, hnew84}, {85, hnew85} };
    auto header_by_height = [&](uint32_t height) -> std::optional<BlockHeader> {
        auto it = active_by_height.find(height);
        if (it == active_by_height.end()) return std::nullopt;
        return it->second;
    };
    auto header_by_hash = [&](const uint256&) -> std::optional<BlockHeader> {
        return std::nullopt;  // not consulted by the common-ancestor path under test
    };

    UtreexoHeadersMessage response =
        bridge.HandleHeadersRequest(request, header_by_hash, header_by_height);

    // Fixed: FindCommonAncestor skips the non-canonical dead-83 and lands on the
    // shared 82, so serving starts at 83 and INCLUDES the fork block new-83.
    // Pre-fix: it accepts dead-83's height (83) and serves from 84 — the fork block
    // is skipped and the CSN can never switch branches (the silent freeze).
    TEST_ASSERT(!response.headers.empty(),
                "#583: dead-branch getheaders must not return an empty reply");
    bool has_fork_block = false;
    for (const auto& hdr : response.headers) {
        if (hdr.GetHash() == hnew83.GetHash()) { has_fork_block = true; break; }
    }
    TEST_ASSERT(has_fork_block,
                "#583: reply must contain the fork block (new-83), not skip past it");

    chain_db.close();
    std::filesystem::remove_all(temp_root);
    std::cout << "  PASSED" << std::endl;
}

int main() {
    SelectParams(Chain::REGTEST);

    std::cout << std::endl;
    std::cout << "═══════════════════════════════════════════════════" << std::endl;
    std::cout << "  E2E Integration: Bridge→CSN Block Sync + TX Relay" << std::endl;
    std::cout << "═══════════════════════════════════════════════════" << std::endl;

    test_e2e_block_sync_then_tx_relay();
    test_e2e_block_with_spends();
    test_block1_missing_genesis_checkpoint_fallback();
    test_batch_proof_with_intra_block_spend();
    test_csn_to_csn_tx_relay();
    test_getheaders_dead_branch_serves_fork_block();

    std::cout << std::endl;
    std::cout << "═══════════════════════════════════════════════════" << std::endl;
    std::cout << "  All " << tests_passed << "/" << tests_total
              << " assertions passed!" << std::endl;
    std::cout << "═══════════════════════════════════════════════════" << std::endl;

    return tests_passed == tests_total ? 0 : 1;
}
