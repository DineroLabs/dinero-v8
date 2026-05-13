/**
 * Soak Test Metrics Implementation
 */

#include "soak_metrics.h"

#include <algorithm>
#include <iomanip>
#include <sstream>

namespace dinero {
namespace soak {

SoakMetricsCollector::SoakMetricsCollector(const AnomalyThresholds& thresholds)
    : thresholds_(thresholds)
    , start_time_(std::chrono::steady_clock::now())
{
}

void SoakMetricsCollector::RecordBlockMined(uint32_t height) {
    chain_height_.store(height);
    blocks_mined_.fetch_add(1);
    AddMetricPoint("blocks_mined", static_cast<double>(blocks_mined_.load()));
}

void SoakMetricsCollector::RecordCTTransaction(bool confirmed) {
    ct_txs_created_.fetch_add(1);
    if (confirmed) {
        ct_txs_confirmed_.fetch_add(1);
    }
    AddMetricPoint("ct_txs_created", static_cast<double>(ct_txs_created_.load()));
}

void SoakMetricsCollector::RecordTransparentTransaction(bool confirmed) {
    transparent_txs_created_.fetch_add(1);
    if (confirmed) {
        transparent_txs_confirmed_.fetch_add(1);
    }
    AddMetricPoint("transparent_txs_created", static_cast<double>(transparent_txs_created_.load()));
}

void SoakMetricsCollector::RecordCTOrphaned() {
    ct_txs_orphaned_.fetch_add(1);
    AddMetricPoint("ct_txs_orphaned", static_cast<double>(ct_txs_orphaned_.load()));
}

void SoakMetricsCollector::RecordProofVerification(double duration_ms) {
    total_proofs_verified_.fetch_add(1);
    AddMetricPoint("proof_verification_ms", duration_ms);
}

void SoakMetricsCollector::RecordMemoryUsage(uint64_t process, uint64_t heap, uint64_t mempool) {
    // Update memory samples under lock
    {
        std::lock_guard<std::mutex> lock(mutex_);
        memory_samples_.push_back({std::chrono::steady_clock::now(), process});
        if (memory_samples_.size() > MAX_HISTORY_POINTS) {
            memory_samples_.pop_front();
        }
    }

    // AddMetricPoint acquires its own lock, so call outside the lock above
    AddMetricPoint("process_memory", static_cast<double>(process));
    AddMetricPoint("heap_memory", static_cast<double>(heap));
    AddMetricPoint("mempool_memory", static_cast<double>(mempool));
}

void SoakMetricsCollector::RecordConsensusError() {
    consensus_errors_.fetch_add(1);
    AddMetricPoint("consensus_errors", static_cast<double>(consensus_errors_.load()));
}

void SoakMetricsCollector::RecordValidationError() {
    validation_errors_.fetch_add(1);
    AddMetricPoint("validation_errors", static_cast<double>(validation_errors_.load()));
}

void SoakMetricsCollector::RecordNetworkError() {
    network_errors_.fetch_add(1);
    AddMetricPoint("network_errors", static_cast<double>(network_errors_.load()));
}

SoakMetrics SoakMetricsCollector::GetCurrentMetrics() const {
    SoakMetrics metrics;
    metrics.chain_height = chain_height_.load();
    metrics.blocks_mined = blocks_mined_.load();
    metrics.ct_txs_created = ct_txs_created_.load();
    metrics.ct_txs_confirmed = ct_txs_confirmed_.load();
    metrics.ct_txs_orphaned = ct_txs_orphaned_.load();
    metrics.transparent_txs_created = transparent_txs_created_.load();
    metrics.transparent_txs_confirmed = transparent_txs_confirmed_.load();
    metrics.total_proofs_verified = total_proofs_verified_.load();
    metrics.consensus_errors = consensus_errors_.load();
    metrics.validation_errors = validation_errors_.load();
    metrics.network_errors = network_errors_.load();
    metrics.start_time = start_time_;
    metrics.last_update = std::chrono::steady_clock::now();

    // Calculate rates
    auto elapsed = std::chrono::duration_cast<std::chrono::minutes>(
        metrics.last_update - start_time_
    ).count();
    if (elapsed > 0) {
        metrics.blocks_per_minute = static_cast<double>(metrics.blocks_mined) / elapsed;
    }

    // Calculate average proof verification time
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = metric_history_.find("proof_verification_ms");
        if (it != metric_history_.end() && !it->second.empty()) {
            double sum = 0.0;
            double max_val = 0.0;
            for (const auto& point : it->second) {
                sum += point.value;
                max_val = std::max(max_val, point.value);
            }
            metrics.avg_proof_verification_ms = sum / it->second.size();
            metrics.peak_proof_verification_ms = max_val;
        }
    }

