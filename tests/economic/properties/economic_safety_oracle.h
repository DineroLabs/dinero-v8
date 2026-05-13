#pragma once

#include "../framework/economic_trace.h"
#include <string>
#include <vector>

namespace dinero {
namespace economic {
namespace test {

/**
 * EconomicViolation - Economic safety property violation record
 *
 * Describes what economic rule was violated and where
 */
struct EconomicViolation {
    std::string property_name;  // e.g., "E1: Fee Validation"
    std::string description;    // Human-readable violation description
    uint64_t timestamp;         // When violation occurred
    std::vector<std::string> involved_nodes;  // Which nodes were involved
    std::optional<std::string> tx_id;  // Transaction involved (if applicable)
    std::string details;        // Additional diagnostic information

    EconomicViolation(
        const std::string& prop_name,
        const std::string& desc,
        uint64_t ts = 0
    ) : property_name(prop_name)
      , description(desc)
      , timestamp(ts)
    {}
};

/**
 * EconomicSafetyOracle - Base class for economic safety property oracles
 *
 * Safety properties answer: "Can economic rules be violated?"
 * - E1: Fee Validation - Invalid fee transactions never accepted
 * - E2: Value Conservation - Fees properly calculated (input - output ≥ 0)
 * - E3: Fee Overflow Protection - Fee calculations never overflow
 * - E4: Minimum Relay Fee - Below-minimum-fee txs never relayed
 * - E5: Dust Threshold - Dust outputs properly rejected
 *
 * Pattern (following Ring 5's ConsensusSafetyOracle):
 * 1. reset() - Clear state before new trace
 * 2. observe(trace) - Analyze trace for violations
 * 3. check(trace) - Public API returning violations
 *
 * Subclass Responsibilities:
 * - Implement observeTrace() to detect property-specific violations
 * - Return empty vector if no violations, populated vector if violations found
 */
class EconomicSafetyOracle {
public:
    virtual ~EconomicSafetyOracle() = default;

    /**
     * Check trace for violations
     *
     * @param trace Execution trace to analyze
     * @return List of violations (empty if property holds)
     */
    std::vector<EconomicViolation> check(const EconomicTrace& trace) {
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
    virtual std::vector<EconomicViolation> observeTrace(const EconomicTrace& trace) = 0;

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
     * Check if transaction was rejected
     */
    bool wasTxRejected(const EconomicTrace& trace, const TxID& tx_id) const;

    /**
     * Check if transaction was confirmed in block
     */
    bool wasTxConfirmed(const EconomicTrace& trace, const TxID& tx_id) const;

    /**
     * Get transaction fee from events
     */
    std::optional<uint64_t> getTxFee(const EconomicTrace& trace, const TxID& tx_id) const;

    /**
     * Get transaction input/output values from events
     */
    std::optional<std::pair<uint64_t, uint64_t>> getTxValues(
        const EconomicTrace& trace,
        const TxID& tx_id
    ) const;
};

} // namespace test
} // namespace economic
} // namespace dinero
