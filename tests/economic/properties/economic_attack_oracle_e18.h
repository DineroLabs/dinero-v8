#pragma once

#include "economic_attack_oracle.h"

namespace dinero {
namespace economic {
namespace test {

/**
 * E18Oracle - Transaction Malleability Resistance
 *
 * Property: Transaction IDs remain stable (no malleability attacks)
 *
 * Violation conditions:
 * - Transaction accepted with one ID, confirmed with different ID
 * - Transaction ID changes after being accepted to mempool
 * - Malleability attack successfully alters transaction ID
 *
 * Observable facts:
 * - TX_ACCEPTED_TO_MEMPOOL events show original tx ID
 * - TX_INCLUDED_IN_BLOCK events show confirmed tx ID
 * - Transaction IDs should match throughout lifecycle
 *
 * Pattern: Check that tx_id remains constant from acceptance to confirmation
 *
 * Note: Transaction malleability was a major vulnerability in early Bitcoin
 * (fixed by SegWit). This oracle ensures tx IDs are immutable.
 *
 * Simplified for Phase 6e: We check that no transaction appears with
 * multiple IDs in the trace (indicating malleability).
 */
class E18Oracle : public EconomicAttackOracle {
public:
    std::string getName() const override {
        return "E18: Transaction Malleability Resistance";
    }

protected:
    std::vector<EconomicAttackViolation> observeTrace(const EconomicTrace& trace) override;
};

} // namespace test
} // namespace economic
} // namespace dinero
