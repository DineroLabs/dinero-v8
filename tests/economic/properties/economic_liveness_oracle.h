#pragma once

#include "../framework/economic_trace.h"
#include <string>
#include <vector>

namespace dinero {
namespace economic {
namespace test {

/**
 * EconomicLivenessViolation - Economic liveness property violation record
 *
 * Describes what economic liveness guarantee was violated and where
 */
struct EconomicLivenessViolation {
    std::string property_name;  // e.g., "E6: Fee-Bearing TX Inclusion"
    std::string description;    // Human-readable violation description
    uint64_t timestamp;         // When violation occurred
    std::vector<std::string> involved_nodes;  // Which nodes were involved
    std::optional<std::string> tx_id;  // Transaction involved (if applicable)
    std::string details;        // Additional diagnostic information

    EconomicLivenessViolation(
        const std::string& prop_name,
        const std::string& desc,
        uint64_t ts = 0
    ) : property_name(prop_name)
      , description(desc)
      , timestamp(ts)
    {}
};

/**
 * EconomicLivenessOracle - Base class for economic liveness property oracles
 *
 * Liveness properties answer: "Do valid economic transactions eventually succeed?"
 * - E6: Fee-Bearing TX Inclusion - Valid fee-bearing txs eventually included
 * - E7: Mempool Replacement - RBF txs eventually replace lower-fee versions
 * - E8: Fee Estimation - Fee estimator provides bounded estimates
 * - E9: Block Assembly - Templates include highest-fee valid transactions
 * - E10: Economic Finality - Transactions with sufficient fee depth don't reorg out
 *
 * Pattern (following Ring 5's ConsensusLivenessOracle):
 * 1. reset() - Clear state before new trace
 * 2. observe(trace) - Analyze trace for violations
 * 3. check(trace) - Public API returning violations
 *
 * Subclass Responsibilities:
 * - Implement observeTrace() to detect property-specific violations
 * - Return empty vector if no violations, populated vector if violations found
 */
class EconomicLivenessOracle {
public:
    virtual ~EconomicLivenessOracle() = default;

    /**
     * Check trace for violations
     *
     * @param trace Execution trace to analyze
     * @return List of violations (empty if property holds)
     */
    std::vector<EconomicLivenessViolation> check(const EconomicTrace& trace) {
        reset();
        return observeTrace(trace);
    }

    /**
     * Get oracle name (for reporting)
     */
    virtual std::string getName() const = 0;

protected:
    /**
     * Reset oracle state before analyzing new trace
     *
     * Subclasses override to clear property-specific state
     */
    virtual void reset() {
        // Default: no state to clear
    }

    /**
     * Observe trace and detect violations
     *
     * Subclasses implement property-specific detection logic
     *
     * @param trace Execution trace to analyze
     * @return List of violations (empty if property holds)
     */
    virtual std::vector<EconomicLivenessViolation> observeTrace(const EconomicTrace& trace) = 0;

    // ========================================================================
    // Helper Methods for Subclasses
    // ========================================================================

    /**
     * Get all nodes from trace
     */
    std::vector<NodeID> getAllNodes(const EconomicTrace& trace) const;

    /**
     * Get all events of specific type
     */
    std::vector<EconomicEvent> getEventsByType(
        const EconomicTrace& trace,
        EconomicEventType type
    ) const;

    /**
     * Get events for specific transaction
     */
    std::vector<EconomicEvent> getEventsForTx(
        const EconomicTrace& trace,
        const TxID& tx_id
    ) const;

    /**
     * Check if transaction was accepted to mempool
     */
    bool wasTxAccepted(const EconomicTrace& trace, const TxID& tx_id) const;

    /**
     * Check if transaction was confirmed in block
     */
    bool wasTxConfirmed(const EconomicTrace& trace, const TxID& tx_id) const;

    /**
     * Check if transaction was included in block template
     */
    bool wasTxInTemplate(const EconomicTrace& trace, const TxID& tx_id) const;

    /**
     * Get time of first TX_ACCEPTED event for transaction
     */
    std::optional<uint64_t> getTxAcceptedTime(const EconomicTrace& trace, const TxID& tx_id) const;

    /**
     * Get time of TX_INCLUDED_IN_BLOCK event for transaction
     */
    std::optional<uint64_t> getTxConfirmedTime(const EconomicTrace& trace, const TxID& tx_id) const;

    /**
     * Get all transactions that were submitted but never confirmed
     */
    std::vector<TxID> getUnconfirmedTxs(const EconomicTrace& trace) const;
};

} // namespace test
} // namespace economic
} // namespace dinero
