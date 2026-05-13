#pragma once
// ═══════════════════════════════════════════════════════════════════════════
// Lightning HTLC Sweep Oracle Interface (L1↔L2 Boundary - Phase 7B)
// ═══════════════════════════════════════════════════════════════════════════
// Defines the interface through which Lightning (L2) requests HTLC sweep
// transaction building and broadcasting.
//
// ARCHITECTURE:
// - Lightning L2 MUST NOT build/sign/broadcast transactions
// - Lightning L2 decides WHAT and WHEN to sweep
// - L1 oracle decides HOW (script building, signing, fee calculation)
// - Production implementation uses wallet + transaction builder
// - Test implementation uses mocks
//
// This enforces Phase 7B architectural boundary: L2 = policy, L1 = execution.
// ═══════════════════════════════════════════════════════════════════════════

#include "lightning/lightning_db_types.h"
#include <string>
#include <optional>
#include <map>

namespace lightning {

/**
 * Interface for Lightning to request HTLC sweep transaction building/broadcasting.
 *
 * Production implementation: Uses wallet + TaprootTxSigner + mempool
 * Test implementation: MockHTLCSweepOracle with controlled responses
 *
 * Replaces direct access to:
 * - Transaction builder
 * - Signing keys
 * - Mempool broadcast
 */
class IHTLCSweepOracle {
public:
    virtual ~IHTLCSweepOracle() = default;

    // ═══════════════════════════════════════════════════════════════════════
    // HTLC Sweep Transaction Building & Broadcasting
    // ═══════════════════════════════════════════════════════════════════════

    /**
     * Build and broadcast an HTLC sweep transaction.
     *
     * Phase 7B Contract:
     * - L2 provides: sweep record with all necessary metadata
     * - L1 builds: actual transaction with correct scripts/witnesses
     * - L1 signs: using appropriate keys
     * - L1 broadcasts: to mempool
     *
     * @param sweep Sweep record (immutable) - contains all L2 policy decisions
     * @return Transaction ID if broadcast succeeded, empty string otherwise
     *
     * Failure modes (returns empty string):
     * - Script construction failed
     * - Insufficient fees
     * - Policy violation
     * - Mempool rejection
     */
    virtual std::string broadcastSweep(
        const dinero::lightning::HTLCSweepRecord& sweep
    ) = 0;

    /**
     * Check if a sweep transaction has been confirmed.
     *
     * @param sweep_txid Transaction ID from broadcastSweep()
     * @return Block height if confirmed, std::nullopt if unconfirmed
     */
    virtual std::optional<uint64_t> getSweepConfirmationHeight(
        const std::string& sweep_txid
    ) const = 0;
};

/**
 * Mock implementation for testing.
 * Allows tests to control sweep outcomes without building real transactions.
 */
class MockHTLCSweepOracle : public IHTLCSweepOracle {
public:
    MockHTLCSweepOracle() = default;

    // Test configuration
    void setBroadcastSucceeds(bool succeeds) { m_broadcast_succeeds = succeeds; }
    void setSweepConfirmed(const std::string& txid, uint64_t height) {
        m_confirmed_sweeps[txid] = height;
    }

    // IHTLCSweepOracle implementation
    std::string broadcastSweep(
        const dinero::lightning::HTLCSweepRecord& sweep
    ) override {
        if (!m_broadcast_succeeds) {
            return ""; // Broadcast failed
        }

        // Mock: Generate deterministic txid from sweep_id
        std::string txid = "sweep_tx_" + sweep.sweep_id;
        m_broadcast_count++;
        m_last_broadcast = sweep;

        return txid;
    }

    std::optional<uint64_t> getSweepConfirmationHeight(
        const std::string& sweep_txid
    ) const override {
        auto it = m_confirmed_sweeps.find(sweep_txid);
        if (it != m_confirmed_sweeps.end()) {
            return it->second;
        }
        return std::nullopt;
    }

    // Test inspection
    uint32_t getBroadcastCount() const { return m_broadcast_count; }
    const dinero::lightning::HTLCSweepRecord& getLastBroadcast() const { return m_last_broadcast; }

private:
    bool m_broadcast_succeeds = true;
    uint32_t m_broadcast_count = 0;
    dinero::lightning::HTLCSweepRecord m_last_broadcast;
    std::map<std::string, uint64_t> m_confirmed_sweeps;
};

} // namespace lightning
