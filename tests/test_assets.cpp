/**
 * Phase 30: Taproot Asset Layer Tests
 *
 * Tests for asset ID, state, proofs, and transfers.
 */

#include <iostream>
#include <cassert>
#include <cstring>
#include <vector>
#include <array>
#include <iomanip>
#include <sstream>

#include "assets/asset_id.h"
#include "assets/asset_state.h"
#include "assets/asset_proof.h"
#include "assets/asset_transfer.h"

using namespace dinero::assets;

// ============================================================================
// Test Utilities
// ============================================================================

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) void test_##name()
#define RUN_TEST(name) do { \
    std::cout << "  Testing " << #name << "... "; \
    try { \
        test_##name(); \
        std::cout << "PASSED" << std::endl; \
        tests_passed++; \
    } catch (const std::exception& e) { \
        std::cout << "FAILED: " << e.what() << std::endl; \
        tests_failed++; \
    } catch (...) { \
        std::cout << "FAILED: Unknown exception" << std::endl; \
        tests_failed++; \
    } \
} while(0)

#define ASSERT_TRUE(cond) do { \
    if (!(cond)) { \
        throw std::runtime_error("Assertion failed: " #cond); \
    } \
} while(0)

#define ASSERT_FALSE(cond) ASSERT_TRUE(!(cond))

#define ASSERT_EQ(a, b) do { \
    if ((a) != (b)) { \
        std::ostringstream oss; \
        oss << "Assertion failed: " << #a << " == " << #b; \
        throw std::runtime_error(oss.str()); \
    } \
} while(0)

// Helper: Create a test 32-byte array
std::array<uint8_t, 32> makeTestKey32(uint8_t seed) {
    std::array<uint8_t, 32> key;
    for (int i = 0; i < 32; i++) {
        key[i] = static_cast<uint8_t>((seed + i) % 256);
    }
    return key;
}

// Helper: Create a test AssetID
AssetID makeTestAssetID(uint8_t seed) {
    return makeTestKey32(seed);
}

// ============================================================================
// Asset ID Tests
// ============================================================================

TEST(asset_id_computation) {
    auto pubkey = makeTestKey32(1);
    auto txid = makeTestKey32(2);
    auto metadata_hash = makeTestKey32(3);

    AssetID id = ComputeAssetID(pubkey, txid, metadata_hash);

    // ID should be 32 bytes
    ASSERT_EQ(id.size(), 32);

    // Same inputs should produce same ID
    AssetID id2 = ComputeAssetID(pubkey, txid, metadata_hash);
    ASSERT_TRUE(id == id2);

    // Different inputs should produce different ID
    auto different_hash = makeTestKey32(4);
    AssetID id3 = ComputeAssetID(pubkey, txid, different_hash);
    ASSERT_FALSE(id == id3);
}

TEST(asset_id_deterministic) {
    auto pubkey1 = makeTestKey32(10);
    auto txid1 = makeTestKey32(20);
    auto meta_hash = makeTestKey32(30);

    AssetID id1 = ComputeAssetID(pubkey1, txid1, meta_hash);
    AssetID id2 = ComputeAssetID(pubkey1, txid1, meta_hash);

    ASSERT_TRUE(id1 == id2);

    // Change pubkey slightly
    auto pubkey2 = pubkey1;
    pubkey2[5] ^= 0x01;
    AssetID id3 = ComputeAssetID(pubkey2, txid1, meta_hash);
    ASSERT_FALSE(id1 == id3);
}

TEST(asset_id_hex_conversion) {
    AssetID id = makeTestAssetID(42);

    std::string hex = AssetIDToHex(id);
    ASSERT_EQ(hex.length(), 64);

    auto parsed = AssetIDFromHex(hex);
    ASSERT_TRUE(parsed.has_value());
    ASSERT_TRUE(AssetIDEqual(id, *parsed));
}

TEST(asset_id_invalid_hex) {
    auto result1 = AssetIDFromHex("abcd");
    ASSERT_FALSE(result1.has_value());

    auto result2 = AssetIDFromHex(std::string(64, 'g'));
    ASSERT_FALSE(result2.has_value());
}

