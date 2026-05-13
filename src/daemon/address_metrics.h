#pragma once

#include <string>
#include <unordered_map>
#include <chrono>
#include <mutex>

namespace dinero {

/**
 * Metrics and rate limiting for address validation failures
 */
class AddressValidationMetrics {
public:
    enum class FailureReason {
        HRP_MISMATCH,
        CHECKSUM_INVALID,
        MIXED_CASE,
        WITVER_NOT_SUPPORTED,
        MALFORMED,
        UNKNOWN
    };

    static AddressValidationMetrics& getInstance();

    // Record a validation failure
    void recordFailure(FailureReason reason, const std::string& address);

    // Get failure counts
    std::unordered_map<std::string, uint64_t> getFailureCounts() const;

    // Check if we should log this failure (rate limiting)
    bool shouldLogFailure(FailureReason reason);

private:
    AddressValidationMetrics() = default;
    
    mutable std::mutex mutex_;
    std::unordered_map<FailureReason, uint64_t> failure_counts_;
    std::unordered_map<FailureReason, std::chrono::steady_clock::time_point> last_log_time_;
    
    static constexpr std::chrono::minutes LOG_RATE_LIMIT{1}; // Log at most once per minute per reason
    
    std::string reasonToString(FailureReason reason) const;
};

} // namespace dinero
