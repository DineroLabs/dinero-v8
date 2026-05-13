#pragma once

#include "economic_incentive_oracle.h"

namespace dinero {
namespace economic {
namespace test {

/**
 * E14Oracle - Spam Prevention
 *
 * Property: Low-fee spam transactions don't crowd out legitimate transactions
 *
 * Violation conditions:
 * - High-fee transaction evicted from mempool while low-fee transaction remains
 * - Mempool eviction policy fails to prioritize high-fee transactions
 * - Spam attack successfully prevents legitimate transaction inclusion
 *
 * Observable facts:
 * - TX_ACCEPTED_TO_MEMPOOL events show accepted transactions
 * - TX_EVICTED_FROM_MEMPOOL events show evicted transactions
 * - Event fee_rate shows transaction priority
 * - Mempool state snapshots show remaining transactions
 *
 * Pattern: Check that evicted transactions have lower fee rates than remaining ones
 *
 * Note: This checks mempool eviction policy. When mempool is full, we should
 * evict lowest-fee transactions first, not highest-fee ones.
 *
 * Simplified for Phase 6d: We check if a high-fee tx was evicted while
 * a low-fee tx remains in mempool (not evicted, not confirmed).
 */
class E14Oracle : public EconomicIncentiveOracle {
public:
    std::string getName() const override {
        return "E14: Spam Prevention";
    }

protected:
    std::vector<EconomicIncentiveViolation> observeTrace(const EconomicTrace& trace) override;
};

} // namespace test
} // namespace economic
} // namespace dinero
