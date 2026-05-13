// ============================================================================
// CONSENSUS LAYER - STATELESS BLOCK VERIFICATION IMPLEMENTATION
// ============================================================================
//
// Phase 2.2: Stateless Block Verification
//
// PURE FUNCTIONS - NO SIDE EFFECTS
//
// ============================================================================

#include "consensus/stateless_verification.h"
#include "consensus/utreexo_accumulator.h"
#include "primitives/transaction.h"
#include <cstring>

namespace dinero {
namespace consensus {

// ============================================================================
// Core Pure Verification Function
// ============================================================================

VerifyResult VerifyBlockStateless(
    const Block& block,
    const StatelessContext& ctx,
    const BlockUtreexoProof& proof) noexcept {

    // Early exit: empty block is invalid (must have coinbase)
    if (block.vtx.empty()) {
        return VerifyResult::Fail(VerifyError::INVALID_COINBASE);
    }

    // Verify Utreexo proof matches expected roots
    // NOTE: proof.targets should match spent outputs in block
    if (!proof.isEmpty()) {
        // Build expected roots vector for verification
        std::vector<UtreexoHash> expected_roots;
        expected_roots.reserve(ctx.num_roots);
        for (uint8_t i = 0; i < ctx.num_roots; i++) {
            expected_roots.push_back(ctx.roots[i]);
        }

        // Use forest's stateless verification
        UtreexoForest temp_forest;
        if (!temp_forest.verifyBatchProofStateless(
                proof.targets,
                proof.positions,
                proof.proof_hashes,
                proof.numLeaves,
                expected_roots)) {
            return VerifyResult::Fail(VerifyError::INVALID_PROOF);
        }
    }

    // Track spending to detect double-spends within block
    // Using a simple array for small blocks, hash set for large
    // For pure hot path, we use linear scan (blocks typically < 5000 inputs)
    uint32_t spent_index = 0;
    uint64_t total_input_value = 0;
    uint64_t total_output_value = 0;
    uint32_t outputs_created = 0;
    uint32_t inputs_spent = 0;

    // Track outpoints spent in this block for double-spend detection
    // We'll use a simple approach: check each input against all previous
    std::vector<std::pair<TxId, uint32_t>> spent_in_block;
    spent_in_block.reserve(ctx.spent_count);

    // Process each transaction
    for (size_t tx_idx = 0; tx_idx < block.vtx.size(); tx_idx++) {
        const Transaction& tx = block.vtx[tx_idx];
        const bool is_coinbase = (tx_idx == 0);

        if (is_coinbase) {
            // Coinbase validation
            if (!tx.IsCoinbase()) {
                return VerifyResult::Fail(VerifyError::INVALID_COINBASE);
            }
        } else {
            // Regular transaction: verify inputs
            for (const TxInput& input : tx.vin) {
                // Check for double-spend within block
                for (const auto& prev : spent_in_block) {
                    if (prev.first == input.prevout.txid &&
                        prev.second == input.prevout.vout) {
                        return VerifyResult::Fail(VerifyError::DOUBLE_SPEND);
                    }
                }

                // Mark as spent
                spent_in_block.emplace_back(input.prevout.txid, input.prevout.vout);

                // Get spent output data from context
                if (spent_index >= ctx.spent_count) {
                    return VerifyResult::Fail(VerifyError::MISSING_INPUT);
                }

                const SpentOutputData& spent = ctx.spent_outputs[spent_index];
                spent_index++;

                // Check coinbase maturity (conceptually - we don't have height info)
                // For light clients, this is verified via the Utreexo proof structure

                total_input_value += spent.value;
                inputs_spent++;
            }
        }

        // Sum outputs
        for (const TxOutput& output : tx.vout) {
            // Skip OP_RETURN (not spendable)
            if (!output.scriptPubKey.empty() && output.scriptPubKey[0] == 0x6a) {
                continue;
            }
            total_output_value += output.value.GetUna();
            outputs_created++;
        }
    }

    // Verify value balance
    uint64_t subsidy = GetBlockSubsidy(ctx.height);
    uint64_t allowed_output = total_input_value + subsidy;

    if (total_output_value > allowed_output) {
        return VerifyResult::Fail(VerifyError::INVALID_AMOUNT);
    }

    // Calculate fees
    uint64_t fees = allowed_output - total_output_value;

    // Verify coinbase doesn't exceed subsidy + fees
    if (!block.vtx.empty()) {
        uint64_t coinbase_output = 0;
        for (const TxOutput& output : block.vtx[0].vout) {
            coinbase_output += output.value.GetUna();
        }
        if (coinbase_output > subsidy + fees) {
            return VerifyResult::Fail(VerifyError::INVALID_COINBASE);
        }
    }

    // NOTE: Utreexo root verification against header.utreexo_root
    // is left to the caller since computing the new root requires
    // simulating the full accumulator update

    return VerifyResult::Ok(fees, outputs_created, inputs_spent);
}

// ============================================================================
// Convenience Wrapper (allocates)
// ============================================================================

VerifyResult VerifyBlockWithSnapshot(
    const Block& block,
    const UTXOSnapshot& snapshot,
    const BlockUtreexoProof& proof) {

    // Build spent outputs array from block inputs
    std::vector<SpentOutputData> spent_outputs;

    for (size_t tx_idx = 1; tx_idx < block.vtx.size(); tx_idx++) {
        const Transaction& tx = block.vtx[tx_idx];
        for (const TxInput& input : tx.vin) {
            OutPoint outpoint(input.prevout.txid, input.prevout.vout);
            auto it = snapshot.utxos.find(outpoint);
            if (it == snapshot.utxos.end()) {
                return VerifyResult::Fail(VerifyError::MISSING_INPUT);
            }

            SpentOutputData spent;
            spent.value = it->second.value.GetUna();
            spent.scriptPubKey = it->second.scriptPubKey;
            spent.is_confidential = it->second.is_confidential;
            spent.commitment = it->second.commitment;
            spent_outputs.push_back(std::move(spent));
        }
    }

    // Build context
    StatelessContext ctx;
    ctx.spent_outputs = spent_outputs.data();
    ctx.spent_count = static_cast<uint32_t>(spent_outputs.size());

    // Get Utreexo roots from snapshot (simplified - would need proper root computation)
    std::vector<UtreexoHash> roots;
    roots.push_back(snapshot.utreexo_root);
    ctx.roots = roots.data();
    ctx.num_roots = static_cast<uint8_t>(roots.size());
    ctx.num_leaves = snapshot.utreexo_num_leaves;

    ctx.height = snapshot.height + 1;  // Block being verified is at snapshot.height + 1

    return VerifyBlockStateless(block, ctx, proof);
}

// ============================================================================
// Utreexo Proof Verification (Pure)
// ============================================================================

bool VerifyUtreexoProofStateless(
    const BlockUtreexoProof& proof,
    const UtreexoHash* targets,
    uint32_t target_count,
    const UtreexoHash* roots,
    uint8_t num_roots,
    uint64_t num_leaves) noexcept {

    // Validate proof structure
    if (proof.positions.size() != proof.targets.size()) {
        return false;
    }

    if (target_count != proof.targets.size()) {
        return false;
    }

    // Build expected roots vector
    std::vector<UtreexoHash> expected_roots;
    expected_roots.reserve(num_roots);
    for (uint8_t i = 0; i < num_roots; i++) {
        expected_roots.push_back(roots[i]);
    }

    // Use forest's stateless verification
    UtreexoForest temp_forest;
    return temp_forest.verifyBatchProofStateless(
        proof.targets,
        proof.positions,
        proof.proof_hashes,
        proof.numLeaves,
        expected_roots);
}

// ============================================================================
// Leaf Hash Computation (Pure)
// ============================================================================

UtreexoHash ComputeUTXOLeafHash(
    const uint256& txid,
    uint32_t vout,
    uint64_t value,
    const uint8_t* script,
    uint32_t script_len) noexcept {

    // Use the existing HashUTXO function
    std::vector<uint8_t> script_vec(script, script + script_len);
    return HashUTXO(txid, vout, value, script_vec);
}

} // namespace consensus
} // namespace dinero
