#include "network/stateless_node.h"
#include "network/utreexo_messages.h"
#include "consensus/chainparams.h"
#include "consensus/block_validation.h"
#include "consensus/subsidy.h"
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

    UtreexoForest expected_after = initial_forest.cloneForHeight(spend_height);
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
    initial_forest = initial_forest.cloneForHeight(funding_height);
    TEST_ASSERT(initial_forest.add(funding_leaf) != UINT64_MAX, "Initial funding leaf add should succeed");

    const uint64_t parent_value = 49'99900000ULL;
    const uint64_t child_value = 49'99800000ULL;
    Block cpfp_block = makeCpfpBlock(
        spend_height, funding_txid, 0, parent_value, child_value, script, funding_block.GetHash());

    // ReplayBlock applies the height-specific canonical-roots transition before
    // mutating the forest; the independent expected model must do the same.
    UtreexoForest expected_after = initial_forest.cloneForHeight(spend_height);
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

    // Metadata covers every input, including the child, while the recovered
    // batch proof contains only the external funding leaf.
    BlockHeader repair_parent = funding_block.header;
    const auto repair_before = initial_forest.getCommitment();
    std::memcpy(repair_parent.utreexo_root.begin(), repair_before.data(), 32);
    Block repair_block = cpfp_block;
    repair_block.header.prev_block_hash = repair_parent.GetHash();
    UtreexoProofMessage repair_msg;
    repair_msg.block_hash = repair_block.GetHash();
    repair_msg.block_height = spend_height;
    repair_msg.accumulator_root_before = repair_before;
    repair_msg.accumulator_root_after = expected_after.getCommitment();
    repair_msg.proof_data.accumulator_root_before = repair_before;
    repair_msg.proof_data.spent_outputs = spent_outputs;
    auto& repair_batch = repair_msg.proof_data.spend_proof;
    repair_batch.targets = {funding_leaf};
    repair_batch.positions = {*initial_forest.findLeafPosition(funding_leaf)};
    repair_batch.proof_hashes = initial_forest.generateBatchProof(repair_batch.targets);
    repair_batch.numLeaves = initial_forest.getNumLeaves();
    repair_batch.format_version = GetUtreexoProofFormatVersion(spend_height);
    std::string repair_error;
    for (bool require_peer_proof : {false, true}) {
        TEST_ASSERT(StatelessNode::ValidateReplayMetadataRepair(
            repair_block, spend_height, repair_parent, {funding_leaf}, repair_msg,
            initial_forest, require_peer_proof, repair_error),
            "CPFP repair accepts two spent outputs and one external target: " + repair_error);
    }
    for (unsigned field = 0; field < 6; ++field) {
        auto wrong = repair_msg;
        auto& child = wrong.proof_data.spent_outputs[1];
        switch (field) {
            case 0: child.value++; break;
            case 1: child.scriptPubKey[0] ^= 1; break;
            case 2: child.created_height++; break;
            case 3: child.is_coinbase = true; break;
            case 4: child.is_confidential = true; break;
            case 5: child.commitment = {0x02}; break;
        }
        TEST_ASSERT(!StatelessNode::ValidateReplayMetadataRepair(
            repair_block, spend_height, repair_parent, {funding_leaf}, wrong,
            initial_forest, true, repair_error),
            "CPFP repair rejects unbound child metadata field " + std::to_string(field));
    }
    TEST_ASSERT(initial_forest.getCommitment() == repair_before,
                "CPFP repair success and failure leave caller forest unchanged");

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

    // Manufacture one una of fee in the child's supplied metadata and pay it
    // in coinbase. The real block's root and external-input proof still match:
    // the ephemeral parent output is never a forest target. Monetary accounting
    // alone would trust this lie unless metadata is bound before forest mutation.
    Block overpay = cpfp_block;
    const uint64_t subsidy = ConsensusSubsidy::GetBlockSubsidy(
        spend_height, Params().sixty_second_activation_height).GetUna();
    overpay.vtx[0].vout[0].value = AmountUna::Una(subsidy + funding_value - child_value + 1);
    UtreexoForest overpay_after = initial_forest.cloneForHeight(spend_height);
    const auto overpay_position = overpay_after.findLeafPosition(funding_leaf);
    const auto overpay_spend = overpay_after.prove(*overpay_position);
    TEST_ASSERT(overpay_spend.has_value() && overpay_after.remove(funding_leaf, *overpay_spend),
                "Forged CPFP fixture removes its real funding leaf");
    for (size_t tx_index : {size_t{0}, size_t{2}}) {
        const auto& tx = overpay.vtx[tx_index];
        TEST_ASSERT(overpay_after.add(HashUTXOForCreationHeight(
            tx.GetTxid().AsUint256(), 0, tx.vout[0].value.GetUna(),
            tx.vout[0].scriptPubKey, spend_height, tx.IsCoinbase())) != UINT64_MAX,
            "Forged CPFP fixture adds only surviving outputs");
    }
    setHeaderRootFromForest(overpay, overpay_after);
    auto forged_spent = spent_outputs;
    forged_spent[1].value = parent_value + 1;
    std::string accounting_error;
    TEST_ASSERT(CheckBlockRewardFromSpentOutputs(overpay, spend_height, &forged_spent, accounting_error),
                "Forged child metadata defeats unauthenticated fee arithmetic as intended by fixture");
    TEST_ASSERT(!StatelessNode::CheckReplayReward(
                    overpay, spend_height, {funding_leaf}, &forged_spent, accounting_error),
                "Reorg reward preflight rejects forged child fee metadata before rewind");
    TEST_ASSERT(StatelessNode::CheckReplayReward(
                    cpfp_block, spend_height, {funding_leaf}, &spent_outputs, accounting_error),
                "Reorg reward preflight accepts exact authentic CPFP metadata");
    UtreexoProofMessage msg;
    msg.block_hash = overpay.GetHash();
    msg.block_height = spend_height;
    msg.accumulator_root_before = initial_forest.getCommitment();
    msg.accumulator_root_after = overpay_after.getCommitment();
    msg.proof_data.spent_outputs = forged_spent;
    auto& batch = msg.proof_data.spend_proof;
    batch.targets = {funding_leaf};
    batch.positions = {*initial_forest.findLeafPosition(funding_leaf)};
    batch.proof_hashes = initial_forest.generateBatchProof(batch.targets);
    batch.numLeaves = initial_forest.getNumLeaves();
    batch.format_version = GetUtreexoProofFormatVersion(spend_height);
    UtreexoForest forward = initial_forest;
    StatelessNode forward_node(&forward);
    forward_node.SyncToForestState(funding_height);
    TEST_ASSERT(!forward_node.ValidateUtreexoProof(overpay, msg, 1),
                "Forward proof rejects manufactured fees from forged child metadata");
    TEST_ASSERT(forward.getCommitment() == initial_forest.getCommitment(),
                "Forged child fees cannot advance canonical forest");
    UtreexoForest scratch = initial_forest;
    TEST_ASSERT(!forward_node.ValidateProofIntoForest(overpay, msg, scratch),
                "Scratch proof rejects manufactured child fees");
    TEST_ASSERT(scratch.getCommitment() == initial_forest.getCommitment(),
                "Forged child fees cannot advance scratch forest");
    const auto tp = UtreexoTransitionProof::generate(initial_forest, overpay, batch, spend_height);
    TEST_ASSERT(tp.verify() && tp.commitment_after == overpay_after.getCommitment(),
                "Forged-fee fixture still carries a valid transition proof");
    UtreexoForest tp_forest = initial_forest;
    StatelessNode tp_node(&tp_forest);
    tp_node.SyncToForestState(funding_height);
    TEST_ASSERT(!tp_node.ValidateWithTransitionProof(overpay, msg, tp, 1),
                "Transition proof rejects manufactured child fees before advancing stump");
    TEST_ASSERT(tp_forest.getCommitment() == initial_forest.getCommitment(),
                "Forged child fees cannot advance transition forest");
}

