#pragma once

/**
 * @file mempool_interface.h
 * @brief FROZEN Wallet ↔ Mempool Interface Contract
 *
 * Version: v0.12.0
 * Status: FROZEN - Do not modify without exceptional justification
 *
 * Design Principle:
 * - Wallet asks questions. Mempool gives answers.
 * - Wallet never reaches inside mempool internals.
 * - Mempool never touches wallet keys or coin selection.
 *
 * This interface must survive:
 * - Future relay protocol changes
 * - Fee estimator rewrites
 * - P2P network evolution
 * - Hardware wallet integration
 */

#include "wallet/transaction.h"
#include <string>
#include <vector>
#include <cstdint>

namespace dinero {
namespace wallet {

// ============================================================================
// READ-ONLY: Mempool → Wallet Information Flow
// ============================================================================

/**
 * @brief Mempool state information
 *
 * Wallet uses this for UI display and fee estimation hints.
 * Does NOT expose internal mempool structure.
 */
struct MempoolInfo {
    size_t tx_count;              // Number of transactions in mempool
    size_t total_bytes;           // Total size in bytes
    double min_relay_fee_rate;    // Minimum fee rate to relay (sat/byte)

    MempoolInfo()
        : tx_count(0), total_bytes(0), min_relay_fee_rate(1.0) {}
};

/**
 * @brief Transaction policy validation result
 *
 * Returned by testAcceptTransaction - tells wallet if tx would be accepted.
 * Wallet uses this BEFORE signing to avoid wasted work.
 */
struct TxPolicyResult {
    bool would_accept;                  // Would mempool accept this tx?
    std::string rejection_reason;       // Human-readable reason if rejected

    // Policy metrics (informational)
    uint32_t ancestor_count;            // Number of unconfirmed ancestors
    uint32_t descendant_count;          // Number of unconfirmed descendants (if replacing)
    double effective_feerate;           // CPFP-aware package feerate (sat/byte)

    // RBF information
    bool conflicts_exist;               // Does this tx double-spend existing mempool tx?
    std::vector<std::string> conflicting_txids;  // TXIDs this would replace

    TxPolicyResult()
        : would_accept(false), ancestor_count(0), descendant_count(0),
          effective_feerate(0.0), conflicts_exist(false) {}
};

/**
 * @brief Transaction submission result
 *
 * Returned after attempting to add transaction to mempool.
 */
struct SubmitResult {
    enum class Status {
        ACCEPTED,       // Transaction accepted into mempool
        REJECTED,       // Transaction rejected (see reason)
        REPLACED        // RBF: Replaced existing transaction(s)
    };

    Status status;
    std::string txid;                       // Transaction ID (if accepted/replaced)
    std::string reason;                     // Rejection reason (if rejected)
    std::vector<std::string> replaced_txids; // TXIDs removed (if RBF replacement)

    // Policy diagnostics
    uint32_t ancestor_count;
    uint32_t descendant_count;
    double effective_feerate;

    SubmitResult()
        : status(Status::REJECTED), ancestor_count(0), descendant_count(0),
          effective_feerate(0.0) {}
};

// ============================================================================
// WRITE-ONLY: Wallet → Mempool Submission Flow
// ============================================================================

/**
 * @brief Transaction submission mode
 *
 * Controls how mempool processes the transaction.
 */
enum class SubmitMode {
    /**
     * TEST_ONLY: Policy validation without signature checks
     * - Used for testing and fee estimation
     * - Never relayed to network
     * - Only available in regtest mode
     */
    TEST_ONLY,

    /**
     * BROADCAST: Full validation with network relay
     * - Validates signatures
     * - Relays to peers if accepted
     * - Production mode
     */
    BROADCAST
};

// ============================================================================
// INTERFACE: Mempool Public API for Wallet
// ============================================================================

/**
 * @brief Abstract interface to mempool (dependency injection)
 *
 * Wallet depends on this interface, not concrete mempool implementation.
 * Enables testing with mock mempools.
 */
class IMempoolInterface {
public:
    virtual ~IMempoolInterface() = default;

    // ========================================================================
    // Query Methods (Read-Only)
    // ========================================================================

    /**
     * @brief Get current mempool state information
     * @return Mempool statistics and configuration
     */
    virtual MempoolInfo getMempoolInfo() const = 0;

    /**
     * @brief Test if transaction would be accepted (dry-run)
     * @param tx Transaction to test
     * @return Policy validation result with diagnostics
     *
     * Does NOT modify mempool state.
     * Use this before signing to check if tx meets policy.
     */
    virtual TxPolicyResult testAcceptTransaction(const Transaction& tx) const = 0;

    /**
     * @brief Check if transaction exists in mempool
     * @param txid Transaction ID
     * @return true if tx is in mempool
     */
    virtual bool hasTransaction(const std::string& txid) const = 0;

    // ========================================================================
    // Mutation Methods (Write)
    // ========================================================================

    /**
     * @brief Submit transaction to mempool
     * @param tx Transaction to submit
     * @param mode Submission mode (TEST_ONLY or BROADCAST)
     * @return Submission result with diagnostics
     *
     * Validates transaction against policy rules.
     * If mode=BROADCAST, relays to network on acceptance.
     */
    virtual SubmitResult submitTransaction(
        const Transaction& tx,
        SubmitMode mode
    ) = 0;
};

// ============================================================================
// EXPLICIT NON-RESPONSIBILITIES
// ============================================================================

/**
 * WALLET DOES NOT:
 * - Enforce mempool policy (ancestor/descendant limits, size limits, etc.)
 * - Calculate package feerate (mempool's job via CPFP)
 * - Track mempool eviction state
 * - Access mempool internal data structures
 * - Estimate fees (separate fee estimator module, not part of v0.12.0)
 *
 * MEMPOOL DOES NOT:
 * - Handle wallet keys or signing
 * - Perform coin selection
 * - Track wallet balances
 * - Manage wallet addresses
 * - Decrypt encrypted wallets
 */

} // namespace wallet
} // namespace dinero
