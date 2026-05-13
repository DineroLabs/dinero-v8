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
 * EconomicAttackViolation
 *
 * Represents a violation of economic attack resistance properties.
 * These violations indicate that the economic system is vulnerable to
 * known attack vectors (double-spend, fee sniping, malleability, etc.).
 */
struct EconomicAttackViolation {
    std::string property_name;
    std::string description;
    uint64_t timestamp;

    std::optional<std::string> tx_id;
    std::optional<std::string> node_id;
    std::vector<std::string> involved_nodes;
    std::string details;

    EconomicAttackViolation(
        const std::string& prop_name,
        const std::string& desc,
        uint64_t ts
    ) : property_name(prop_name), description(desc), timestamp(ts) {}
};

/**
 * EconomicAttackOracle
 *
 * Base class for all economic attack resistance oracles (E16-E20).
 *
 * Pattern: Observable-facts-only (inherited from Ring 5)
 * - Oracles only assert over facts present in the trace
 * - No inference about what "should" or "must" happen
 * - Check if system resists known economic attacks
 *
 * Lifecycle:
 *   1. reset() - Clear internal state
 *   2. observeTrace() - Examine trace, return violations
 *   3. check() - Convenience wrapper (reset + observe)
 *
 * Subclasses implement:
 *   - getName() - Property name
 *   - observeTrace() - Property-specific attack detection
 */
class EconomicAttackOracle {
public:
    virtual ~EconomicAttackOracle() = default;

    /**
     * Check trace for economic attack vulnerabilities
     */
    std::vector<EconomicAttackViolation> check(const EconomicTrace& trace) {
        reset();
        return observeTrace(trace);
    }

    /**
     * Get property name (e.g., "E16: Double-Spend Attack Resistance")
     */
    virtual std::string getName() const = 0;

protected:
    /**
     * Reset oracle state before checking new trace
     */
    virtual void reset() {}

    /**
     * Observe trace and detect attack vulnerabilities
     */
    virtual std::vector<EconomicAttackViolation> observeTrace(const EconomicTrace& trace) = 0;

    // ========================================================================
    // Helper Methods (Common Patterns for E16-E20)
    // ========================================================================

    /**
     * Get all transactions that were confirmed in blocks
     */
    std::vector<std::string> getConfirmedTxs(const EconomicTrace& trace) const;

    /**
     * Get all transactions that were reorged out
     */
    std::vector<std::string> getReorgedTxs(const EconomicTrace& trace) const;

    /**
     * Get timestamp when transaction was confirmed
     */
    std::optional<uint64_t> getTxConfirmedTime(const EconomicTrace& trace, const std::string& tx_id) const;

    /**
     * Get block height where transaction was confirmed
     */
    std::optional<uint64_t> getTxConfirmedHeight(const EconomicTrace& trace, const std::string& tx_id) const;

    /**
     * Check if transaction was reorged out after confirmation
     */
    bool wasTxReorgedOut(const EconomicTrace& trace, const std::string& tx_id) const;

    /**
     * Get all events of a specific type
     */
    std::vector<EconomicEvent> getEventsByType(const EconomicTrace& trace, EconomicEventType type) const;

    /**
     * Get all events for a specific transaction
     */
    std::vector<EconomicEvent> getEventsForTx(const EconomicTrace& trace, const std::string& tx_id) const;

    /**
     * Get all block template assembly events
     */
    std::vector<EconomicEvent> getBlockTemplateEvents(const EconomicTrace& trace) const;

    /**
     * Check if two transactions are conflicting (double-spend)
     */
    bool areConflicting(const std::string& tx1_id, const std::string& tx2_id) const;
};

} // namespace test
} // namespace economic
} // namespace dinero