TEST(asset_id_short_display) {
    AssetID id = makeTestAssetID(0x12);
    std::string short_id = AssetIDShort(id);

    ASSERT_TRUE(short_id.length() > 0);
    ASSERT_EQ(short_id.length(), 8);  // First 8 hex chars
}

TEST(asset_id_native) {
    AssetID null_id = NullAssetID();
    ASSERT_TRUE(IsNativeAsset(null_id));

    AssetID non_null = makeTestAssetID(1);
    ASSERT_FALSE(IsNativeAsset(non_null));
}

// ============================================================================
// Asset Metadata Tests
// ============================================================================

TEST(metadata_serialization) {
    AssetMetadata meta;
    meta.name = "SerializeTest";
    meta.ticker = "SER";
    meta.decimals = 18;
    meta.description = "Testing serialization";
    meta.icon_url = "https://example.com/icon.png";

    auto serialized = meta.serialize();
    ASSERT_TRUE(serialized.size() > 0);

    auto deserialized = AssetMetadata::deserialize(serialized);
    ASSERT_TRUE(deserialized.has_value());

    ASSERT_EQ(deserialized->name, meta.name);
    ASSERT_EQ(deserialized->ticker, meta.ticker);
    ASSERT_EQ(deserialized->decimals, meta.decimals);
}

TEST(metadata_hash) {
    AssetMetadata meta1;
    meta1.name = "HashTest";
    meta1.ticker = "HASH";
    meta1.decimals = 8;

    AssetMetadata meta2 = meta1;

    auto hash1 = meta1.hash();
    auto hash2 = meta2.hash();

    ASSERT_TRUE(hash1 == hash2);

    meta2.decimals = 10;
    auto hash3 = meta2.hash();
    ASSERT_FALSE(hash1 == hash3);
}

TEST(metadata_json) {
    AssetMetadata meta;
    meta.name = "JSONTest";
    meta.ticker = "JSON";
    meta.decimals = 6;
    meta.description = "JSON roundtrip test";

    std::string json = meta.toJSON();
    ASSERT_TRUE(json.find("JSONTest") != std::string::npos);
    ASSERT_TRUE(json.find("JSON") != std::string::npos);

    auto parsed = AssetMetadata::fromJSON(json);
    ASSERT_TRUE(parsed.has_value());
    ASSERT_EQ(parsed->name, meta.name);
}

// ============================================================================
// Asset Supply Config Tests
// ============================================================================

TEST(supply_config_fixed) {
    AssetSupplyConfig config;
    config.model = SupplyModel::FIXED;
    config.initial_supply = 1000000;
    config.max_supply = 1000000;
    config.burn_enabled = false;

    auto serialized = config.serialize();
    auto deserialized = AssetSupplyConfig::deserialize(serialized);

    ASSERT_TRUE(deserialized.has_value());
    ASSERT_EQ(static_cast<int>(deserialized->model), static_cast<int>(SupplyModel::FIXED));
    ASSERT_EQ(deserialized->initial_supply, config.initial_supply);
    ASSERT_FALSE(deserialized->burn_enabled);
}

TEST(supply_config_capped) {
    AssetSupplyConfig config;
    config.model = SupplyModel::CAPPED;
    config.initial_supply = 100000;
    config.max_supply = 21000000;
    config.burn_enabled = true;

    auto serialized = config.serialize();
    auto deserialized = AssetSupplyConfig::deserialize(serialized);

    ASSERT_TRUE(deserialized.has_value());
    ASSERT_TRUE(deserialized->burn_enabled);
}

// ============================================================================
// Asset Genesis Tests
// ============================================================================

