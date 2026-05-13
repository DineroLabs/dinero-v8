// Copyright (c) 2026 The Dinero Core developers
// Distributed under the MIT software license

#pragma once

#include "lightning/utreexo/lightning_utreexo_client.h"
#include "primitives/uint256.h"
#include "primitives/block.h"
#include <memory>
#include <string>
#include <vector>
#include <unordered_map>
#include <mutex>

namespace lightning {
namespace utreexo {

/**
 * WatchtowerProofMonitor - Stateless watchtower using Utreexo proofs
 *
 * Phase 11.4: Monitors Lightning channels for breaches without storing UTXO set
 *
 * Purpose: Enable stateless watchtowers that can detect and respond to
 * channel breaches using only Utreexo proofs + headers.
 *
 * Key Operations:
 * - Monitor channels for commitment transaction broadcasts
 * - Detect breaches (old commitment transactions)
 * - Verify commitment proofs against Utreexo accumulator
 * - Construct breach remedy transactions
 *
 * Integration: Used by watchtower services to monitor client channels
 *
 * Usage:
 *   WatchtowerProofMonitor monitor(utreexo_client);
 *   monitor.WatchChannel(channel_id, latest_commitment, revocation_secrets);
 *   auto result = monitor.MonitorChannel(channel_id);
 *   if (result.breach_detected) {
 *       // Broadcast breach remedy
 *   }
 */
class WatchtowerProofMonitor {
public:
    /**
     * Breach detection result
     */
    struct BreachDetectionResult {
        bool breach_detected = false;
        dinero::uint256 breach_txid;
        uint64_t commitment_number = 0;
        std::vector<uint8_t> remedy_tx;

        BreachDetectionResult() = default;
        BreachDetectionResult(bool detected) : breach_detected(detected) {}
    };

    /**
     * Channel monitoring data
     */
    struct ChannelMonitoringData {
        std::string channel_id;
        std::vector<uint8_t> latest_commitment;
        std::vector<std::vector<uint8_t>> revocation_secrets;
        uint64_t latest_commitment_number = 0;
    };

    /**
     * Construct monitor with Utreexo client
     * @param utreexo_client Client for querying proofs
     */
    explicit WatchtowerProofMonitor(
        std::shared_ptr<LightningUtreexoClient> utreexo_client
    );

    /**
     * Add channel to watch list
     *
     * @param channel_id Unique channel identifier
     * @param latest_commitment Latest commitment transaction bytes
     * @param revocation_secrets Revocation secrets for old commitments
     */
    void WatchChannel(
        const std::string& channel_id,
        const std::vector<uint8_t>& latest_commitment,
        const std::vector<std::vector<uint8_t>>& revocation_secrets
    );

    /**
     * Remove channel from watch list
     *
     * @param channel_id Channel to stop monitoring
     */
    void UnwatchChannel(const std::string& channel_id);

    /**
     * Monitor channel for breaches
     *
     * Checks if any revoked commitment transactions have been broadcast.
     *
     * @param channel_id Channel to monitor
     * @param latest_commitment Latest commitment transaction (optional update)
     * @param revocation_secrets Revocation secrets (optional update)
     * @return Breach detection result with remedy transaction if breach found
     */
    BreachDetectionResult MonitorChannel(
        const std::string& channel_id,
        const std::vector<uint8_t>& latest_commitment = {},
        const std::vector<std::vector<uint8_t>>& revocation_secrets = {}
    );

    /**
     * Verify commitment proof
     *
     * Validates that a commitment transaction exists on-chain.
     *
     * @param commitment_tx Commitment transaction bytes
     * @param proof Utreexo proof for commitment UTXO
     * @param utreexo_root Expected Utreexo root
     * @return True if proof is valid
     */
    bool VerifyCommitmentProof(
        const std::vector<uint8_t>& commitment_tx,
        const dinero::consensus::BlockUtreexoData& proof,
        const dinero::uint256& utreexo_root
    );

    /**
     * Construct breach remedy transaction
     *
     * Creates a transaction that claims all funds from a revoked commitment.
     *
     * Note: This is a simplified implementation for Phase 11.
     * In a full implementation, this would:
     * 1. Parse the commitment transaction
     * 2. Create inputs spending all outputs
     * 3. Sign with revocation secret
     * 4. Add appropriate fees
     *
     * @param breach_txid Transaction ID of the breach
     * @param revocation_secret Revocation secret for the breached commitment
     * @param proof Proof for the breach transaction
     * @return Signed breach remedy transaction bytes
     */
    std::optional<std::vector<uint8_t>> ConstructBreachRemedy(
        const dinero::uint256& breach_txid,
        const std::vector<uint8_t>& revocation_secret,
        const dinero::consensus::BlockUtreexoData& proof
    );

    /**
     * Get monitoring statistics
     */
    struct Stats {
        uint64_t channels_watched = 0;
        uint64_t breaches_detected = 0;
        uint64_t remedies_constructed = 0;
        uint64_t proof_verifications = 0;
        uint64_t proof_verification_failures = 0;
    };
    Stats GetStats() const;

    /**
     * Get list of watched channels
     */
    std::vector<std::string> GetWatchedChannels() const;

private:
    // Utreexo client for proof queries
    std::shared_ptr<LightningUtreexoClient> utreexo_client_;

    // Watched channels: channel_id → monitoring data
    std::unordered_map<std::string, ChannelMonitoringData> watched_channels_;

    // Statistics
    Stats stats_;

    mutable std::mutex mutex_;

    // Helper: Validate proof structure
    bool ValidateProofStructure(const dinero::consensus::BlockUtreexoData& proof);

    // Helper: Extract transaction ID from commitment tx
    dinero::uint256 ExtractTxid(const std::vector<uint8_t>& tx_bytes);

    // Helper: Check if commitment is revoked
    bool IsRevoked(
        const std::vector<uint8_t>& commitment_tx,
        const std::vector<std::vector<uint8_t>>& revocation_secrets
    );

    // Helper: Find matching revocation secret
    std::optional<std::vector<uint8_t>> FindRevocationSecret(
        const std::vector<uint8_t>& commitment_tx,
        const std::vector<std::vector<uint8_t>>& revocation_secrets
    );

    // Helper: Construct breach remedy without locking (called from MonitorChannel)
    std::optional<std::vector<uint8_t>> ConstructBreachRemedyNoLock(
        const dinero::uint256& breach_txid,
        const std::vector<uint8_t>& revocation_secret,
        const dinero::consensus::BlockUtreexoData& proof
    );
};

} // namespace utreexo
} // namespace lightning