void applyRewardFixtureAdditions(UtreexoForest& forest, const Block& block, uint32_t height) {
    for (const auto& tx : block.vtx) {
        const auto txid = tx.GetTxid();
        for (uint32_t i = 0; i < tx.vout.size(); ++i) {
            const auto& output = tx.vout[i];
            TEST_ASSERT(forest.add(HashUTXOForCreationHeight(
                txid.AsUint256(), i, output.value.GetUna(), output.scriptPubKey,
                height, tx.IsCoinbase())) != UINT64_MAX, "Reward fixture addition");
        }
    }
}

// These fixtures deliberately have a correct accumulator transition. Rejecting
// an overpay therefore has to come from monetary validation, not a bad root.
void test_replay_reward(uint64_t excess, bool split, bool omit_metadata,
                        bool legacy_leaf = false, uint32_t spend_height_override = 0) {
    std::cout << "CSN reward replay excess=" << excess << " split=" << split
              << " omit_metadata=" << omit_metadata << " legacy=" << legacy_leaf << std::endl;
    const uint32_t funding_height = legacy_leaf ? 1 : GetUtreexoMaturityLeafActivationHeight();
    const uint32_t spend_height = spend_height_override != 0
        ? spend_height_override : (legacy_leaf ? 2 : funding_height + 100);
    const auto script = makeScript();
    constexpr uint64_t funding_value = 50'00000000ULL;
    constexpr uint64_t fee = 10'000000ULL; // 0.1 DIN, above the old admission estimate.
    const uint64_t subsidy = ConsensusSubsidy::GetBlockSubsidy(
        spend_height, Params().sixty_second_activation_height).GetUna();
    Block funding = makeCoinbaseBlock(funding_height,
        {{funding_value, script}, {funding_value, script}});
    const TxId funding_txid = funding.vtx[0].GetTxid();
    // A legacy fixture is a non-coinbase output so no maturity assumption is
    // needed; the post-activation fixture spends an exactly mature coinbase.
    const bool is_coinbase = !legacy_leaf;
    const auto leaf = HashUTXOForCreationHeight(
        funding_txid.AsUint256(), 0, funding_value, script, funding_height, is_coinbase);
    UtreexoForest initial;
    initial = initial.cloneForHeight(funding_height);
    TEST_ASSERT(initial.add(leaf) != UINT64_MAX, "Reward fixture funding add");
    const auto sibling_leaf = HashUTXOForCreationHeight(
        funding_txid.AsUint256(), 1, funding_value, script, funding_height, is_coinbase);
    TEST_ASSERT(initial.add(sibling_leaf) != UINT64_MAX,
                "Reward fixture retains a sibling requiring a nonempty batch proof");
    Block block = makeSpendBlock(spend_height, funding_txid, 0,
                                 funding_value - fee, script, funding.GetHash());
    block.vtx[0].vout[0].value = AmountUna::Una(split ? subsidy : subsidy + fee + excess);
    if (split) {
        TxOutput second;
        second.value = AmountUna::Una(fee + excess);
        second.scriptPubKey = script;
        block.vtx[0].vout.push_back(second);
    }
    UtreexoForest expected = initial.cloneForHeight(spend_height);
    const auto position = expected.findLeafPosition(leaf);
    TEST_ASSERT(position.has_value(), "Reward fixture funding position");
    const auto proof = expected.prove(*position);
    TEST_ASSERT(proof.has_value() && expected.remove(leaf, *proof), "Reward fixture spend");
    applyRewardFixtureAdditions(expected, block, spend_height);
    setHeaderRootFromForest(block, expected);
    std::vector<SpentOutputData> spent{
        SpentOutputData(funding_value, script, funding_height, is_coinbase)};
    UtreexoForest actual = initial;
    StatelessNode node(&actual);
    node.SyncToForestState(spend_height - 1);
    const bool accepted = node.ReplayBlock(block, spend_height, {leaf},
                                           omit_metadata ? nullptr : &spent);
    const bool should_accept = excess == 0 && !omit_metadata &&
        (!is_coinbase || spend_height >= funding_height + 100);
    TEST_ASSERT(accepted == should_accept,
                "Root-correct replay must enforce exact reward across all coinbase outputs and require fee metadata");
    TEST_ASSERT(actual.getCommitment() == (should_accept ? expected : initial).getCommitment(),
                "Rejected reward replay must preserve forest; accepted replay must match expected root");

    if (!omit_metadata) {
        UtreexoProofMessage msg;
        msg.block_hash = block.GetHash();
        msg.block_height = spend_height;
        msg.accumulator_root_before = initial.getCommitment();
        msg.accumulator_root_after = expected.getCommitment();
        msg.proof_data.spent_outputs = spent;
        auto& batch = msg.proof_data.spend_proof;
        batch.targets = {leaf};
        batch.positions = {*initial.findLeafPosition(leaf)};
        batch.proof_hashes = initial.generateBatchProof(batch.targets);
        batch.numLeaves = initial.getNumLeaves();
        batch.format_version = GetUtreexoProofFormatVersion(spend_height);

        // Recovery binds untrusted metadata to LOCAL block/parent identities,
        // preserves the old target sequence, and never touches the live forest.
        BlockHeader parent_header = funding.header;
        const auto parent_root = initial.getCommitment();
        std::memcpy(parent_header.utreexo_root.begin(), parent_root.data(), 32);
        Block repair_block = block;
        repair_block.header.prev_block_hash = parent_header.GetHash();
        auto repair_msg = msg;
        repair_msg.block_hash = repair_block.GetHash();
        repair_msg.proof_data.accumulator_root_before = parent_root;
        std::string repair_error;
        TEST_ASSERT(StatelessNode::ValidateReplayMetadataRepair(
            repair_block, spend_height, parent_header, {leaf}, repair_msg,
            initial, true, repair_error) == should_accept,
            "Recovered metadata repair validates exact reward on scratch");
        TEST_ASSERT(StatelessNode::ValidateReplayMetadataRepair(
            repair_block, spend_height, parent_header, {leaf}, repair_msg,
            initial, false, repair_error) == should_accept,
            "Locally recovered undo metadata validates on scratch");
        TEST_ASSERT(initial.getCommitment() == parent_root,
                    "Metadata repair must not advance caller forest");
        if (should_accept) {
            TEST_ASSERT(!repair_msg.proof_data.spend_proof.proof_hashes.empty(),
                        "Two-leaf repair fixture cryptographically requires a sibling hash");
            auto local_undo = repair_msg;
            local_undo.proof_data.spend_proof.positions.clear();
            local_undo.proof_data.spend_proof.proof_hashes.clear();
            TEST_ASSERT(StatelessNode::ValidateReplayMetadataRepair(
                repair_block, spend_height, parent_header, {leaf}, local_undo,
                initial, false, repair_error),
                "Trusted local undo metadata needs no supplied batch proof");
            TEST_ASSERT(!StatelessNode::ValidateReplayMetadataRepair(
                repair_block, spend_height, parent_header, {leaf}, local_undo,
                initial, true, repair_error),
                "Peer repair cannot omit batch positions and sibling hashes");

            auto wrong = repair_msg;
            wrong.block_height++;
            TEST_ASSERT(!StatelessNode::ValidateReplayMetadataRepair(
                repair_block, spend_height, parent_header, {leaf}, wrong,
                initial, true, repair_error), "Repair rejects peer height mismatch");
            wrong = repair_msg;
            wrong.accumulator_root_after[0] ^= 1;
            TEST_ASSERT(!StatelessNode::ValidateReplayMetadataRepair(
                repair_block, spend_height, parent_header, {leaf}, wrong,
                initial, true, repair_error), "Repair binds root-after to stored header");
            wrong = repair_msg;
            wrong.proof_data.spent_outputs[0].value++;
            TEST_ASSERT(!StatelessNode::ValidateReplayMetadataRepair(
                repair_block, spend_height, parent_header, {leaf}, wrong,
                initial, true, repair_error), "Repair rejects peer amount lie");
            wrong = repair_msg;
            wrong.proof_data.spent_outputs.clear();
            TEST_ASSERT(!StatelessNode::ValidateReplayMetadataRepair(
                repair_block, spend_height, parent_header, {leaf}, wrong,
                initial, true, repair_error), "Repair refuses proof without metadata");
            auto wrong_targets = std::vector<UtreexoHash>{leaf};
            wrong_targets[0][0] ^= 1;
            TEST_ASSERT(!StatelessNode::ValidateReplayMetadataRepair(
                repair_block, spend_height, parent_header, wrong_targets, repair_msg,
                initial, true, repair_error), "Repair cannot replace original target identities");
            wrong = repair_msg;
            wrong.proof_data.spend_proof.positions[0]++;
            TEST_ASSERT(!StatelessNode::ValidateReplayMetadataRepair(
                repair_block, spend_height, parent_header, {leaf}, wrong,
                initial, true, repair_error), "Repair requires valid batch proof");
            wrong = repair_msg;
            wrong.proof_data.spend_proof.proof_hashes[0][0] ^= 1;
            TEST_ASSERT(!StatelessNode::ValidateReplayMetadataRepair(
                repair_block, spend_height, parent_header, {leaf}, wrong,
                initial, true, repair_error), "Repair rejects corrupted sibling hash");

            wrong = repair_msg;
            wrong.block_hash.begin()[0] ^= 1;
            TEST_ASSERT(!StatelessNode::ValidateReplayMetadataRepair(
                repair_block, spend_height, parent_header, {leaf}, wrong,
                initial, true, repair_error), "Repair binds response to exact stored block hash");
            BlockHeader wrong_parent = parent_header;
            wrong_parent.nonce++;
            TEST_ASSERT(!StatelessNode::ValidateReplayMetadataRepair(
                repair_block, spend_height, wrong_parent, {leaf}, repair_msg,
                initial, true, repair_error), "Repair binds local parent identity");
            wrong = repair_msg;
            wrong.accumulator_root_before[0] ^= 1;
            TEST_ASSERT(!StatelessNode::ValidateReplayMetadataRepair(
                repair_block, spend_height, parent_header, {leaf}, wrong,
                initial, true, repair_error), "Repair binds envelope before-root");
            wrong = repair_msg;
            wrong.proof_data.accumulator_root_before[0] ^= 1;
            TEST_ASSERT(!StatelessNode::ValidateReplayMetadataRepair(
                repair_block, spend_height, parent_header, {leaf}, wrong,
                initial, true, repair_error), "Repair binds payload before-root");
            UtreexoForest wrong_scratch = initial;
            TEST_ASSERT(wrong_scratch.add(UtreexoHash(32, 0xa5)) != UINT64_MAX,
                        "Wrong-history fixture changes the parent forest");
            TEST_ASSERT(!StatelessNode::ValidateReplayMetadataRepair(
                repair_block, spend_height, parent_header, {leaf}, repair_msg,
                wrong_scratch, true, repair_error), "Repair rejects wrong historical scratch root");

            wrong = repair_msg;
            wrong.proof_data.spend_proof.numLeaves++;
            TEST_ASSERT(!StatelessNode::ValidateReplayMetadataRepair(
                repair_block, spend_height, parent_header, {leaf}, wrong,
                initial, true, repair_error), "Peer repair binds declared leaf count");
            for (uint8_t version : {0, 3, 7, 255}) {
                wrong = repair_msg;
                wrong.proof_data.spend_proof.format_version = version;
                TEST_ASSERT(!StatelessNode::ValidateReplayMetadataRepair(
                    repair_block, spend_height, parent_header, {leaf}, wrong,
                    initial, true, repair_error), "Peer repair rejects unsupported metadata format");
            }
            if (!legacy_leaf) {
                wrong = repair_msg;
                wrong.proof_data.spent_outputs[0].is_coinbase = false;
                TEST_ASSERT(!StatelessNode::ValidateReplayMetadataRepair(
                    repair_block, spend_height, parent_header, {leaf}, wrong,
                    initial, true, repair_error), "Repair binds v2 coinbase flag to the target");
                wrong = repair_msg;
                wrong.proof_data.spent_outputs[0].created_height = funding_height - 1;
                TEST_ASSERT(!StatelessNode::ValidateReplayMetadataRepair(
                    repair_block, spend_height, parent_header, {leaf}, wrong,
                    initial, true, repair_error), "Repair rejects v2 height downgrade to a legacy leaf");
            }
        }

        UtreexoForest forward = initial;
        StatelessNode forward_node(&forward);
        forward_node.SyncToForestState(spend_height - 1);
        TEST_ASSERT(forward_node.ValidateUtreexoProof(block, msg, 1) == should_accept,
                    "Forward batch proof must enforce exact reward before canonical mutation");
        TEST_ASSERT(forward.getCommitment() == (should_accept ? expected : initial).getCommitment(),
                    "Forward overpay must leave canonical forest unchanged");

        UtreexoForest scratch = initial;
        TEST_ASSERT(forward_node.ValidateProofIntoForest(block, msg, scratch) == should_accept,
                    "Speculative batch proof must enforce exact reward before scratch mutation");
        TEST_ASSERT(scratch.getCommitment() == (should_accept ? expected : initial).getCommitment(),
                    "Speculative overpay must leave scratch forest unchanged");

        const auto transition = UtreexoTransitionProof::generate(initial, block, batch, spend_height);
        TEST_ASSERT(transition.verify() && transition.commitment_after == expected.getCommitment(),
                    "Reward fixture has an independently valid transition proof");
        UtreexoForest tp_forest = initial;
        StatelessNode tp_node(&tp_forest);
        tp_node.SyncToForestState(spend_height - 1);
        TEST_ASSERT(tp_node.ValidateWithTransitionProof(block, msg, transition, 1) == should_accept,
                    "Transition proof must enforce exact reward before stump mutation");
        if (!should_accept) {
            TEST_ASSERT(tp_forest.getCommitment() == initial.getCommitment(),
                        "Rejected transition proof must preserve canonical forest");
        }
    }
}