TEST(genesis_compute_id) {
    AssetGenesis genesis;
    genesis.issuer_pubkey = makeTestKey32(50);
    genesis.creation_txid = "0102030405060708091011121314151617181920212223242526272829303132";
    genesis.creation_output_index = 0;
    genesis.metadata.name = "GenesisToken";
    genesis.metadata.ticker = "GEN";
    genesis.metadata.decimals = 8;
    genesis.supply.model = SupplyModel::FIXED;
    genesis.supply.initial_supply = 1000000;

    AssetID id = genesis.computeID();
    ASSERT_EQ(id.size(), 32);

    AssetID id2 = genesis.computeID();
    ASSERT_TRUE(id == id2);
}

TEST(genesis_script_generation) {
    AssetGenesis genesis;
    genesis.issuer_pubkey = makeTestKey32(70);
    genesis.creation_txid = "aabbccdd1234567890abcdef1234567890abcdef1234567890abcdef12345678";
    genesis.creation_output_index = 0;
    genesis.metadata.name = "ScriptToken";
    genesis.metadata.ticker = "SCR";

    auto script = genesis.generateGenesisScript();
    ASSERT_TRUE(script.size() > 0);
}

// ============================================================================
// Asset Commitment Tests
// ============================================================================

TEST(commitment_to_script) {
    AssetCommitment commitment;
    commitment.asset_id = makeTestAssetID(110);
    commitment.amount = 100000;
    commitment.state_hash = makeTestAssetID(120);

    auto script = commitment.toScript();
    ASSERT_TRUE(script.size() > 0);
    ASSERT_TRUE(script.size() >= 32 + 8);
}

TEST(commitment_from_script) {
    AssetCommitment original;
    original.asset_id = makeTestAssetID(130);
    original.amount = 999999;
    original.state_hash = makeTestAssetID(140);

    auto script = original.toScript();
    auto parsed = AssetCommitment::fromScript(script);

    ASSERT_TRUE(parsed.has_value());
    ASSERT_TRUE(parsed->asset_id == original.asset_id);
    ASSERT_EQ(parsed->amount, original.amount);
}

TEST(commitment_hash) {
    AssetCommitment commitment;
    commitment.asset_id = makeTestAssetID(150);
    commitment.amount = 50000;
    commitment.state_hash = makeTestAssetID(160);

    auto hash1 = commitment.hash();
    auto hash2 = commitment.hash();

    ASSERT_TRUE(hash1 == hash2);

    commitment.amount = 60000;
    auto hash3 = commitment.hash();
    ASSERT_FALSE(hash1 == hash3);
}

// ============================================================================
// Asset UTXO Tests
// ============================================================================

TEST(utxo_outpoint) {
    AssetUTXO utxo;
    utxo.txid = "abcd1234567890abcd1234567890abcd1234567890abcd1234567890abcd1234";
    utxo.vout = 2;

    std::string outpoint = utxo.outpoint();
    ASSERT_TRUE(outpoint.find(":2") != std::string::npos);
}

TEST(utxo_serialization) {
    AssetUTXO utxo;
    utxo.txid = "1111111111111111111111111111111111111111111111111111111111111111";
    utxo.vout = 0;
    utxo.asset_id = makeTestAssetID(170);
    utxo.amount = 1000000;
    utxo.state_hash = makeTestAssetID(180);
    utxo.script_pubkey = {0x51, 0x20};
    for (int i = 0; i < 32; i++) utxo.script_pubkey.push_back(0xAA);
    utxo.owner_address = "din1qtest...";
    utxo.height = 500000;
    utxo.timestamp = 1700000000;
    utxo.is_spent = false;

    auto serialized = utxo.serialize();
    auto deserialized = AssetUTXO::deserialize(serialized);

    ASSERT_TRUE(deserialized.has_value());
    ASSERT_EQ(deserialized->vout, utxo.vout);
    ASSERT_EQ(deserialized->amount, utxo.amount);
    ASSERT_FALSE(deserialized->is_spent);
}

// ============================================================================
// Asset State Transition Tests
// ============================================================================

