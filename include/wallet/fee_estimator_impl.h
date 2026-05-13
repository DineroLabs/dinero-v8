#pragma once
#include "wallet/wallet_iface.h"

namespace dinero {
    class Mempool; // Forward declaration
}

namespace din {

/**
 * @brief Fee estimator implementation using Dinero mempool
 * 
 * Provides fee estimation based on mempool transaction data
 * and historical confirmation patterns.
 */
class FeeEstimatorImpl : public IFeeEstimator {
public:
    explicit FeeEstimatorImpl(dinero::Mempool* mempool);
    
    // IFeeEstimator implementation
    std::optional<double> estimate(int target_blocks) const override;
    double getMinRelayFee() const override;
    double getFallbackFee() const override;

private:
    dinero::Mempool* mempool_;
    
    // Fee rate constants (sat/vB)
    static constexpr double MIN_RELAY_FEE = 1.0;    // 1 sat/vB minimum
    static constexpr double FALLBACK_FEE = 20.0;    // 20 sat/vB fallback
    static constexpr double FAST_FEE = 50.0;        // 1 block target
    static constexpr double NORMAL_FEE = 30.0;      // 2-6 block target  
    static constexpr double ECONOMY_FEE = 20.0;     // 6+ block target
};

} // namespace din
