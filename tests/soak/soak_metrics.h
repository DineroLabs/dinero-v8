#pragma once

/**
 * Soak Test Metrics Collection
 *
 * Collects and analyzes metrics during soak testing to detect anomalies.
 */

#include <atomic>
#include <chrono>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace dinero {
namespace soak {

/**
 * Time-series metric point
 */
struct MetricPoint {
    std::chrono::steady_clock::time_point timestamp;
    double value;
};

/**
 * Anomaly detection result
 */
struct AnomalyReport {
    bool has_anomaly = false;
    std::string metric_name;
    std::string description;
    double expected_value = 0.0;
    double actual_value = 0.0;
    std::chrono::steady_clock::time_point detected_at;
};

/**
 * Aggregated soak metrics
 */
struct SoakMetrics {
    // Chain state
    uint32_t chain_height = 0;
    uint32_t blocks_mined = 0;
    double blocks_per_minute = 0.0;

    // CT transactions
    uint64_t ct_txs_created = 0;
    uint64_t ct_txs_confirmed = 0;
    uint64_t ct_txs_orphaned = 0;
    uint64_t ct_txs_in_mempool = 0;

    // Transparent transactions
    uint64_t transparent_txs_created = 0;
    uint64_t transparent_txs_confirmed = 0;

    // Verification performance
    double avg_proof_verification_ms = 0.0;
    double peak_proof_verification_ms = 0.0;
    uint64_t total_proofs_verified = 0;

    // Memory usage (bytes)
    uint64_t process_memory = 0;
    uint64_t heap_memory = 0;
    uint64_t mempool_memory = 0;

    // Errors
    uint32_t consensus_errors = 0;
    uint32_t validation_errors = 0;
    uint32_t network_errors = 0;

    // Timing
    std::chrono::steady_clock::time_point start_time;
    std::chrono::steady_clock::time_point last_update;
    std::chrono::seconds elapsed() const {
        return std::chrono::duration_cast<std::chrono::seconds>(last_update - start_time);
    }
};

/**
 * Anomaly detection thresholds
 */
struct AnomalyThresholds {
    // Memory growth (bytes per hour)
    uint64_t max_memory_growth_per_hour = 10 * 1024 * 1024;  // 10 MB/hr

    // Orphan rate
    double max_orphan_rate = 0.01;  // 1%

    // Verification slowdown
    double max_proof_verification_ms = 500.0;  // 500ms

    // Error thresholds
    uint32_t max_consensus_errors = 0;
    uint32_t max_validation_errors = 10;
};

/**
 * Soak Metrics Collector
 *
 * Collects time-series metrics and detects anomalies.
 */
class SoakMetricsCollector {
public:
    explicit SoakMetricsCollector(const AnomalyThresholds& thresholds = AnomalyThresholds{});
    ~SoakMetricsCollector() = default;

    // Record metrics
    void RecordBlockMined(uint32_t height);
    void RecordCTTransaction(bool confirmed);
    void RecordTransparentTransaction(bool confirmed);
    void RecordCTOrphaned();
    void RecordProofVerification(double duration_ms);
    void RecordMemoryUsage(uint64_t process, uint64_t heap, uint64_t mempool);
    void RecordConsensusError();
    void RecordValidationError();
    void RecordNetworkError();

    // Query metrics
    SoakMetrics GetCurrentMetrics() const;
    std::vector<MetricPoint> GetMetricHistory(const std::string& metric_name) const;

    // Anomaly detection
    std::vector<AnomalyReport> CheckForAnomalies() const;
    bool HasCriticalAnomaly() const;

    // Reset
    void Reset();

    // Reporting
    std::string GenerateReport() const;
    std::string GenerateSummary() const;

private:
    void AddMetricPoint(const std::string& name, double value);
    double CalculateRate(const std::deque<MetricPoint>& points, std::chrono::seconds window) const;

    mutable std::mutex mutex_;
    AnomalyThresholds thresholds_;

    // Current metrics
    std::atomic<uint32_t> chain_height_{0};
    std::atomic<uint32_t> blocks_mined_{0};
    std::atomic<uint64_t> ct_txs_created_{0};
    std::atomic<uint64_t> ct_txs_confirmed_{0};
    std::atomic<uint64_t> ct_txs_orphaned_{0};
    std::atomic<uint64_t> transparent_txs_created_{0};
    std::atomic<uint64_t> transparent_txs_confirmed_{0};
    std::atomic<uint64_t> total_proofs_verified_{0};
    std::atomic<uint32_t> consensus_errors_{0};
    std::atomic<uint32_t> validation_errors_{0};
    std::atomic<uint32_t> network_errors_{0};

    // Time-series data
    std::map<std::string, std::deque<MetricPoint>> metric_history_;
    static constexpr size_t MAX_HISTORY_POINTS = 10000;

    // Timing
    std::chrono::steady_clock::time_point start_time_;

    // Memory tracking
    std::deque<std::pair<std::chrono::steady_clock::time_point, uint64_t>> memory_samples_;
};

} // namespace soak
} // namespace dinero
