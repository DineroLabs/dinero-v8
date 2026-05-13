#pragma once

#include "economic_trace.h"
#include "economic_types.h"
#include <vector>
#include <string>
#include <optional>
#include <map>
#include <set>

namespace dinero {
namespace economic {
namespace test {

/**
 * EconomicIncentiveViolation
 *
 * Represents a violation of economic incentive compatibility properties.
 * These violations indicate that the economic system fails to properly
 * align incentives (miners, users, attackers).
 */
struct EconomicIncentiveViolation {
    std::string property_name;
    std::string description;
    uint64_t timestamp;

    std::optional<std::string> tx_id;
    std::optional<std::string> node_id;
    std::vector<std::string> involved_nodes;
    std::string details;

    EconomicIncentiveViolation(
        const std::string& prop_name,
        const std::string& desc,
        uint64_t ts
    ) : property_name(prop_name), description(desc), timestamp(ts) {}
};

/**
 * EconomicIncentiveOracle
 *
 * Base class for all economic incentive compatibility oracles (E11-E15).
 *
 * Pattern: Observable-facts-only (inherited from Ring 5)
 * - Oracles only assert over facts present in the trace
 * - No inference about what "should" or "must" happen
 * - Check if incentive structures are properly aligned
 *
 * Lifecycle:
 *   1. reset() - Clear internal state
 *   2. observeTrace() - Examine trace, return violations
 *   3. check() - Convenience wrapper (reset + observe)
 *
 * Subclasses implement:
 *   - getName() - Property name
 *   - observeTrace() - Property-specific violation detection
 */
class EconomicIncentiveOracle {
public:
    virtual ~EconomicIncentiveOracle() = default;

    /**
     * Check trace for incentive compatibility violations
     */
    std::vector<EconomicIncentiveViolation> check(const EconomicTrace& trace) {
        reset();
        return observeTrace(trace);
    }

    /**
     * Get property name (e.g., "E11: Mining Incentive Compatibility")
     */
    virtual std::string getName() const = 0;

protected:
    /**
     * Reset oracle state before checking new trace
     */
    virtual void reset() {}

    /**
     * Observe trace and detect violations
     */
    virtual std::vector<EconomicIncentiveViolation> observeTrace(const EconomicTrace& trace) = 0;

    // ========================================================================
    // Helper Methods (Common Patterns for E11-E15)
    // ========================================================================

    /**
     * Get all transactions that were accepted to mempool
     */
    std::vector<std::string> getAcceptedTxs(const EconomicTrace& trace) const;

    /**
     * Get all transactions that were confirmed in blocks
     */
    std::vector<std::string> getConfirmedTxs(const EconomicTrace& trace) const;

    /**
     * Get all transactions that were evicted from mempool
     */
    std::vector<std::string> getEvictedTxs(const EconomicTrace& trace) const;

    /**
     * Get fee rate for a transaction from trace events
     */
    std::optional<double> getTxFeeRate(const EconomicTrace& trace, const std::string& tx_id) const;

    /**
     * Get timestamp when transaction was accepted to mempool
     */
    std::optional<uint64_t> getTxAcceptedTime(const EconomicTrace& trace, const std::string& tx_id) const;

    /**
     * Get timestamp when transaction was confirmed
     */
    std::optional<uint64_t> getTxConfirmedTime(const EconomicTrace& trace, const std::string& tx_id) const;

    /**
     * Get all transactions selected for block at given timestamp
     */
    std::vector<std::string> getSelectedTxsAtTime(const EconomicTrace& trace, uint64_t timestamp) const;

    /**
     * Get all transactions excluded from block at given timestamp
     */
    std::vector<std::string> getExcludedTxsAtTime(const EconomicTrace& trace, uint64_t timestamp) const;

    /**
     * Check if transaction was evicted from mempool
     */
    bool wasTxEvicted(const EconomicTrace& trace, const std::string& tx_id) const;

    /**
     * Get all events of a specific type
     */
    std::vector<EconomicEvent> getEventsByType(const EconomicTrace& trace, EconomicEventType type) const;

    /**
     * Get all events for a specific transaction
     */
    std::vector<EconomicEvent> getEventsForTx(const EconomicTrace& trace, const std::string& tx_id) const;
};

} // namespace test
} // namespace economic
} // namespace dinero
