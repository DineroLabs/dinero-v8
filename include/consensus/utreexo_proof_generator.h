#pragma once

/**
 * @file utreexo_proof_generator.h
 * @brief Phase 34.4: Utreexo Proof Generation for Block Templates
 *
 * This file provides functions to generate Utreexo proofs for block templates.
 * Separated from utreexo_proof_relay.h to avoid circular dependencies with
 * primitives/block.h.
 *
 * Usage:
 *   #include "consensus/utreexo_proof_generator.h"
 *   auto result = consensus::GenerateBlockProofs(block, utxo_set);
 */

#include "consensus/utreexo_proof_relay.h"
#include "primitives/block.h"
#include "consensus/global_utxo_set.h"

namespace dinero {
namespace consensus {

/**
 * @brief Result of proof generation
 */
struct ProofGenerationResult {
    bool success;
    BlockUtreexoProofs proofs;
    size_t proofs_generated;
    size_t proofs_failed;
    std::string error_message;

    ProofGenerationResult()
        : success(true), proofs_generated(0), proofs_failed(0) {}
};

/**
 * @brief Generate Utreexo proofs for all inputs in a block
 *
 * Iterates through all non-coinbase inputs and generates proofs
 * from the GlobalUTXOSet's Utreexo accumulator.
 *
 * @param block The block to generate proofs for
 * @param utxo_set The GlobalUTXOSet to query for proofs
 * @return BlockUtreexoProofs structure with all input proofs
 *
 * Note: If a UTXO is not found in the accumulator (shouldn't happen for
 * valid blocks), an empty proof is used for that input. Validators should
 * reject blocks with invalid/missing proofs.
 */
BlockUtreexoProofs GenerateBlockProofs(
    const Block& block,
    const GlobalUTXOSet& utxo_set);

/**
 * @brief Generate proofs with detailed result
 *
 * Same as GenerateBlockProofs but returns detailed statistics about
 * the generation process, including success/failure counts.
 */
ProofGenerationResult GenerateBlockProofsWithResult(
    const Block& block,
    const GlobalUTXOSet& utxo_set);

/**
 * @brief Generate proofs for a transaction list
 *
 * Lower-level function that generates proofs for a vector of transactions.
 * Useful when you have transactions but not a complete block structure.
 *
 * @param transactions Transaction vector (coinbase at index 0 is skipped)
 * @param block_hash Hash to associate with the proof batch
 * @param utxo_set The GlobalUTXOSet to query for proofs
 * @return ProofGenerationResult with detailed statistics
 */
ProofGenerationResult GenerateTransactionProofs(
    const std::vector<Transaction>& transactions,
    const std::string& block_hash,
    const GlobalUTXOSet& utxo_set);

} // namespace consensus
} // namespace dinero