TEST(state_transition_transfer) {
    AssetStateTransition transition;
    transition.type = TransitionType::TRANSFER;

    AssetStateTransition::Input input;
    input.txid = "4444444444444444444444444444444444444444444444444444444444444444";
    input.vout = 0;
    input.asset_id = makeTestAssetID(210);
    input.amount = 1000;
    transition.inputs.push_back(input);

    AssetStateTransition::Output out1;
    out1.asset_id = input.asset_id;
    out1.amount = 600;
    transition.outputs.push_back(out1);

    AssetStateTransition::Output out2;
    out2.asset_id = input.asset_id;
    out2.amount = 400;
    transition.outputs.push_back(out2);

    ASSERT_TRUE(transition.validate());
    ASSERT_TRUE(transition.checkConservation());
}

TEST(state_transition_inflation_fail) {
    AssetStateTransition transition;
    transition.type = TransitionType::TRANSFER;

    AssetStateTransition::Input input;
    input.asset_id = makeTestAssetID(220);
    input.amount = 1000;
    transition.inputs.push_back(input);

    AssetStateTransition::Output out;
    out.asset_id = input.asset_id;
    out.amount = 1001;
    transition.outputs.push_back(out);

    ASSERT_FALSE(transition.checkConservation());
}

TEST(state_transition_mint) {
    AssetStateTransition transition;
    transition.type = TransitionType::MINT;

    AssetStateTransition::Output out;
    out.asset_id = makeTestAssetID(240);
    out.amount = 10000;
    transition.outputs.push_back(out);

    ASSERT_TRUE(transition.validate());
}

TEST(state_transition_burn) {
    AssetStateTransition transition;
    transition.type = TransitionType::BURN;

    AssetStateTransition::Input input;
    input.asset_id = makeTestAssetID(250);
    input.amount = 5000;
    transition.inputs.push_back(input);

    ASSERT_TRUE(transition.validate());
}

TEST(state_transition_hash) {
    AssetStateTransition transition;
    transition.type = TransitionType::TRANSFER;

    AssetStateTransition::Input input;
    input.txid = "6666666666666666666666666666666666666666666666666666666666666666";
    input.asset_id = makeTestAssetID(26);
    input.amount = 500;
    transition.inputs.push_back(input);

    AssetStateTransition::Output out;
    out.asset_id = input.asset_id;
    out.amount = 500;
    transition.outputs.push_back(out);

    auto hash1 = transition.computeHash();
    auto hash2 = transition.computeHash();

    ASSERT_TRUE(hash1 == hash2);
}

// ============================================================================
// Coin Selection Tests
// ============================================================================

TEST(coin_selection_exact_match) {
    std::vector<AssetUTXO> available;
    AssetID asset_id = makeTestAssetID(27);

    for (int i = 0; i < 5; i++) {
        AssetUTXO utxo;
        utxo.txid = std::string(64, '0' + i);
        utxo.vout = 0;
        utxo.asset_id = asset_id;
        utxo.amount = 1000 * (i + 1);
        utxo.is_spent = false;
        available.push_back(utxo);
    }

    auto result = SelectAssetCoins(available, asset_id, 3000);

    ASSERT_EQ(result.total_selected, 3000);
    ASSERT_EQ(result.change_amount, 0);
}

TEST(coin_selection_with_change) {
    std::vector<AssetUTXO> available;
    AssetID asset_id = makeTestAssetID(28);

    AssetUTXO utxo;
    utxo.txid = std::string(64, 'a');
    utxo.vout = 0;
    utxo.asset_id = asset_id;
    utxo.amount = 10000;
    utxo.is_spent = false;
    available.push_back(utxo);

    auto result = SelectAssetCoins(available, asset_id, 7500);

    ASSERT_EQ(result.total_selected, 10000);
    ASSERT_EQ(result.change_amount, 2500);
}

