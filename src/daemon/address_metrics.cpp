#include "daemon/address_metrics.h"
#include "common/logger.h"
#include <sstream>

namespace dinero {

AddressValidationMetrics& AddressValidationMetrics::getInstance() {
    static AddressValidationMetrics instance;
    return instance;
}

void AddressValidationMetrics::recordFailure(FailureReason reason, const std::string& address) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    // Increment failure count
    failure_counts_[reason]++;
    
    // Log with rate limiting
    if (shouldLogFailure(reason)) {
        std::string reason_str = reasonToString(reason);
        g_logger.info("Address validation failure: " + reason_str + " for address: " + address);
        last_log_time_[reason] = std::chrono::steady_clock::now();
    }
}

std::unordered_map<std::string, uint64_t> AddressValidationMetrics::getFailureCounts() const {
    std::lock_guard<std::mutex> lock(mutex_);
    
    std::unordered_map<std::string, uint64_t> result;
    for (const auto& [reason, count] : failure_counts_) {
        result[reasonToString(reason)] = count;
    }
    return result;
}

bool AddressValidationMetrics::shouldLogFailure(FailureReason reason) {
    auto now = std::chrono::steady_clock::now();
    auto it = last_log_time_.find(reason);
    
    if (it == last_log_time_.end()) {
        return true; // First time logging this reason
    }
    
    return (now - it->second) >= LOG_RATE_LIMIT;
}

std::string AddressValidationMetrics::reasonToString(FailureReason reason) const {
    switch (reason) {
        case FailureReason::HRP_MISMATCH:
            return "hrp_mismatch";
        case FailureReason::CHECKSUM_INVALID:
            return "checksum_invalid";
        case FailureReason::MIXED_CASE:
            return "mixed_case";
        case FailureReason::WITVER_NOT_SUPPORTED:
            return "witver_not_supported";
        case FailureReason::MALFORMED:
            return "malformed";
        case FailureReason::UNKNOWN:
        default:
            return "unknown";
    }
}

} // namespace dinero
