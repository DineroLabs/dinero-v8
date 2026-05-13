/**
 * @file utreexo_proof_generator.cpp
 * @brief Phase 34.4: Utreexo Proof Generation Implementation
 *
 * Generates Utreexo proofs for block templates, enabling stateless validation.
 */

#include "consensus/utreexo_proof_generator.h"

namespace dinero {
namespace consensus {

// ═══════════════════════════════════════════════════════════════════════════
// Block Proof Generation
// ═══════════════════════════════════════════════════════════════════════════

BlockUtreexoProofs GenerateBlockProofs(
    const Block& block,
    const GlobalUTXOSet& utxo_set)
{
    auto result = GenerateBlockProofsWithResult(block, utxo_set);
    return std::move(result.proofs);
}

ProofGenerationResult GenerateBlockProofsWithResult(
    const Block& block,
    const GlobalUTXOSet& utxo_set)
{
    return GenerateTransactionProofs(block.vtx, block.GetHash(), utxo_set);
}

// ═══════════════════════════════════════════════════════════════════════════
// Transaction List Proof Generation
// ═══════════════════════════════════════════════════════════════════════════

ProofGenerationResult GenerateTransactionProofs(
    const std::vector<Transaction>& transactions,
    const std::string& block_hash,
    const GlobalUTXOSet& utxo_set)
{
    ProofGenerationResult result;
    result.proofs.version = UTREEXO_PROOF_VERSION;
    result.proofs.blockHash = block_hash;

    // Iterate through all transactions (skip coinbase at index 0)
    for (size_t tx_idx = 1; tx_idx < transactions.size(); ++tx_idx) {
        const auto& tx = transactions[tx_idx];

        // For each input in this transaction
        for (size_t in_idx = 0; in_idx < tx.vin.size(); ++in_idx) {
            const auto& txin = tx.vin[in_idx];

            // Get proof from GlobalUTXOSet
            auto proof_opt = utxo_set.getUtreexoProof(txin.prevout.txid, txin.prevout.vout);

            TxInProof txin_proof;
            txin_proof.outpoint = txin.prevout;

            if (proof_opt.has_value()) {
                txin_proof.proof = proof_opt.value();
                result.proofs_generated++;
            } else {
                // UTXO not found in accumulator
                result.proofs_failed++;
                result.success = false;
                if (result.error_message.empty()) {
                    result.error_message = "Missing proof for " + txin.prevout.txid +
                                          ":" + std::to_string(txin.prevout.vout);
                }
            }

            result.proofs.proofs.push_back(std::move(txin_proof));
        }
    }

    return result;
}

} // namespace consensus
} // namespace dinero