TEST(coin_selection_skip_spent) {
    std::vector<AssetUTXO> available;
    AssetID asset_id = makeTestAssetID(30);

    AssetUTXO spent;
    spent.txid = std::string(64, 'x');
    spent.vout = 0;
    spent.asset_id = asset_id;
    spent.amount = 10000;
    spent.is_spent = true;
    available.push_back(spent);

    AssetUTXO unspent;
    unspent.txid = std::string(64, 'y');
    unspent.vout = 0;
    unspent.asset_id = asset_id;
    unspent.amount = 5000;
    unspent.is_spent = false;
    available.push_back(unspent);

    auto result = SelectAssetCoins(available, asset_id, 3000);

    ASSERT_EQ(result.total_selected, 5000);
    ASSERT_EQ(result.selected_utxos.size(), 1);
}

TEST(coin_selection_wrong_asset) {
    std::vector<AssetUTXO> available;
    AssetID asset1 = makeTestAssetID(31);
    AssetID asset2 = makeTestAssetID(32);

    AssetUTXO utxo;
    utxo.txid = std::string(64, 'z');
    utxo.vout = 0;
    utxo.asset_id = asset1;
    utxo.amount = 10000;
    utxo.is_spent = false;
    available.push_back(utxo);

    auto result = SelectAssetCoins(available, asset2, 5000);

    ASSERT_EQ(result.total_selected, 0);
    ASSERT_EQ(result.selected_utxos.size(), 0);
}

// ============================================================================
// Merkle Proof Tests
// ============================================================================

TEST(merkle_tree_build) {
    std::vector<std::array<uint8_t, 32>> leaves;
    for (int i = 0; i < 4; i++) {
        leaves.push_back(makeTestAssetID(33 + i));
    }

    auto root = BuildMerkleTree(leaves);
    ASSERT_EQ(root.size(), 32);  // Root is a 32-byte hash
}

TEST(merkle_proof_verify) {
    std::vector<std::array<uint8_t, 32>> leaves;
    for (int i = 0; i < 4; i++) {
        leaves.push_back(makeTestAssetID(34 + i));
    }

    auto root = BuildMerkleTree(leaves);
    MerkleProof proof = GenerateMerkleProof(leaves, 1);

    // Proof should have the leaf hash set
    ASSERT_TRUE(proof.leaf_hash == leaves[1]);
    // Proof should have the root set
    ASSERT_TRUE(proof.root == root);
    // Proof should verify
    ASSERT_TRUE(proof.verify());
}

TEST(merkle_proof_single_leaf) {
    std::vector<std::array<uint8_t, 32>> leaves;
    leaves.push_back(makeTestAssetID(35));

    auto root = BuildMerkleTree(leaves);

    // Single leaf: root equals the leaf
    ASSERT_TRUE(root == leaves[0]);
}

// ============================================================================
// Tagged Hash Tests
// ============================================================================

TEST(tagged_hash) {
    std::vector<uint8_t> data = {0x01, 0x02, 0x03, 0x04};

    auto hash1 = TaggedHash("test/tag", data);
    auto hash2 = TaggedHash("test/tag", data);

    ASSERT_TRUE(hash1 == hash2);

    auto hash3 = TaggedHash("other/tag", data);
    ASSERT_FALSE(hash1 == hash3);
}

TEST(taproot_leaf_hash) {
    std::vector<uint8_t> script = {0x51, 0x20};
    for (int i = 0; i < 32; i++) script.push_back(0xBB);

    auto hash = TaprootLeafHash(0xC0, script);  // version, then script
    ASSERT_EQ(hash.size(), 32);
}

TEST(taproot_branch_hash) {
    std::array<uint8_t, 32> left = makeTestAssetID(37);
    std::array<uint8_t, 32> right = makeTestAssetID(38);

    auto branch = TaprootBranchHash(left, right);
    ASSERT_EQ(branch.size(), 32);

    auto branch2 = TaprootBranchHash(right, left);
    ASSERT_TRUE(branch == branch2);
}

// ============================================================================
// Transfer Tests
// ============================================================================

TEST(transfer_request_validate) {
    AssetTransferRequest request;
    AssetID asset_id = makeTestAssetID(39);

    AssetTransferRequest::Source src;
    src.txid = std::string(64, '1');
    src.vout = 0;
    src.asset_id = asset_id;
    src.amount = 10000;
    request.sources.push_back(src);

    AssetTransferRequest::Destination dst;
    dst.asset_id = asset_id;
    dst.address = "din1q...recipient";
    dst.amount = 10000;
    request.destinations.push_back(dst);

    request.fee_rate = 1;

    auto result = request.validate();
    ASSERT_TRUE(result == TransferError::SUCCESS);
}

