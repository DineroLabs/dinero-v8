// Copyright (c) 2026 The Dinero Core developers
// Distributed under the MIT software license

#pragma once

#include "lightning/utreexo/lightning_utreexo_client.h"
#include "primitives/uint256.h"
#include "primitives/block.h"
#include <memory>
#include <string>

namespace lightning {
namespace utreexo {

/**
 * ChannelProofValidator - Verify channel funding using Utreexo proofs
 *
 * Phase 11.2: Validates Lightning channel funding transactions
 *
 * Purpose: Ensure channel funding UTXOs exist on-chain before accepting
 * channel open requests.
 *
 * Key Operations:
 * - Validate funding proof against Utreexo accumulator
 * - Verify funding amount matches expected value
 * - Check minimum confirmation requirements
 *
 * Integration: Called by ChannelManager::OpenChannel() before accepting funding
 *
 * Usage:
 *   ChannelProofValidator validator(utreexo_client);
 *   auto result = validator.ValidateFundingProof(txid, vout, amount, proof, root);
 *   if (result.valid) {
 *       // Accept channel funding
 *   }
 */
class ChannelProofValidator {
public:
    /**
     * Validation result
     */
    struct ValidationResult {
        bool valid = false;
        std::string error;
        uint32_t confirmations = 0;

        ValidationResult() = default;
        ValidationResult(bool v) : valid(v) {}
        ValidationResult(bool v, const std::string& e) : valid(v), error(e) {}
    };

    /**
     * Construct validator with Utreexo client
     * @param utreexo_client Client for querying proofs
     */
    explicit ChannelProofValidator(
        std::shared_ptr<LightningUtreexoClient> utreexo_client
    );

    /**
     * Validate funding proof
     *
     * Verifies that:
     * 1. Proof is valid against Utreexo root
     * 2. Funding output exists at specified vout
     * 3. Funding amount matches expected value
     *
     * @param funding_txid Funding transaction ID
     * @param funding_vout Funding output index
     * @param expected_amount Expected funding amount (una)
     * @param proof Utreexo proof data
     * @param utreexo_root Expected Utreexo accumulator root
     * @return Validation result with error message if invalid
     */
    ValidationResult ValidateFundingProof(
        const dinero::uint256& funding_txid,
        uint32_t funding_vout,
        uint64_t expected_amount,
        const dinero::consensus::BlockUtreexoData& proof,
        const dinero::uint256& utreexo_root
    );

    /**
     * Check minimum confirmations
     *
     * Note: This is a simplified implementation that assumes the proof
     * existence implies confirmation. In a full implementation, this would
     * query block height and calculate actual confirmations.
     *
     * @param funding_txid Funding transaction ID
     * @param min_confirmations Required minimum confirmations
     * @return True if funding has >= min_confirmations
     */
    bool HasMinimumConfirmations(
        const dinero::uint256& funding_txid,
        uint32_t min_confirmations
    );

    /**
     * Get validation statistics
     */
    struct Stats {
        uint64_t validations_total = 0;
        uint64_t validations_success = 0;
        uint64_t validations_failed = 0;
        uint64_t invalid_proofs = 0;
        uint64_t amount_mismatches = 0;
    };
    Stats GetStats() const;

private:
    // Utreexo client for proof queries
    std::shared_ptr<LightningUtreexoClient> utreexo_client_;

    // Statistics
    Stats stats_;

    mutable std::mutex mutex_;

    // Helper: Validate proof structure
    bool ValidateProofStructure(const dinero::consensus::BlockUtreexoData& proof);

    // Helper: Verify accumulator root
    bool VerifyAccumulatorRoot(
        const dinero::consensus::BlockUtreexoData& proof,
        const dinero::uint256& expected_root
    );

    // Helper: Validate funding amount (simplified for testing)
    bool ValidateFundingAmount(
        const dinero::uint256& funding_txid,
        uint32_t funding_vout,
        uint64_t expected_amount
    );
};

} // namespace utreexo
} // namespace lightning
