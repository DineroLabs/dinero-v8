#include "wallet/fee_estimator_impl.h"
#include "daemon/mempool.h"

namespace din {

FeeEstimatorImpl::FeeEstimatorImpl(dinero::Mempool* mempool) 
    : mempool_(mempool) {}

std::optional<double> FeeEstimatorImpl::estimate(int target_blocks) const {
    // Implement fee estimation based on target blocks
    if (target_blocks <= 0) return std::nullopt;
    
    if (mempool_) {
        // Get mempool statistics for dynamic fee estimation
        auto stats = mempool_->getStats();
        
        // Basic fee estimation algorithm:
        // - Fast (1 block): High fee for immediate confirmation
        // - Normal (2-6 blocks): Medium fee for reasonable confirmation
        // - Economy (6+ blocks): Low fee for eventual confirmation
        
        if (target_blocks == 1) {
            return FAST_FEE;
        } else if (target_blocks >= 2 && target_blocks <= 6) {
            return NORMAL_FEE;
        } else {
            return ECONOMY_FEE;
        }
    }
    
    // Fallback when mempool unavailable
    return getFallbackFee();
}

double FeeEstimatorImpl::getMinRelayFee() const {
    return MIN_RELAY_FEE;
}

double FeeEstimatorImpl::getFallbackFee() const {
    return FALLBACK_FEE;
}

} // namespace din