TEST(transfer_request_insufficient) {
    AssetTransferRequest request;
    AssetID asset_id = makeTestAssetID(40);

    AssetTransferRequest::Source src;
    src.asset_id = asset_id;
    src.amount = 5000;
    request.sources.push_back(src);

    AssetTransferRequest::Destination dst;
    dst.asset_id = asset_id;
    dst.amount = 10000;
    request.destinations.push_back(dst);

    ASSERT_FALSE(request.checkCoverage());
}

TEST(transfer_error_strings) {
    auto success_str = TransferErrorToString(TransferError::SUCCESS);
    ASSERT_TRUE(success_str.length() > 0);

    auto conservation_str = TransferErrorToString(TransferError::CONSERVATION_VIOLATION);
    ASSERT_TRUE(conservation_str.length() > 0);
}

// ============================================================================
// Asset Script Generation Tests
// ============================================================================

TEST(generate_asset_script) {
    auto asset_id = makeTestAssetID(43);
    uint64_t amount = 1000000;
    std::array<uint8_t, 32> state_hash = makeTestAssetID(44);

    auto script = GenerateAssetScript(asset_id, amount, state_hash);

    ASSERT_TRUE(script.size() > 0);
    ASSERT_TRUE(script.size() >= 32);
}

TEST(parse_asset_script) {
    auto asset_id = makeTestAssetID(47);
    uint64_t amount = 250000;
    std::array<uint8_t, 32> state_hash = makeTestAssetID(48);

    auto script = GenerateAssetScript(asset_id, amount, state_hash);
    auto parsed = ParseAssetScript(script);

    ASSERT_TRUE(parsed.has_value());
    ASSERT_TRUE(parsed->asset_id == asset_id);
    ASSERT_EQ(parsed->amount, amount);
}

// ============================================================================
// Multi-Asset Transaction Tests
// ============================================================================

TEST(multi_asset_tx_build) {
    MultiAssetTransaction tx;

    MultiAssetTransaction::AssetMovement mov1;
    mov1.asset_id = makeTestAssetID(49);
    mov1.amount = 1000;
    mov1.from_address = "sender1";
    mov1.to_address = "recipient1";
    tx.movements.push_back(mov1);

    tx.fee_amount = 500;

    auto raw = tx.buildRawTx();
    ASSERT_TRUE(raw.size() > 0);
}

TEST(multi_asset_tx_vsize) {
    MultiAssetTransaction tx;

    MultiAssetTransaction::AssetMovement mov;
    mov.asset_id = makeTestAssetID(51);
    mov.amount = 10000;
    tx.movements.push_back(mov);

    uint64_t vsize = tx.estimateVSize();

    ASSERT_TRUE(vsize > 0);
    ASSERT_TRUE(vsize < 10000);
}

// ============================================================================
// Asset State Machine Tests
// ============================================================================

TEST(state_machine_compute_new_state) {
    AssetStateMachine machine;
    machine.current_state = makeTestAssetID(52);
    machine.transition_count = 0;

    std::vector<uint8_t> input_data = {0x01, 0x02, 0x03};
    auto new_state = machine.computeNewState(input_data);

    ASSERT_EQ(new_state.size(), 32);
    ASSERT_FALSE(new_state == machine.current_state);
}

TEST(state_machine_verify_transition) {
    AssetStateMachine machine;
    machine.current_state = makeTestAssetID(53);
    machine.transition_count = 5;

    std::vector<uint8_t> proof = {0xAA, 0xBB, 0xCC};
    auto expected_new = machine.computeNewState(proof);

    ASSERT_TRUE(machine.verifyTransition(machine.current_state, expected_new, proof));

    auto wrong_old = makeTestAssetID(54);
    ASSERT_FALSE(machine.verifyTransition(wrong_old, expected_new, proof));
}