    return metrics;
}

std::vector<MetricPoint> SoakMetricsCollector::GetMetricHistory(const std::string& metric_name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = metric_history_.find(metric_name);
    if (it == metric_history_.end()) {
        return {};
    }
    return {it->second.begin(), it->second.end()};
}

std::vector<AnomalyReport> SoakMetricsCollector::CheckForAnomalies() const {
    std::vector<AnomalyReport> anomalies;
    auto now = std::chrono::steady_clock::now();

    // Check consensus errors
    if (consensus_errors_.load() > thresholds_.max_consensus_errors) {
        AnomalyReport report;
        report.has_anomaly = true;
        report.metric_name = "consensus_errors";
        report.description = "Consensus errors detected";
        report.expected_value = static_cast<double>(thresholds_.max_consensus_errors);
        report.actual_value = static_cast<double>(consensus_errors_.load());
        report.detected_at = now;
        anomalies.push_back(report);
    }

    // Check orphan rate
    uint64_t ct_total = ct_txs_created_.load();
    uint64_t ct_orphaned = ct_txs_orphaned_.load();
    if (ct_total > 0) {
        double orphan_rate = static_cast<double>(ct_orphaned) / ct_total;
        if (orphan_rate > thresholds_.max_orphan_rate) {
            AnomalyReport report;
            report.has_anomaly = true;
            report.metric_name = "orphan_rate";
            report.description = "CT orphan rate exceeds threshold";
            report.expected_value = thresholds_.max_orphan_rate;
            report.actual_value = orphan_rate;
            report.detected_at = now;
            anomalies.push_back(report);
        }
    }

    // Check memory growth
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (memory_samples_.size() >= 2) {
            auto oldest = memory_samples_.front();
            auto newest = memory_samples_.back();
            auto time_diff = std::chrono::duration_cast<std::chrono::hours>(
                newest.first - oldest.first
            ).count();
            if (time_diff > 0) {
                double growth_per_hour = static_cast<double>(newest.second - oldest.second) / time_diff;
                if (growth_per_hour > thresholds_.max_memory_growth_per_hour) {
                    AnomalyReport report;
                    report.has_anomaly = true;
                    report.metric_name = "memory_growth";
                    report.description = "Memory growth rate exceeds threshold";
                    report.expected_value = static_cast<double>(thresholds_.max_memory_growth_per_hour);
                    report.actual_value = growth_per_hour;
                    report.detected_at = now;
                    anomalies.push_back(report);
                }
            }
        }
    }

    return anomalies;
}

bool SoakMetricsCollector::HasCriticalAnomaly() const {
    auto anomalies = CheckForAnomalies();
    for (const auto& a : anomalies) {
        if (a.metric_name == "consensus_errors") {
            return true;
        }
    }
    return false;
}

void SoakMetricsCollector::Reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    chain_height_.store(0);
    blocks_mined_.store(0);
    ct_txs_created_.store(0);
    ct_txs_confirmed_.store(0);
    ct_txs_orphaned_.store(0);
    transparent_txs_created_.store(0);
    transparent_txs_confirmed_.store(0);
    total_proofs_verified_.store(0);
    consensus_errors_.store(0);
    validation_errors_.store(0);
    network_errors_.store(0);
    metric_history_.clear();
    memory_samples_.clear();
    start_time_ = std::chrono::steady_clock::now();
}

std::string SoakMetricsCollector::GenerateReport() const {
    auto metrics = GetCurrentMetrics();
    auto anomalies = CheckForAnomalies();

    std::ostringstream oss;
    oss << "=== CT Soak Test Metrics Report ===\n\n";

    oss << "Duration: " << metrics.elapsed().count() << " seconds\n\n";

    oss << "Chain State:\n";
    oss << "  Height: " << metrics.chain_height << "\n";
    oss << "  Blocks Mined: " << metrics.blocks_mined << "\n";
    oss << "  Blocks/Minute: " << std::fixed << std::setprecision(2) << metrics.blocks_per_minute << "\n\n";

    oss << "CT Transactions:\n";
    oss << "  Created: " << metrics.ct_txs_created << "\n";
    oss << "  Confirmed: " << metrics.ct_txs_confirmed << "\n";
    oss << "  Orphaned: " << metrics.ct_txs_orphaned << "\n\n";

    oss << "Transparent Transactions:\n";
    oss << "  Created: " << metrics.transparent_txs_created << "\n";
    oss << "  Confirmed: " << metrics.transparent_txs_confirmed << "\n\n";

    oss << "Proof Verification:\n";
    oss << "  Total Verified: " << metrics.total_proofs_verified << "\n";
    oss << "  Avg Time: " << std::fixed << std::setprecision(2) << metrics.avg_proof_verification_ms << " ms\n";
    oss << "  Peak Time: " << metrics.peak_proof_verification_ms << " ms\n\n";

    oss << "Errors:\n";
    oss << "  Consensus: " << metrics.consensus_errors << "\n";
    oss << "  Validation: " << metrics.validation_errors << "\n";
    oss << "  Network: " << metrics.network_errors << "\n\n";

    if (!anomalies.empty()) {
        oss << "Anomalies Detected:\n";
        for (const auto& a : anomalies) {
            oss << "  - " << a.metric_name << ": " << a.description << "\n";
            oss << "    Expected: " << a.expected_value << ", Actual: " << a.actual_value << "\n";
        }
    } else {
        oss << "No Anomalies Detected\n";
    }

    return oss.str();
}

std::string SoakMetricsCollector::GenerateSummary() const {
    auto metrics = GetCurrentMetrics();
    std::ostringstream oss;
    oss << "Height=" << metrics.chain_height
        << " CT=" << metrics.ct_txs_confirmed << "/" << metrics.ct_txs_created
        << " Errors=" << metrics.consensus_errors;
    return oss.str();
}

void SoakMetricsCollector::AddMetricPoint(const std::string& name, double value) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto& history = metric_history_[name];
    history.push_back({std::chrono::steady_clock::now(), value});
    if (history.size() > MAX_HISTORY_POINTS) {
        history.pop_front();
    }
}

double SoakMetricsCollector::CalculateRate(
    const std::deque<MetricPoint>& points,
    std::chrono::seconds window
) const {
    if (points.size() < 2) return 0.0;

    auto now = std::chrono::steady_clock::now();
    auto cutoff = now - window;

    double sum = 0.0;
    int count = 0;
    for (const auto& p : points) {
        if (p.timestamp >= cutoff) {
            sum += p.value;
            count++;
        }
    }

    return count > 0 ? sum / count : 0.0;
}

} // namespace soak
} // namespace dinero
