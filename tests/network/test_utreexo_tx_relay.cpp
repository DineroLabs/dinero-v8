/**
 * Phase #4: Utreexo TX Relay Test Suite
 *
 * Tests:
 * 1. GenerateProofsForTransaction — bridge generates per-input proofs
 * 2. ValidateUtreexoTx with valid proofs — CSN accepts
 * 3. ValidateUtreexoTx with stale root — CSN rejects
 * 4. ValidateUtreexoTx with corrupt proof — CSN rejects
 * 5. ValidateUtreexoTx proof count mismatch — CSN rejects
 * 6. utxotx wire format round-trip — serialize + deserialize
 * 7. Coinbase inputs skipped — coinbase inputs don't require proofs
 */

#include "consensus/utreexo_accumulator.h"
#include "consensus/utreexo_maturity_leaf_activation.h"
#include "consensus/interfaces/iutxo_provider.h"
#include "network/bridge_node.h"
#include "network/stateless_node.h"
#include "primitives/transaction.h"
#include "primitives/hash_domains.h"
#include "consensus/chainparams.h"
#include <iostream>
#include <cassert>
#include <vector>
#include <cstring>
#include <unordered_map>

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
                     uint32_t height = 1,
                     bool is_coinbase = false) {
        OutPoint op(txid, vout);
        UTXOEntry entry(AmountUna::Una(value), script, height, is_coinbase);
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

static uint256 makeTxId(uint64_t id) {
    uint256 txid;
    std::memset(txid.data, 0, 32);
    std::memcpy(txid.data, &id, sizeof(id));
    return txid;
}

struct TestUTXO {
    TxId txid;
    uint32_t vout;
    uint64_t value;
    std::vector<uint8_t> scriptPubKey;
    UtreexoHash leafHash;
    uint64_t forest_position;
};

static TestUTXO makeTestUTXO(uint64_t id, uint32_t vout, uint64_t value) {
    TestUTXO u;
    u.txid = TxId(makeTxId(id));
    u.vout = vout;
    u.value = value;
    u.scriptPubKey = {0x51, 0x20};  // P2TR prefix
    u.scriptPubKey.resize(34, 0x00);
    u.leafHash = HashUTXOLegacy(u.txid.AsUint256(), u.vout, u.value, u.scriptPubKey);
    u.forest_position = 0;
    return u;
}

// Build a spending transaction that references the given UTXOs as inputs
static Transaction buildSpendingTx(const std::vector<TestUTXO>& inputs) {
    Transaction tx;
    tx.version = 2;
    tx.lockTime = 0;
    for (const auto& utxo : inputs) {
        TxInput inp;
        inp.prevout.txid = utxo.txid;
        inp.prevout.vout = utxo.vout;
        tx.vin.push_back(inp);
    }
    // Single dummy output
    TxOutput out;
    out.value = AmountUna::Una(0);
    out.scriptPubKey = {0x6a};  // OP_RETURN
    tx.vout.push_back(out);
    return tx;
}

// ============================================================================
// Test 1: GenerateProofsForTransaction
// ============================================================================

static void test_generate_proofs_for_tx() {
    std::cout << "Test 1: GenerateProofsForTransaction..." << std::endl;

    // Setup forest + mock UTXO provider
    UtreexoForest forest;
    auto mock_utxo = std::make_shared<MockUTXOProvider>();

    // Create 5 UTXOs, add to forest and mock provider
    std::vector<TestUTXO> utxos;
    for (uint64_t i = 1; i <= 5; i++) {
        auto u = makeTestUTXO(i, 0, 1000 * i);
        u.forest_position = forest.add(u.leafHash);
        mock_utxo->AddTestUTXO(u.txid, u.vout, u.value, u.scriptPubKey);
        utxos.push_back(u);
    }

    BridgeNode bridge(mock_utxo, &forest);

    // Build a TX that spends UTXOs 0 and 2
    Transaction tx = buildSpendingTx({utxos[0], utxos[2]});

    auto result = bridge.GenerateProofsForTransaction(tx);
    TEST_ASSERT(result.has_value(), "GenerateProofsForTransaction should succeed");
    TEST_ASSERT(result->size() == 2, "Should generate 2 proofs for 2 inputs");

    // Verify each proof individually
    auto roots = forest.getRoots();
    for (size_t i = 0; i < result->size(); i++) {
        const auto& [proof, spent] = (*result)[i];
        const auto& utxo = (i == 0) ? utxos[0] : utxos[2];

        TEST_ASSERT(spent.value == utxo.value,
                    "SpentOutputData value should match UTXO");
        TEST_ASSERT(spent.scriptPubKey == utxo.scriptPubKey,
                    "SpentOutputData script should match UTXO");

        UtreexoHash leaf = HashUTXOLegacy(utxo.txid.AsUint256(), utxo.vout,
                                     utxo.value, utxo.scriptPubKey);
        bool valid = proof.verify(leaf, roots);
        TEST_ASSERT(valid, "Individual proof should verify against forest roots");
    }

    std::cout << "  PASSED" << std::endl;
}

// ============================================================================
// Test 2: ValidateUtreexoTx with Valid Proofs
// ============================================================================

static void test_validate_utreexo_tx_valid() {
    std::cout << "Test 2: ValidateUtreexoTx with valid proofs..." << std::endl;

    UtreexoForest forest;
    auto mock_utxo = std::make_shared<MockUTXOProvider>();

    std::vector<TestUTXO> utxos;
    for (uint64_t i = 1; i <= 4; i++) {
        auto u = makeTestUTXO(i, 0, 2000 * i);
        u.forest_position = forest.add(u.leafHash);
        mock_utxo->AddTestUTXO(u.txid, u.vout, u.value, u.scriptPubKey);
        utxos.push_back(u);
    }

    BridgeNode bridge(mock_utxo, &forest);
    StatelessNode csn(&forest);

    // Build TX spending UTXOs 1 and 3
    Transaction tx = buildSpendingTx({utxos[1], utxos[3]});

    auto proofs = bridge.GenerateProofsForTransaction(tx);
    TEST_ASSERT(proofs.has_value(), "Proof generation should succeed");

    // CSN validates: accumulator_root must match local commitment
    auto root = forest.getCommitment();
    bool valid = csn.ValidateUtreexoTx(tx, *proofs, root);
    TEST_ASSERT(valid, "CSN should accept tx with valid proofs and matching root");

    std::cout << "  PASSED" << std::endl;
}

// ============================================================================
// Test 3: ValidateUtreexoTx with Stale Root
// ============================================================================

static void test_validate_utreexo_tx_stale_root() {
    std::cout << "Test 3: ValidateUtreexoTx with stale root..." << std::endl;

    UtreexoForest forest;
    auto mock_utxo = std::make_shared<MockUTXOProvider>();

    std::vector<TestUTXO> utxos;
    for (uint64_t i = 1; i <= 3; i++) {
        auto u = makeTestUTXO(i, 0, 5000);
        u.forest_position = forest.add(u.leafHash);
        mock_utxo->AddTestUTXO(u.txid, u.vout, u.value, u.scriptPubKey);
        utxos.push_back(u);
    }

    BridgeNode bridge(mock_utxo, &forest);

    // Generate proofs while root is at 3 leaves
    Transaction tx = buildSpendingTx({utxos[0]});
    auto proofs = bridge.GenerateProofsForTransaction(tx);
    TEST_ASSERT(proofs.has_value(), "Proof generation should succeed");

    auto old_root = forest.getCommitment();

    // Advance the forest (add a new UTXO) → root changes
    auto extra = makeTestUTXO(100, 0, 9999);
    forest.add(extra.leafHash);

    // CSN constructed on the NEW forest state
    StatelessNode csn(&forest);

    // Try to validate with OLD root → should fail freshness check
    bool valid = csn.ValidateUtreexoTx(tx, *proofs, old_root);
    TEST_ASSERT(!valid, "CSN should reject tx with stale accumulator root");

    std::cout << "  PASSED" << std::endl;
}

// ============================================================================
// Test 4: ValidateUtreexoTx with Corrupt Proof
// ============================================================================

static void test_validate_utreexo_tx_corrupt_proof() {
    std::cout << "Test 4: ValidateUtreexoTx with corrupt proof..." << std::endl;

    UtreexoForest forest;
    auto mock_utxo = std::make_shared<MockUTXOProvider>();

    std::vector<TestUTXO> utxos;
    for (uint64_t i = 1; i <= 4; i++) {
        auto u = makeTestUTXO(i, 0, 3000);
        u.forest_position = forest.add(u.leafHash);
        mock_utxo->AddTestUTXO(u.txid, u.vout, u.value, u.scriptPubKey);
        utxos.push_back(u);
    }

    BridgeNode bridge(mock_utxo, &forest);
    StatelessNode csn(&forest);

    Transaction tx = buildSpendingTx({utxos[0], utxos[2]});
    auto proofs = bridge.GenerateProofsForTransaction(tx);
    TEST_ASSERT(proofs.has_value(), "Proof generation should succeed");

    auto root = forest.getCommitment();

    // Tamper with the first proof's sibling hash
    auto tampered = *proofs;
    if (!tampered[0].first.siblings.empty()) {
        tampered[0].first.siblings[0][0] ^= 0xFF;
    }

    bool valid = csn.ValidateUtreexoTx(tx, tampered, root);
    TEST_ASSERT(!valid, "CSN should reject tx with tampered proof sibling");

    std::cout << "  PASSED" << std::endl;
}

// ============================================================================
// Test 5: ValidateUtreexoTx Proof Count Mismatch
// ============================================================================

static void test_validate_utreexo_tx_proof_count_mismatch() {
    std::cout << "Test 5: ValidateUtreexoTx proof count mismatch..." << std::endl;

    UtreexoForest forest;
    auto mock_utxo = std::make_shared<MockUTXOProvider>();

    std::vector<TestUTXO> utxos;
    for (uint64_t i = 1; i <= 4; i++) {
        auto u = makeTestUTXO(i, 0, 4000);
        u.forest_position = forest.add(u.leafHash);
        mock_utxo->AddTestUTXO(u.txid, u.vout, u.value, u.scriptPubKey);
        utxos.push_back(u);
    }

    BridgeNode bridge(mock_utxo, &forest);
    StatelessNode csn(&forest);

    // TX with 2 inputs
    Transaction tx = buildSpendingTx({utxos[0], utxos[1]});
    auto proofs = bridge.GenerateProofsForTransaction(tx);
    TEST_ASSERT(proofs.has_value(), "Proof generation should succeed");

    auto root = forest.getCommitment();

    // Remove one proof → count mismatch
    auto short_proofs = *proofs;
    short_proofs.pop_back();
    TEST_ASSERT(short_proofs.size() == 1, "Should have 1 proof after pop");

    bool valid = csn.ValidateUtreexoTx(tx, short_proofs, root);
    TEST_ASSERT(!valid, "CSN should reject tx with wrong number of proofs");

    // Add extra proof → count mismatch the other way
    auto long_proofs = *proofs;
    long_proofs.push_back((*proofs)[0]);  // Duplicate first proof
    TEST_ASSERT(long_proofs.size() == 3, "Should have 3 proofs");

    valid = csn.ValidateUtreexoTx(tx, long_proofs, root);
    TEST_ASSERT(!valid, "CSN should reject tx with too many proofs");

    std::cout << "  PASSED" << std::endl;
}

// ============================================================================
// Test 6: utxotx Wire Format Round-Trip
// ============================================================================

static void test_utxotx_wire_format_roundtrip() {
    std::cout << "Test 6: utxotx proof wire format round-trip..." << std::endl;

    UtreexoForest forest;
    auto mock_utxo = std::make_shared<MockUTXOProvider>();

    std::vector<TestUTXO> utxos;
    for (uint64_t i = 1; i <= 3; i++) {
        auto u = makeTestUTXO(i, 0, 7000 * i);
        u.forest_position = forest.add(u.leafHash);
        mock_utxo->AddTestUTXO(u.txid, u.vout, u.value, u.scriptPubKey);
        utxos.push_back(u);
    }

    BridgeNode bridge(mock_utxo, &forest);

    Transaction tx = buildSpendingTx({utxos[0], utxos[1]});
    auto proofs = bridge.GenerateProofsForTransaction(tx);
    TEST_ASSERT(proofs.has_value(), "Proof generation should succeed");

    auto root = forest.getCommitment();

    // Serialize the proof payload portion of utxotx wire format
    // (proof_size + proof_bytes + value + script_size + script) per input
    // + accumulator_root at end
    std::vector<uint8_t> wire;

    uint32_t num_proofs = static_cast<uint32_t>(proofs->size());
    for (int i = 0; i < 4; i++) wire.push_back((num_proofs >> (i * 8)) & 0xFF);

    for (const auto& [proof, spent] : *proofs) {
        auto proof_bytes = proof.serialize();
        uint32_t proof_size = static_cast<uint32_t>(proof_bytes.size());
        for (int i = 0; i < 4; i++) wire.push_back((proof_size >> (i * 8)) & 0xFF);
        wire.insert(wire.end(), proof_bytes.begin(), proof_bytes.end());

        uint64_t val = spent.value;
        for (int i = 0; i < 8; i++) wire.push_back((val >> (i * 8)) & 0xFF);

        uint32_t script_size = static_cast<uint32_t>(spent.scriptPubKey.size());
        for (int i = 0; i < 4; i++) wire.push_back((script_size >> (i * 8)) & 0xFF);
        wire.insert(wire.end(), spent.scriptPubKey.begin(), spent.scriptPubKey.end());
    }

    wire.insert(wire.end(), root.begin(), root.end());

    TEST_ASSERT(wire.size() > 0, "Wire format should be non-empty");

    // Deserialize
    size_t pos = 0;

    uint32_t parsed_num_proofs = 0;
    for (int i = 0; i < 4; i++) parsed_num_proofs |= (uint32_t)wire[pos + i] << (i * 8);
    pos += 4;
    TEST_ASSERT(parsed_num_proofs == num_proofs, "Parsed num_proofs should match");

    std::vector<std::pair<UtreexoProof, SpentOutputData>> parsed_proofs;
    for (uint32_t p = 0; p < parsed_num_proofs; p++) {
        uint32_t proof_sz = 0;
        for (int i = 0; i < 4; i++) proof_sz |= (uint32_t)wire[pos + i] << (i * 8);
        pos += 4;

        std::vector<uint8_t> proof_bytes(wire.begin() + pos, wire.begin() + pos + proof_sz);
        UtreexoProof proof = UtreexoProof::deserialize(proof_bytes);
        pos += proof_sz;

        uint64_t value = 0;
        for (int i = 0; i < 8; i++) value |= (uint64_t)wire[pos + i] << (i * 8);
        pos += 8;

        uint32_t script_sz = 0;
        for (int i = 0; i < 4; i++) script_sz |= (uint32_t)wire[pos + i] << (i * 8);
        pos += 4;

        std::vector<uint8_t> script(wire.begin() + pos, wire.begin() + pos + script_sz);
        pos += script_sz;

        parsed_proofs.push_back({proof, SpentOutputData(value, script)});
    }

    UtreexoHash parsed_root(32);
    std::memcpy(parsed_root.data(), wire.data() + pos, 32);
    pos += 32;
    TEST_ASSERT(parsed_root == root, "Parsed accumulator root should match");
    TEST_ASSERT(pos == wire.size(), "Should consume entire wire payload");

    // Verify deserialized proofs match originals
    for (size_t i = 0; i < proofs->size(); i++) {
        TEST_ASSERT(parsed_proofs[i].second.value == (*proofs)[i].second.value,
                    "Parsed SpentOutputData value should match original");
        TEST_ASSERT(parsed_proofs[i].second.scriptPubKey == (*proofs)[i].second.scriptPubKey,
                    "Parsed SpentOutputData script should match original");
    }

    // Verify deserialized proofs still validate against the forest
    StatelessNode csn(&forest);
    bool valid = csn.ValidateUtreexoTx(tx, parsed_proofs, parsed_root);
    TEST_ASSERT(valid, "CSN should accept tx with deserialized proofs");

    std::cout << "  PASSED" << std::endl;
}

// ============================================================================
// Test 7: Coinbase Inputs Skipped
// ============================================================================

static void test_coinbase_inputs_skipped() {
    std::cout << "Test 7: Coinbase inputs skipped..." << std::endl;

    UtreexoForest forest;
    auto mock_utxo = std::make_shared<MockUTXOProvider>();

    auto u = makeTestUTXO(42, 0, 50000);
    u.forest_position = forest.add(u.leafHash);
    mock_utxo->AddTestUTXO(u.txid, u.vout, u.value, u.scriptPubKey);

    BridgeNode bridge(mock_utxo, &forest);

    // Build TX with coinbase input + regular input
    Transaction tx;
    tx.version = 2;

    // Coinbase input (null txid, vout=0xFFFFFFFF)
    TxInput coinbase_in;
    // TxId default-constructed is all zeros (null)
    coinbase_in.prevout.vout = 0xFFFFFFFF;
    tx.vin.push_back(coinbase_in);

    // Regular input spending our UTXO
    TxInput regular_in;
    regular_in.prevout.txid = u.txid;
    regular_in.prevout.vout = u.vout;
    tx.vin.push_back(regular_in);

    TxOutput out;
    out.value = AmountUna::Una(0);
    out.scriptPubKey = {0x6a};
    tx.vout.push_back(out);

    auto proofs = bridge.GenerateProofsForTransaction(tx);
    TEST_ASSERT(proofs.has_value(), "Should generate proofs for mixed coinbase+regular tx");
    TEST_ASSERT(proofs->size() == 1, "Should have 1 proof (coinbase input skipped)");

    // Validate on CSN
    StatelessNode csn(&forest);
    auto root = forest.getCommitment();
    bool valid = csn.ValidateUtreexoTx(tx, *proofs, root);
    TEST_ASSERT(valid, "CSN should accept tx with coinbase + regular inputs");

    std::cout << "  PASSED" << std::endl;
}

// ============================================================================
// Test 8: GenerateProofsForTransaction with Missing UTXO
// ============================================================================

static void test_generate_proofs_missing_utxo() {
    std::cout << "Test 8: GenerateProofsForTransaction with missing UTXO..." << std::endl;

    UtreexoForest forest;
    auto mock_utxo = std::make_shared<MockUTXOProvider>();

    // Add UTXO to forest but NOT to mock provider
    auto u = makeTestUTXO(99, 0, 1000);
    forest.add(u.leafHash);

    BridgeNode bridge(mock_utxo, &forest);

    Transaction tx = buildSpendingTx({u});
    auto result = bridge.GenerateProofsForTransaction(tx);
    TEST_ASSERT(!result.has_value(), "Should return nullopt when UTXO not in provider");

    std::cout << "  PASSED" << std::endl;
}

// ============================================================================
// Test 9: Multiple UTXOs from Same Transaction
// ============================================================================

static void test_multiple_outputs_same_tx() {
    std::cout << "Test 9: Multiple UTXOs from same transaction..." << std::endl;

    UtreexoForest forest;
    auto mock_utxo = std::make_shared<MockUTXOProvider>();

    // Create 3 outputs from the same txid but different vout
    TxId common_txid(makeTxId(777));
    std::vector<TestUTXO> utxos;
    for (uint32_t vout = 0; vout < 3; vout++) {
        TestUTXO u;
        u.txid = common_txid;
        u.vout = vout;
        u.value = 1000 * (vout + 1);
        u.scriptPubKey = {0x51, 0x20};
        u.scriptPubKey.resize(34, 0x00);
        u.leafHash = HashUTXOLegacy(u.txid.AsUint256(), u.vout, u.value, u.scriptPubKey);
        u.forest_position = forest.add(u.leafHash);
        mock_utxo->AddTestUTXO(u.txid, u.vout, u.value, u.scriptPubKey);
        utxos.push_back(u);
    }

    BridgeNode bridge(mock_utxo, &forest);
    StatelessNode csn(&forest);

    // Spend all 3 outputs
    Transaction tx = buildSpendingTx(utxos);
    auto proofs = bridge.GenerateProofsForTransaction(tx);
    TEST_ASSERT(proofs.has_value(), "Should generate proofs for multi-output spend");
    TEST_ASSERT(proofs->size() == 3, "Should have 3 proofs");

    auto root = forest.getCommitment();
    bool valid = csn.ValidateUtreexoTx(tx, *proofs, root);
    TEST_ASSERT(valid, "CSN should accept multi-output same-tx spend");

    std::cout << "  PASSED" << std::endl;
}

// ============================================================================
// Test 10: Proof Serialization Round-Trip
// ============================================================================

static void test_proof_serialization_roundtrip() {
    std::cout << "Test 10: Proof serialization round-trip..." << std::endl;

    UtreexoForest forest;
    auto mock_utxo = std::make_shared<MockUTXOProvider>();

    // Add several UTXOs to build a deeper tree
    std::vector<TestUTXO> utxos;
    for (uint64_t i = 1; i <= 8; i++) {
        auto u = makeTestUTXO(i, 0, 1000 * i);
        u.forest_position = forest.add(u.leafHash);
        mock_utxo->AddTestUTXO(u.txid, u.vout, u.value, u.scriptPubKey);
        utxos.push_back(u);
    }

    BridgeNode bridge(mock_utxo, &forest);

    Transaction tx = buildSpendingTx({utxos[3]});  // Spend 4th UTXO
    auto proofs = bridge.GenerateProofsForTransaction(tx);
    TEST_ASSERT(proofs.has_value(), "Proof generation should succeed");
    TEST_ASSERT(proofs->size() == 1, "Should have 1 proof");

    const auto& [orig_proof, orig_spent] = (*proofs)[0];

    // Serialize and deserialize the proof
    auto proof_bytes = orig_proof.serialize();
    TEST_ASSERT(!proof_bytes.empty(), "Serialized proof should be non-empty");

    UtreexoProof deserialized = UtreexoProof::deserialize(proof_bytes);

    // Verify round-trip preserved all fields
    TEST_ASSERT(deserialized.siblings.size() == orig_proof.siblings.size(),
                "Sibling count should match after round-trip");
    TEST_ASSERT(deserialized.position == orig_proof.position,
                "Position should match after round-trip");
    TEST_ASSERT(deserialized.numLeaves == orig_proof.numLeaves,
                "numLeaves should match after round-trip");

    for (size_t i = 0; i < orig_proof.siblings.size(); i++) {
        TEST_ASSERT(deserialized.siblings[i] == orig_proof.siblings[i],
                    "Sibling hash should match after round-trip");
    }

    // Deserialized proof should still verify
    auto roots = forest.getRoots();
    bool valid = deserialized.verify(utxos[3].leafHash, roots);
    TEST_ASSERT(valid, "Deserialized proof should still verify");

    std::cout << "  PASSED" << std::endl;
}

// ============================================================================
// Test 11: Tampered SpentOutputData Fails Validation
// ============================================================================

static void test_tampered_spent_output_data() {
    std::cout << "Test 11: Tampered SpentOutputData fails validation..." << std::endl;

    UtreexoForest forest;
    auto mock_utxo = std::make_shared<MockUTXOProvider>();

    auto u = makeTestUTXO(50, 0, 25000);
    u.forest_position = forest.add(u.leafHash);
    mock_utxo->AddTestUTXO(u.txid, u.vout, u.value, u.scriptPubKey);

    BridgeNode bridge(mock_utxo, &forest);
    StatelessNode csn(&forest);

    Transaction tx = buildSpendingTx({u});
    auto proofs = bridge.GenerateProofsForTransaction(tx);
    TEST_ASSERT(proofs.has_value(), "Proof generation should succeed");

    auto root = forest.getCommitment();

    // Tamper with value → leaf hash changes → proof fails
    auto tampered_proofs = *proofs;
    tampered_proofs[0].second.value = 99999;  // Wrong value

    bool valid = csn.ValidateUtreexoTx(tx, tampered_proofs, root);
    TEST_ASSERT(!valid, "CSN should reject tx with tampered SpentOutputData value");

    // Tamper with scriptPubKey
    auto tampered_proofs2 = *proofs;
    tampered_proofs2[0].second.scriptPubKey[0] ^= 0xFF;

    valid = csn.ValidateUtreexoTx(tx, tampered_proofs2, root);
    TEST_ASSERT(!valid, "CSN should reject tx with tampered SpentOutputData script");

    std::cout << "  PASSED" << std::endl;
}

// ============================================================================
// Test 12: Coinbase maturity enforced in tx proof validation
// ============================================================================

static void test_validate_utreexo_tx_rejects_immature_coinbase() {
    std::cout << "Test 12: ValidateUtreexoTx rejects immature coinbase..." << std::endl;

    const uint32_t activation = GetUtreexoMaturityLeafActivationHeight();
    TEST_ASSERT(activation > 0, "Regtest maturity leaf activation must be non-zero for this test");

    UtreexoForest forest;
    auto mock_utxo = std::make_shared<MockUTXOProvider>();

    TestUTXO coinbase = makeTestUTXO(900, 0, 100'00000000ULL);
    coinbase.leafHash = HashUTXOForCreationHeight(
        coinbase.txid.AsUint256(),
        coinbase.vout,
        coinbase.value,
        coinbase.scriptPubKey,
        activation,
        true);
    coinbase.forest_position = forest.add(coinbase.leafHash);
    mock_utxo->AddTestUTXO(
        coinbase.txid,
        coinbase.vout,
        coinbase.value,
        coinbase.scriptPubKey,
        activation,
        true);

    BridgeNode bridge(mock_utxo, &forest);
    StatelessNode csn(&forest);
    Transaction tx = buildSpendingTx({coinbase});
    auto proofs = bridge.GenerateProofsForTransaction(tx);
    TEST_ASSERT(proofs.has_value(), "Proof generation should succeed for coinbase UTXO");
    TEST_ASSERT((*proofs)[0].second.created_height == activation,
                "SpentOutputData must carry coinbase creation height");
    TEST_ASSERT((*proofs)[0].second.is_coinbase,
                "SpentOutputData must carry coinbase flag");

    const auto root = forest.getCommitment();
    bool valid = csn.ValidateUtreexoTx(tx, *proofs, root, activation + 99);
    TEST_ASSERT(!valid, "CSN mempool path must reject immature coinbase input");

    valid = csn.ValidateUtreexoTx(tx, *proofs, root, activation + 100);
    TEST_ASSERT(valid, "CSN mempool path must accept coinbase input at maturity boundary");

    std::cout << "  PASSED" << std::endl;
}

// ============================================================================
// Main
// ============================================================================

int main() {
    SelectParams(Chain::REGTEST);

    std::cout << std::endl;
    std::cout << "═══════════════════════════════════════════════════" << std::endl;
    std::cout << "  Phase #4: Utreexo TX Relay Test Suite" << std::endl;
    std::cout << "═══════════════════════════════════════════════════" << std::endl;

    test_generate_proofs_for_tx();
    test_validate_utreexo_tx_valid();
    test_validate_utreexo_tx_stale_root();
    test_validate_utreexo_tx_corrupt_proof();
    test_validate_utreexo_tx_proof_count_mismatch();
    test_utxotx_wire_format_roundtrip();
    test_coinbase_inputs_skipped();
    test_generate_proofs_missing_utxo();
    test_multiple_outputs_same_tx();
    test_proof_serialization_roundtrip();
    test_tampered_spent_output_data();
    test_validate_utreexo_tx_rejects_immature_coinbase();

    std::cout << std::endl;
    std::cout << "═══════════════════════════════════════════════════" << std::endl;
    std::cout << "  All " << tests_passed << "/" << tests_total
              << " assertions passed!" << std::endl;
    std::cout << "═══════════════════════════════════════════════════" << std::endl;

    return tests_passed == tests_total ? 0 : 1;
}
