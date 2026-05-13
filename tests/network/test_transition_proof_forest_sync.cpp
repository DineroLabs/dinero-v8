#include "network/stateless_node.h"
#include "network/utreexo_messages.h"
#include "consensus/utreexo_accumulator.h"
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

}  // namespace

int main() {
    test_transition_proof_keeps_empty_forest_aligned();
    test_transition_proof_keeps_forest_aligned_for_next_batch_block();
    std::cout << "PASS: " << tests_passed << "/" << tests_total << " assertions" << std::endl;
    return 0;
}