// ============================================================================
// Main Test Runner
// ============================================================================

int main() {
    std::cout << "\n========================================" << std::endl;
    std::cout << "Phase 30: Taproot Asset Layer Tests" << std::endl;
    std::cout << "========================================\n" << std::endl;

    // Asset ID Tests
    std::cout << "Asset ID Tests:" << std::endl;
    RUN_TEST(asset_id_computation);
    RUN_TEST(asset_id_deterministic);
    RUN_TEST(asset_id_hex_conversion);
    RUN_TEST(asset_id_invalid_hex);
    RUN_TEST(asset_id_short_display);
    RUN_TEST(asset_id_native);

    // Metadata Tests
    std::cout << "\nMetadata Tests:" << std::endl;
    RUN_TEST(metadata_serialization);
    RUN_TEST(metadata_hash);
    RUN_TEST(metadata_json);

    // Supply Config Tests
    std::cout << "\nSupply Config Tests:" << std::endl;
    RUN_TEST(supply_config_fixed);
    RUN_TEST(supply_config_capped);

    // Genesis Tests
    std::cout << "\nGenesis Tests:" << std::endl;
    RUN_TEST(genesis_compute_id);
    RUN_TEST(genesis_script_generation);

    // Commitment Tests
    std::cout << "\nCommitment Tests:" << std::endl;
    RUN_TEST(commitment_to_script);
    RUN_TEST(commitment_from_script);
    RUN_TEST(commitment_hash);

    // UTXO Tests
    std::cout << "\nUTXO Tests:" << std::endl;
    RUN_TEST(utxo_outpoint);
    RUN_TEST(utxo_serialization);

    // State Transition Tests
    std::cout << "\nState Transition Tests:" << std::endl;
    RUN_TEST(state_transition_transfer);
    RUN_TEST(state_transition_inflation_fail);
    RUN_TEST(state_transition_mint);
    RUN_TEST(state_transition_burn);
    RUN_TEST(state_transition_hash);

    // Coin Selection Tests
    std::cout << "\nCoin Selection Tests:" << std::endl;
    RUN_TEST(coin_selection_exact_match);
    RUN_TEST(coin_selection_with_change);
    RUN_TEST(coin_selection_skip_spent);
    RUN_TEST(coin_selection_wrong_asset);

    // Merkle Proof Tests
    std::cout << "\nMerkle Proof Tests:" << std::endl;
    RUN_TEST(merkle_tree_build);
    RUN_TEST(merkle_proof_verify);
    RUN_TEST(merkle_proof_single_leaf);

    // Tagged Hash Tests
    std::cout << "\nTagged Hash Tests:" << std::endl;
    RUN_TEST(tagged_hash);
    RUN_TEST(taproot_leaf_hash);
    RUN_TEST(taproot_branch_hash);

    // Transfer Tests
    std::cout << "\nTransfer Tests:" << std::endl;
    RUN_TEST(transfer_request_validate);
    RUN_TEST(transfer_request_insufficient);
    RUN_TEST(transfer_error_strings);

    // Script Generation Tests
    std::cout << "\nScript Generation Tests:" << std::endl;
    RUN_TEST(generate_asset_script);
    RUN_TEST(parse_asset_script);

    // Multi-Asset TX Tests
    std::cout << "\nMulti-Asset Transaction Tests:" << std::endl;
    RUN_TEST(multi_asset_tx_build);
    RUN_TEST(multi_asset_tx_vsize);

    // State Machine Tests
    std::cout << "\nState Machine Tests:" << std::endl;
    RUN_TEST(state_machine_compute_new_state);
    RUN_TEST(state_machine_verify_transition);

    // Summary
    std::cout << "\n========================================" << std::endl;
    std::cout << "Results: " << tests_passed << " passed, " << tests_failed << " failed" << std::endl;
    std::cout << "========================================\n" << std::endl;

    return tests_failed > 0 ? 1 : 0;
}
