#pragma once
// ═══════════════════════════════════════════════════════════════════════════
// Lightning Justice Oracle Interface (L1↔L2 Boundary - Phase 7C)
// ═══════════════════════════════════════════════════════════════════════════
// Defines the interface through which Lightning (L2) requests justice
// transaction building and broadcasting after breach detection.
//
// ARCHITECTURE:
// - Lightning L2 MUST NOT build/sign/broadcast transactions
// - Lightning L2 decides WHEN justice is required
// - L1 oracle decides HOW (script building, revocation key derivation, signing)
// - Production implementation uses wallet + revocation keys + transaction builder
// - Test implementation uses mocks
//
// This enforces Phase 7C architectural boundary: L2 = breach detection, L1 = punishment execution.
// ═══════════════════════════════════════════════════════════════════════════

#include "lightning/lightning_db_types.h"
#include "common/status.h"
#include "result.h"
#include <string>
#include <optional>
#include <map>

namespace lightning {

/**
 * @brief Justice transaction data returned by oracle
 *
 * Contains the fully built and signed justice transaction ready for broadcast.
 */
struct JusticeTx {
    std::string tx_hex;        // Fully signed transaction (hex)
    std::string txid;          // Transaction ID (hex)
    uint64_t total_value;      // Total value being claimed (una)
};

/**
 * Interface for Lightning to request justice transaction building/broadcasting.
 *
 * Production implementation: Uses wallet + revocation keys + TaprootTxSigner
 * Test implementation: MockJusticeOracle with controlled outcomes
 *
 * Replaces direct access to:
 * - Revocation key derivation
 * - Transaction builder
 * - Signing keys
 * - Mempool broadcast
 */
class IJusticeOracle {
public:
    virtual ~IJusticeOracle() = default;

    // ═══════════════════════════════════════════════════════════════════════
    // Justice Transaction Building & Broadcasting
    // ═══════════════════════════════════════════════════════════════════════

    /**
     * Build a justice transaction to punish a revoked commitment.
     *
     * Phase 7C Contract:
     * - L2 provides: justice record + channel with all necessary metadata
     * - L1 builds: transaction using revocation secret to claim all outputs
     * - L1 signs: using appropriate keys
     * - Returns: fully signed transaction ready for broadcast
     *
     * @param justice Justice record (immutable) - contains L2 breach detection
     * @param channel Channel record - contains output scripts and values
     * @return JusticeTx if build succeeded, error otherwise
     *
     * Failure modes:
     * - Script construction failed
     * - Revocation key derivation failed
     * - Signing failed
     * - Output not found on-chain
     */
    virtual Result<JusticeTx> buildJusticeTransaction(
        const dinero::lightning::JusticeRecord& justice,
        const dinero::lightning::ChannelRecord& channel
    ) = 0;

    /**
     * Broadcast a justice transaction to the network.
     *
     * @param tx Justice transaction to broadcast
     * @return Status::Ok if broadcast succeeded, error otherwise
     *
     * Failure modes:
     * - Mempool rejection
     * - Double-spend detected
     * - Policy violation
     */
    virtual dinero::Status broadcastJusticeTransaction(
        const JusticeTx& tx
    ) = 0;

    /**
     * Check if a justice transaction has been confirmed.
     *
     * @param justice_txid Transaction ID from buildJusticeTransaction()
     * @return Block height if confirmed, std::nullopt if unconfirmed
     */
    virtual std::optional<uint64_t> getJusticeConfirmationHeight(
        const std::string& justice_txid
    ) const = 0;
};

/**
 * Mock implementation for testing.
 * Allows tests to control justice outcomes without building real transactions.
 */
class MockJusticeOracle : public IJusticeOracle {
public:
    MockJusticeOracle() = default;

    // Test configuration
    void setBuildSucceeds(bool succeeds) { m_build_succeeds = succeeds; }
    void setBroadcastSucceeds(bool succeeds) { m_broadcast_succeeds = succeeds; }
    void setJusticeConfirmed(const std::string& txid, uint64_t height) {
        m_confirmed_justice[txid] = height;
    }

    // IJusticeOracle implementation
    Result<JusticeTx> buildJusticeTransaction(
        const dinero::lightning::JusticeRecord& justice,
        const dinero::lightning::ChannelRecord& channel
    ) override {
        (void)channel; // Unused in mock

        if (!m_build_succeeds) {
            return Result<JusticeTx>::Err("Mock: Build failed");
        }

        // Mock: Generate deterministic txid from justice_id
        JusticeTx tx;
        tx.txid = "justice_tx_" + justice.justice_id;
        tx.tx_hex = "mock_justice_tx_hex";
        tx.total_value = 500000; // Mock: Claim 500k sats

        m_build_count++;
        m_last_justice_built = justice;

        return Result<JusticeTx>::Ok(tx);
    }

    dinero::Status broadcastJusticeTransaction(
        const JusticeTx& tx
    ) override {
        if (!m_broadcast_succeeds) {
            return dinero::Status::Internal;
        }

        m_broadcast_count++;
        m_last_txid_broadcast = tx.txid;

        return dinero::Status::Ok;
    }

    std::optional<uint64_t> getJusticeConfirmationHeight(
        const std::string& justice_txid
    ) const override {
        auto it = m_confirmed_justice.find(justice_txid);
        if (it != m_confirmed_justice.end()) {
            return it->second;
        }
        return std::nullopt;
    }

    // Test inspection
    uint32_t getBuildCount() const { return m_build_count; }
    uint32_t getBroadcastCount() const { return m_broadcast_count; }
    const dinero::lightning::JusticeRecord& getLastJusticeBuilt() const { return m_last_justice_built; }
    const std::string& getLastTxidBroadcast() const { return m_last_txid_broadcast; }

private:
    bool m_build_succeeds = true;
    bool m_broadcast_succeeds = true;
    uint32_t m_build_count = 0;
    uint32_t m_broadcast_count = 0;
    dinero::lightning::JusticeRecord m_last_justice_built;
    std::string m_last_txid_broadcast;
    std::map<std::string, uint64_t> m_confirmed_justice;
};

} // namespace lightning
