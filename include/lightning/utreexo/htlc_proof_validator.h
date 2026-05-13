// Copyright (c) 2026 The Dinero Core developers
// Distributed under the MIT software license

#pragma once

#include "lightning/utreexo/lightning_utreexo_client.h"
#include "primitives/uint256.h"
#include "primitives/block.h"
#include <memory>
#include <string>
#include <vector>

namespace lightning {
namespace utreexo {

/**
 * HTLCProofValidator - Verify HTLC settlements using Utreexo proofs
 *
 * Phase 11.3: Validates Lightning HTLC (Hash Time Locked Contract) settlements
 *
 * Purpose: Ensure HTLC outputs exist on-chain before settling payments.
 *
 * Key Operations:
 * - Validate HTLC settlement proof against Utreexo accumulator
 * - Verify preimage matches HTLC hash
 * - Check timeout eligibility for HTLC claims
 *
 * Integration: Called by HTLCManager::SettleHTLC() before claiming payment
 *
 * Usage:
 *   HTLCProofValidator validator(utreexo_client);
 *   auto result = validator.ValidateHTLCSettlement(txid, vout, preimage, proof, root);
 *   if (result.valid && result.preimage_match) {
 *       // Settle HTLC
 *   }
 */
class HTLCProofValidator {
public:
    /**
     * HTLC validation result
     */
    struct HTLCValidationResult {
        bool valid = false;
        std::string error;
        bool timeout_eligible = false;
        bool preimage_match = false;

        HTLCValidationResult() = default;
        HTLCValidationResult(bool v) : valid(v) {}
        HTLCValidationResult(bool v, const std::string& e) : valid(v), error(e) {}
    };

    /**
     * Construct validator with Utreexo client
     * @param utreexo_client Client for querying proofs
     */
    explicit HTLCProofValidator(
        std::shared_ptr<LightningUtreexoClient> utreexo_client
    );

    /**
     * Validate HTLC settlement
     *
     * Verifies that:
     * 1. Proof is valid against Utreexo root
     * 2. HTLC output exists at specified vout
     * 3. Preimage matches HTLC hash (if provided)
     *
     * @param htlc_txid HTLC transaction ID
     * @param htlc_vout HTLC output index
     * @param preimage Preimage for hash-locked payment (empty for timeout)
     * @param proof Utreexo proof data
     * @param utreexo_root Expected Utreexo accumulator root
     * @return Validation result with preimage match status
     */
    HTLCValidationResult ValidateHTLCSettlement(
        const dinero::uint256& htlc_txid,
        uint32_t htlc_vout,
        const std::vector<uint8_t>& preimage,
        const dinero::consensus::BlockUtreexoData& proof,
        const dinero::uint256& utreexo_root
    );

    /**
     * Check timeout eligibility
     *
     * Determines if an HTLC can be claimed via timeout path.
     *
     * Note: This is a simplified implementation for Phase 11.
     * In a full implementation, this would:
     * 1. Parse HTLC script to extract timeout parameters
     * 2. Compare current height/time against timeout
     * 3. Verify timeout conditions are met
     *
     * @param htlc_script HTLC script bytes
     * @param current_height Current blockchain height
     * @param current_time Current block time
     * @return True if HTLC timeout has been reached
     */
    bool IsTimeoutEligible(
        const std::vector<uint8_t>& htlc_script,
        uint32_t current_height,
        uint64_t current_time
    );

    /**
     * Get validation statistics
     */
    struct Stats {
        uint64_t validations_total = 0;
        uint64_t validations_success = 0;
        uint64_t validations_failed = 0;
        uint64_t invalid_proofs = 0;
        uint64_t preimage_mismatches = 0;
        uint64_t timeout_claims = 0;
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

    // Helper: Verify preimage matches HTLC hash
    bool VerifyPreimage(
        const std::vector<uint8_t>& preimage,
        const std::vector<uint8_t>& htlc_hash
    );

    // Helper: Extract HTLC hash from transaction output
    std::vector<uint8_t> ExtractHTLCHash(
        const dinero::uint256& htlc_txid,
        uint32_t htlc_vout
    );
};

} // namespace utreexo
} // namespace lightning