void test_coinbase_only_replay_reward(uint64_t excess) {
    const uint32_t height = GetUtreexoMaturityLeafActivationHeight();
    const uint64_t subsidy = ConsensusSubsidy::GetBlockSubsidy(
        height, Params().sixty_second_activation_height).GetUna();
    Block block = makeCoinbaseBlock(height, {{subsidy + excess, makeScript()}});
    UtreexoForest initial;
    UtreexoForest expected = initial.cloneForHeight(height);
    applyRewardFixtureAdditions(expected, block, height);
    setHeaderRootFromForest(block, expected);
    UtreexoForest actual = initial;
    StatelessNode node(&actual);
    node.SyncToForestState(height - 1);
    const bool accepted = node.ReplayBlock(block, height, {});
    TEST_ASSERT(accepted == (excess == 0), "Coinbase-only replay checks subsidy without requiring spent metadata");
    TEST_ASSERT(actual.getCommitment() == (excess == 0 ? expected : initial).getCommitment(),
                "Coinbase-only overpay must preserve the forest");
}

void test_reward_accounting_cardinality_and_overflow() {
    std::cout << "Pure replay reward accounting cardinality and overflow..." << std::endl;
    const uint32_t height = 120;
    const auto script = makeScript();
    const uint64_t subsidy = ConsensusSubsidy::GetBlockSubsidy(
        height, Params().sixty_second_activation_height).GetUna();
    constexpr uint64_t fee = 10'000000ULL;
    Block funding = makeCoinbaseBlock(20, {{50'00000000ULL, script}});
    Block block = makeSpendBlock(height, funding.vtx[0].GetTxid(), 0,
                                 50'00000000ULL - fee, script);
    block.vtx[0].vout[0].value = AmountUna::Una(subsidy + fee);
    std::vector<SpentOutputData> spent{SpentOutputData(50'00000000ULL, script, 20, true)};
    auto rejects = [&](const Block& candidate, const std::vector<SpentOutputData>* metadata,
                       const char* expected) {
        std::string error;
        TEST_ASSERT(!CheckBlockRewardFromSpentOutputs(candidate, height, metadata, error),
                    std::string("Reward accounting rejects ") + expected);
        TEST_ASSERT(error == expected, "Reward accounting rejects for the intended reason: " + error);
    };
    std::string error;
    TEST_ASSERT(CheckBlockRewardFromSpentOutputs(block, height, &spent, error),
                "Pure accounting accepts exact transparent fee");
    rejects(block, nullptr, "block-reward-missing-spent-outputs");
    std::vector<SpentOutputData> empty;
    rejects(block, &empty, "block-reward-spent-outputs-underrun");
    auto surplus = spent;
    surplus.push_back(spent.front());
    rejects(block, &surplus, "block-reward-spent-outputs-surplus");

    Block input_overflow = block;
    input_overflow.vtx[1].vin.push_back(input_overflow.vtx[1].vin.front());
    auto oversized = surplus;
    oversized[0].value = UINT64_MAX;
    oversized[1].value = 1;
    rejects(input_overflow, &oversized, "block-reward-input-total-overflow");

    Block output_overflow = block;
    output_overflow.vtx[1].vout[0].value = AmountUna::UnsafeFromRaw(UINT64_MAX);
    TxOutput extra;
    extra.value = AmountUna::Una(1);
    extra.scriptPubKey = {0x6a}; // unspendable outputs still count in reward sums
    output_overflow.vtx[1].vout.push_back(extra);
    rejects(output_overflow, &spent, "block-reward-output-total-overflow");

    // Only accounting is under test here: proof/bundle verification remains a
    // separate required gate. Zero-input shielded fees do not need UTXO metadata.
    Block shielded = makeCoinbaseBlock(height, {{subsidy + fee, script}});
    Transaction unshield;
    unshield.version = Transaction::TX_VERSION_SHIELDED_V2;
    unshield.SetExplicitFee(fee);
    TxOutput payout;
    payout.value = AmountUna::Una(5'00000000ULL);
    payout.scriptPubKey = script;
    unshield.vout.push_back(payout);
    shielded.vtx.push_back(unshield);
    TEST_ASSERT(CheckBlockRewardFromSpentOutputs(shielded, height, nullptr, error),
                "Zero-input shielded explicit fee needs no spent metadata for accounting");
    shielded.vtx[1].SetExplicitFee(UINT64_MAX);
    rejects(shielded, nullptr, "block-reward-subsidy-fee-overflow");
    unshield.SetExplicitFee(1);
    shielded.vtx.push_back(unshield);
    rejects(shielded, nullptr, "block-reward-fee-total-overflow");

    Block coinbase_overflow = makeCoinbaseBlock(height, {{UINT64_MAX, script}, {1, {0x6a}}});
    rejects(coinbase_overflow, nullptr, "block-reward-output-total-overflow");
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
    test_replay_reward(0, false, false);
    test_replay_reward(1, false, false);
    test_replay_reward(1, true, false);
    test_replay_reward(0, false, true, true);
    test_replay_reward(0, false, false, true);
    test_replay_reward(0, false, false, true,
                       GetUtreexoMaturityLeafActivationHeight() + 100);
    test_replay_reward(0, false, false, false,
                       GetUtreexoMaturityLeafActivationHeight() + 99);
    test_coinbase_only_replay_reward(0);
    test_coinbase_only_replay_reward(1);
    test_reward_accounting_cardinality_and_overflow();
    std::cout << "RESULT: " << tests_passed << "/" << tests_total << " assertions" << std::endl;
    return tests_passed == tests_total ? 0 : 1;
}
