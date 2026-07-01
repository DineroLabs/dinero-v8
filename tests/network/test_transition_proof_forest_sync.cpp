#include "network/stateless_node.h"
#include "network/utreexo_messages.h"
#include "consensus/chainparams.h"
#include "consensus/utreexo_accumulator.h"
#include "consensus/utreexo_maturity_leaf_activation.h"
#include "primitives/block.h"
#include "primitives/transaction.h"

#include <cassert>
#include <cstring>
#include <iostream>
#include <vector>

using namespace dinero;
using namespace dinero::consensus;
using namespace dinero::network;

namespace {

int tests_passed = 0;
int tests_total = 0;

#define TEST_ASSERT(cond, msg) do { \
    tests_total++; \
    if (!(cond)) { \
        std::cerr << "FAIL: " << msg << " (" << __FILE__ << ":" << __LINE__ << ")" << std::endl; \
        assert(false); \
    } else { \
        tests_passed++; \
    } \
} while (0)

std::vector<uint8_t> makeScript() {
    std::vector<uint8_t> script = {0x51, 0x20};
    script.resize(34, 0x00);
    return script;
}

Block makeCoinbaseBlock(uint32_t height,
                        const std::vector<std::pair<uint64_t, std::vector<uint8_t>>>& outputs,
                        const uint256& prev_hash = uint256()) {
    Block block;
    std::memset(&block.header, 0, sizeof(BlockHeader));
    block.header.version = 1;
    block.header.prev_block_hash = prev_hash;
    block.header.timestamp = 1700000000 + height * 600;
    block.header.difficulty = 0x1d00ffff;
    block.header.nonce = height;

    Transaction coinbase;
    coinbase.version = 2;
    coinbase.witness_version = 0xFF;
    TxInput cb_in;
    cb_in.prevout.vout = 0xFFFFFFFF;
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

std::vector<UtreexoHash> extractAdditions(const Block& block) {
    return UtreexoTransitionProof::computeAdditionHashes(block);
}

void applyAdditions(UtreexoForest& forest, const Block& block) {
    for (const auto& leaf : extractAdditions(block)) {
        TEST_ASSERT(forest.add(leaf) != UINT64_MAX, "Forest add should succeed");
    }
}

void setHeaderRootFromForest(Block& block, const UtreexoForest& forest) {
    const auto root = forest.getCommitment();
    TEST_ASSERT(root.size() == 32, "Forest commitment must be 32 bytes");
    std::memcpy(block.header.utreexo_root.begin(), root.data(), 32);
}

Block makeSpendBlock(uint32_t height,
                     const TxId& prev_txid,
                     uint32_t prev_vout,
                     uint64_t output_value,
                     const std::vector<uint8_t>& script,
                     const uint256& prev_hash = uint256()) {
    Block block = makeCoinbaseBlock(height, {{1'00000000ULL, script}}, prev_hash);

    Transaction spend;
    spend.version = 2;
    spend.witness_version = 0xFF;
    TxInput input;
    input.prevout.txid = prev_txid;
    input.prevout.vout = prev_vout;
    spend.vin.push_back(input);
    TxOutput out;
    out.value = AmountUna::Una(output_value);
    out.scriptPubKey = script;
    spend.vout.push_back(out);
    block.vtx.push_back(spend);
    return block;
}

Block makeCpfpBlock(uint32_t height,
                    const TxId& funding_txid,
                    uint32_t funding_vout,
                    uint64_t parent_value,
                    uint64_t child_value,
                    const std::vector<uint8_t>& script,
                    const uint256& prev_hash = uint256()) {
    Block block = makeCoinbaseBlock(height, {{1'00000000ULL, script}}, prev_hash);

    Transaction parent;
    parent.version = 2;
    parent.witness_version = 0xFF;
    TxInput parent_input;
    parent_input.prevout.txid = funding_txid;
    parent_input.prevout.vout = funding_vout;
    parent.vin.push_back(parent_input);
    TxOutput parent_output;
    parent_output.value = AmountUna::Una(parent_value);
    parent_output.scriptPubKey = script;
    parent.vout.push_back(parent_output);
    block.vtx.push_back(parent);

    Transaction child;
    child.version = 2;
    child.witness_version = 0xFF;
    TxInput child_input;
    child_input.prevout.txid = parent.GetTxid();
    child_input.prevout.vout = 0;
    child.vin.push_back(child_input);
    TxOutput child_output;
    child_output.value = AmountUna::Una(child_value);
    child_output.scriptPubKey = script;
    child.vout.push_back(child_output);
    block.vtx.push_back(child);

    return block;
}

void test_transition_proof_keeps_empty_forest_aligned() {
    std::cout << "Transition proof forest sync from empty pre-state..." << std::endl;

    auto script = makeScript();

    UtreexoForest bridge_forest;
    UtreexoForest csn_forest;
    StatelessNode csn(&csn_forest);
    csn.SyncToForestState(0);

    Block block1 = makeCoinbaseBlock(1, {{25'00000000ULL, script}});
    BlockUtreexoProof empty_batch_proof;
    UtreexoTransitionProof tp1 = UtreexoTransitionProof::generate(bridge_forest, block1, empty_batch_proof);
    applyAdditions(bridge_forest, block1);

    UtreexoProofMessage proof_msg1;
    proof_msg1.block_hash = block1.GetHash();
    proof_msg1.block_height = 1;
    proof_msg1.accumulator_root_after = tp1.commitment_after;

    TEST_ASSERT(
        csn.ValidateWithTransitionProof(block1, proof_msg1, tp1, 1),
        "Deletion-free transition proof from empty pre-state should validate"
    );
    TEST_ASSERT(
        csn_forest.getCommitment() == bridge_forest.getCommitment(),
        "Shared forest must advance from empty pre-state alongside deletion-free TP validation"
    );
}

void test_transition_proof_keeps_forest_aligned_for_next_batch_block() {
    std::cout << "Transition proof forest sync regression..." << std::endl;

    auto script = makeScript();

    Block genesis = makeCoinbaseBlock(0, {{50'00000000ULL, script}});
    UtreexoForest bridge_forest;
    applyAdditions(bridge_forest, genesis);

    UtreexoForest csn_forest = bridge_forest;
    StatelessNode csn(&csn_forest);
    csn.SyncToForestState(0);

    Block block1 = makeCoinbaseBlock(1, {{25'00000000ULL, script}}, genesis.GetHash());
    BlockUtreexoProof empty_batch_proof;
    UtreexoTransitionProof tp1 = UtreexoTransitionProof::generate(bridge_forest, block1, empty_batch_proof);
    applyAdditions(bridge_forest, block1);

    UtreexoProofMessage proof_msg1;
    proof_msg1.block_hash = block1.GetHash();
    proof_msg1.block_height = 1;
    proof_msg1.accumulator_root_after = tp1.commitment_after;

    TEST_ASSERT(
        csn.ValidateWithTransitionProof(block1, proof_msg1, tp1, 1),
        "Deletion-free transition proof should validate"
    );
    TEST_ASSERT(
        csn_forest.getCommitment() == bridge_forest.getCommitment(),
        "Shared forest must advance alongside deletion-free TP validation"
    );

    Block block2 = makeCoinbaseBlock(2, {{12'50000000ULL, script}}, block1.GetHash());
    UtreexoHash root_before_2 = bridge_forest.getCommitment();
    applyAdditions(bridge_forest, block2);

    UtreexoProofMessage proof_msg2;
    proof_msg2.block_hash = block2.GetHash();
    proof_msg2.block_height = 2;
    proof_msg2.accumulator_root_before = root_before_2;
    proof_msg2.accumulator_root_after = bridge_forest.getCommitment();
    proof_msg2.proof_data.accumulator_root_before = root_before_2;
    proof_msg2.proof_data.spend_proof.numLeaves = csn_forest.getNumLeaves();

    TEST_ASSERT(
        csn.ValidateUtreexoProof(block2, proof_msg2, 1),
        "Next batch proof should see the advanced shared forest pre-state"
    );
}

void test_replay_rejects_immature_v2_coinbase_spend() {
    std::cout << "CSN replay rejects immature v2 coinbase spend..." << std::endl;

    const uint32_t activation = GetUtreexoMaturityLeafActivationHeight();
    TEST_ASSERT(activation > 0, "Regtest maturity leaf activation must be non-zero for this test");
    const uint32_t coinbase_height = activation;
    const uint32_t spend_height = coinbase_height + 99;

    auto script = makeScript();
    Block coinbase_block = makeCoinbaseBlock(coinbase_height, {{50'00000000ULL, script}});
    const TxId coinbase_txid = coinbase_block.vtx[0].GetTxid();
    const uint64_t coinbase_value = coinbase_block.vtx[0].vout[0].value.GetUna();

    const UtreexoHash coinbase_leaf = HashUTXOForCreationHeight(
        coinbase_txid.AsUint256(), 0, coinbase_value, script, coinbase_height, true);

    UtreexoForest initial_forest;
    TEST_ASSERT(initial_forest.add(coinbase_leaf) != UINT64_MAX, "Initial coinbase leaf add should succeed");

    Block spend_block = makeSpendBlock(
        spend_height, coinbase_txid, 0, 49'99900000ULL, script, coinbase_block.GetHash());

    UtreexoForest expected_after = initial_forest;
    auto position = expected_after.findLeafPosition(coinbase_leaf);
    TEST_ASSERT(position.has_value(), "Expected after-forest must locate coinbase leaf");
    auto proof = expected_after.prove(*position);
    TEST_ASSERT(proof.has_value(), "Expected after-forest must prove coinbase leaf");
    TEST_ASSERT(expected_after.remove(coinbase_leaf, *proof), "Expected after-forest remove should succeed");
    applyAdditions(expected_after, spend_block);
    setHeaderRootFromForest(spend_block, expected_after);

    std::vector<SpentOutputData> spent_outputs;
    spent_outputs.emplace_back(coinbase_value, script, coinbase_height, true);

    UtreexoForest replay_forest = initial_forest;
    StatelessNode csn(&replay_forest);
    csn.SyncToForestState(coinbase_height);

    TEST_ASSERT(
        !csn.ReplayBlock(spend_block, spend_height, {coinbase_leaf}, &spent_outputs),
        "ReplayBlock must reject an immature v2 coinbase spend even when the root transition matches"
    );
    TEST_ASSERT(
        replay_forest.getCommitment() == initial_forest.getCommitment(),
        "Failed replay must leave the forest unchanged"
    );
}

void test_replay_requires_metadata_after_maturity_leaf_activation() {
    std::cout << "CSN replay requires maturity metadata after activation..." << std::endl;

    const uint32_t activation = GetUtreexoMaturityLeafActivationHeight();
    TEST_ASSERT(activation > 0, "Regtest maturity leaf activation must be non-zero for this test");
    auto script = makeScript();

    Block source = makeCoinbaseBlock(activation, {{1'00000000ULL, script}});
    const TxId source_txid = source.vtx[0].GetTxid();
    const uint64_t source_value = source.vtx[0].vout[0].value.GetUna();
    const UtreexoHash source_leaf = HashUTXOForCreationHeight(
        source_txid.AsUint256(), 0, source_value, script, activation, false);

    UtreexoForest initial_forest;
    TEST_ASSERT(initial_forest.add(source_leaf) != UINT64_MAX, "Initial non-coinbase leaf add should succeed");

    Block spend_block = makeSpendBlock(activation + 1, source_txid, 0, 99'000000ULL, script, source.GetHash());
    UtreexoForest expected_after = initial_forest;
    auto position = expected_after.findLeafPosition(source_leaf);
    TEST_ASSERT(position.has_value(), "Expected after-forest must locate source leaf");
    auto proof = expected_after.prove(*position);
    TEST_ASSERT(proof.has_value(), "Expected after-forest must prove source leaf");
    TEST_ASSERT(expected_after.remove(source_leaf, *proof), "Expected after-forest remove should succeed");
    applyAdditions(expected_after, spend_block);
    setHeaderRootFromForest(spend_block, expected_after);

    UtreexoForest replay_forest = initial_forest;
    StatelessNode csn(&replay_forest);
    csn.SyncToForestState(activation);

    TEST_ASSERT(
        !csn.ReplayBlock(spend_block, activation + 1, {source_leaf}),
        "ReplayBlock must fail closed when post-activation spent-output metadata is missing"
    );
}

void test_replay_binds_maturity_metadata_to_spend_target() {
    std::cout << "CSN replay binds maturity metadata to spend target..." << std::endl;

    const uint32_t activation = GetUtreexoMaturityLeafActivationHeight();
    TEST_ASSERT(activation > 0, "Regtest maturity leaf activation must be non-zero for this test");
    auto script = makeScript();

    const uint32_t coinbase_height = activation;
    const uint32_t spend_height = coinbase_height + 99;
    Block coinbase_block = makeCoinbaseBlock(coinbase_height, {{50'00000000ULL, script}});
    const TxId coinbase_txid = coinbase_block.vtx[0].GetTxid();
    const uint64_t coinbase_value = coinbase_block.vtx[0].vout[0].value.GetUna();

    const UtreexoHash true_coinbase_leaf = HashUTXOForCreationHeight(
        coinbase_txid.AsUint256(), 0, coinbase_value, script, coinbase_height, true);

    UtreexoForest initial_forest;
    TEST_ASSERT(initial_forest.add(true_coinbase_leaf) != UINT64_MAX, "Initial coinbase leaf add should succeed");

    Block spend_block = makeSpendBlock(
        spend_height, coinbase_txid, 0, 49'99900000ULL, script, coinbase_block.GetHash());

    UtreexoForest expected_after = initial_forest;
    auto position = expected_after.findLeafPosition(true_coinbase_leaf);
    TEST_ASSERT(position.has_value(), "Expected after-forest must locate true coinbase leaf");
    auto proof = expected_after.prove(*position);
    TEST_ASSERT(proof.has_value(), "Expected after-forest must prove true coinbase leaf");
    TEST_ASSERT(expected_after.remove(true_coinbase_leaf, *proof), "Expected after-forest remove should succeed");
    applyAdditions(expected_after, spend_block);
    setHeaderRootFromForest(spend_block, expected_after);

    auto assert_lied_metadata_rejected = [&](const SpentOutputData& lied_spent,
                                             const char* label) {
        UtreexoForest replay_forest = initial_forest;
        StatelessNode csn(&replay_forest);
        csn.SyncToForestState(coinbase_height);

        std::vector<SpentOutputData> spent_outputs = {lied_spent};
        TEST_ASSERT(
            !csn.ReplayBlock(spend_block, spend_height, {true_coinbase_leaf}, &spent_outputs),
            std::string("ReplayBlock must reject true hash plus lied metadata: ") + label
        );
        TEST_ASSERT(
            replay_forest.getCommitment() == initial_forest.getCommitment(),
            std::string("Lied metadata replay must leave forest unchanged: ") + label
        );
    };

    assert_lied_metadata_rejected(
        SpentOutputData(coinbase_value, script, coinbase_height, false),
        "is_coinbase=false");
    assert_lied_metadata_rejected(
        SpentOutputData(coinbase_value, script, activation - 1, true),
        "created_height<activation");
}

void test_replay_accepts_post_activation_cpfp_and_binds_child_metadata() {
    std::cout << "CSN replay accepts post-activation CPFP and binds child metadata..." << std::endl;

    const uint32_t activation = GetUtreexoMaturityLeafActivationHeight();
    TEST_ASSERT(activation > 0, "Regtest maturity leaf activation must be non-zero for this test");
    const uint32_t funding_height = activation;
    const uint32_t spend_height = activation + 1;
    auto script = makeScript();

    Block funding_block = makeCoinbaseBlock(funding_height, {{50'00000000ULL, script}});
    const TxId funding_txid = funding_block.vtx[0].GetTxid();
    const uint64_t funding_value = funding_block.vtx[0].vout[0].value.GetUna();
    const UtreexoHash funding_leaf = HashUTXOForCreationHeight(
        funding_txid.AsUint256(), 0, funding_value, script, funding_height, false);

    UtreexoForest initial_forest;
    TEST_ASSERT(initial_forest.add(funding_leaf) != UINT64_MAX, "Initial funding leaf add should succeed");

    const uint64_t parent_value = 49'99900000ULL;
    const uint64_t child_value = 49'99800000ULL;
    Block cpfp_block = makeCpfpBlock(
        spend_height, funding_txid, 0, parent_value, child_value, script, funding_block.GetHash());

    UtreexoForest expected_after = initial_forest;
    auto position = expected_after.findLeafPosition(funding_leaf);
    TEST_ASSERT(position.has_value(), "Expected after-forest must locate funding leaf");
    auto proof = expected_after.prove(*position);
    TEST_ASSERT(proof.has_value(), "Expected after-forest must prove funding leaf");
    TEST_ASSERT(expected_after.remove(funding_leaf, *proof), "Expected after-forest remove should succeed");
    const TxId coinbase_txid = cpfp_block.vtx[0].GetTxid();
    TEST_ASSERT(
        expected_after.add(HashUTXOForCreationHeight(
            coinbase_txid.AsUint256(),
            0,
            cpfp_block.vtx[0].vout[0].value.GetUna(),
            cpfp_block.vtx[0].vout[0].scriptPubKey,
            spend_height,
            true)) != UINT64_MAX,
        "Expected after-forest coinbase add should succeed");
    const TxId child_txid = cpfp_block.vtx[2].GetTxid();
    TEST_ASSERT(
        expected_after.add(HashUTXOForCreationHeight(
            child_txid.AsUint256(),
            0,
            cpfp_block.vtx[2].vout[0].value.GetUna(),
            cpfp_block.vtx[2].vout[0].scriptPubKey,
            spend_height,
            false)) != UINT64_MAX,
        "Expected after-forest child add should succeed");
    setHeaderRootFromForest(cpfp_block, expected_after);

    std::vector<SpentOutputData> spent_outputs;
    spent_outputs.emplace_back(funding_value, script, funding_height, false);
    spent_outputs.emplace_back(parent_value, script, spend_height, false);

    UtreexoForest replay_forest = initial_forest;
    StatelessNode csn(&replay_forest);
    csn.SyncToForestState(funding_height);
    TEST_ASSERT(
        csn.ReplayBlock(cpfp_block, spend_height, {funding_leaf}, &spent_outputs),
        "ReplayBlock must accept a post-activation CPFP block with one forest target and two spent outputs"
    );
    TEST_ASSERT(
        replay_forest.getCommitment() == expected_after.getCommitment(),
        "Accepted CPFP replay must advance the forest to the expected root"
    );

    std::vector<SpentOutputData> lied_child_outputs = spent_outputs;
    lied_child_outputs[1].value = parent_value - 1;
    UtreexoForest lied_replay_forest = initial_forest;
    StatelessNode lied_csn(&lied_replay_forest);
    lied_csn.SyncToForestState(funding_height);
    TEST_ASSERT(
        !lied_csn.ReplayBlock(cpfp_block, spend_height, {funding_leaf}, &lied_child_outputs),
        "ReplayBlock must reject lied metadata for an ephemeral child input"
    );
    TEST_ASSERT(
        lied_replay_forest.getCommitment() == initial_forest.getCommitment(),
        "Rejected CPFP metadata lie must leave the forest unchanged"
    );
}

}  // namespace

int main() {
    SelectParams(Chain::REGTEST);

    test_transition_proof_keeps_empty_forest_aligned();
    test_transition_proof_keeps_forest_aligned_for_next_batch_block();
    test_replay_rejects_immature_v2_coinbase_spend();
    test_replay_requires_metadata_after_maturity_leaf_activation();
    test_replay_binds_maturity_metadata_to_spend_target();
    test_replay_accepts_post_activation_cpfp_and_binds_child_metadata();
    std::cout << "PASS: " << tests_passed << "/" << tests_total << " assertions" << std::endl;
    return 0;
}
