#pragma once
#include "din_json.h"
#include "primitives/uint256.h"  // Phase M.1.A: uint256-based transaction identity
#include <string>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <chrono>
#include <memory>

// Forward declarations
namespace dinero {
namespace policy {
    class FeeEstimator;
    class SmartFeeRecommender;
}
}

namespace dinero {
namespace mempool {

struct Transaction {
    uint256 txid;  // Phase M.1.A: uint256-based identity
    uint64_t fee;
    uint64_t size;
    uint64_t weight;
    std::chrono::system_clock::time_point arrival_time;
    std::vector<uint256> depends;  // Phase M.1.A: uint256 dependencies
    bool rbf_enabled = false;
};

struct FeeEstimate {
    double fee_rate;  // una per byte
    uint32_t blocks;  // confirmation target
    double confidence;
};

class PolicyEngine {
public:
    PolicyEngine();
    
    // Fee estimation
    FeeEstimate estimateFee(uint32_t target_blocks) const;
    std::vector<FeeEstimate> getFeeEstimates() const;
    
    // Transaction validation
    bool validateTransaction(const Transaction& tx) const;
    bool isRBFEnabled(const uint256& txid) const;  // Phase M.1.A: uint256 parameter

    // Mempool management
    void addTransaction(const Transaction& tx);
    void removeTransaction(const uint256& txid);  // Phase M.1.A: uint256 parameter
    void confirmTransaction(const uint256& txid, uint32_t confirm_height);  // Phase M.1.A: uint256 parameter
    void updateCurrentHeight(uint32_t height);
    bool hasTransaction(const uint256& txid) const;  // Phase M.1.A: uint256 parameter
    
    // Policy queries
    uint64_t getMinRelayFee() const { return min_relay_fee_; }
    uint64_t getMaxMempoolSize() const { return max_mempool_size_; }
    size_t getMempoolCount() const;
    uint64_t getMempoolBytes() const;
    
    // JSON serialization
    din::Json toJson() const;
    
private:
    mutable std::mutex mtx_;
    std::unordered_map<uint256, Transaction> mempool_;  // Phase M.1.A: uint256 keys
    
    // Policy parameters
    uint64_t min_relay_fee_ = 1000;  // 1000 sat/kB
    uint64_t max_mempool_size_ = 300 * 1024 * 1024;  // 300MB
    uint32_t max_mempool_expiry_ = 14 * 24 * 60 * 60;  // 14 days
    uint32_t current_height_ = 0;  // Current blockchain height
    
    // Fee estimation components
    std::shared_ptr<policy::FeeEstimator> fee_estimator_;
    std::unique_ptr<policy::SmartFeeRecommender> smart_recommender_;
    mutable std::chrono::system_clock::time_point last_estimate_update_;
    
    void updateFeeEstimates() const;
    double calculateFeeRate(uint32_t target_blocks) const;
};

} // namespace mempool
} // namespace dinero
