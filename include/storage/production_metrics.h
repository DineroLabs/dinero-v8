#pragma once

#include <string>
#include <unordered_map>
#include <chrono>
#include <atomic>
#include <mutex>

namespace dinero {
namespace storage {

/**
 * Production-ready Prometheus metrics for Dinero storage
 * 
 * Implements the exact metrics specified in the go-live checklist:
 * - Timers: din_storage_block_connect_ms, din_storage_db_write_ms
 * - Counters: din_storage_write_batches_total, din_utxos_created_total, din_reorg_events_total
 * - Gauges: din_utxo_set_size, din_db_size_bytes, din_compaction_debt_bytes, din_backpressure_level
 */
class ProductionMetrics {
public:
    ProductionMetrics();
    ~ProductionMetrics();
    
    // === Timer Metrics ===
    
    /**
     * Record block connect duration (din_storage_block_connect_ms)
     */
    void recordBlockConnectDuration(double duration_ms);
    
    /**
     * Record database write duration (din_storage_db_write_ms)
     */
    void recordDbWriteDuration(double duration_ms);
    
    // === Counter Metrics ===
    
    /**
     * Increment write batches counter (din_storage_write_batches_total)
     */
    void incrementWriteBatches();
    
    /**
     * Increment UTXOs created counter (din_utxos_created_total)
     */
    void incrementUtxosCreated(uint64_t count = 1);
    
    /**
     * Increment reorg events counter (din_reorg_events_total)
     */
    void incrementReorgEvents();
    
    // === Gauge Metrics ===
    
    /**
     * Set UTXO set size (din_utxo_set_size)
     */
    void setUtxoSetSize(uint64_t size);
    
    /**
     * Set database size (din_db_size_bytes)
     */
    void setDbSizeBytes(uint64_t size);
    
    /**
     * Set compaction debt (din_compaction_debt_bytes)
     */
    void setCompactionDebtBytes(uint64_t debt);
    
    /**
     * Set backpressure level (din_backpressure_level)
     * 0 = None, 1 = Throttle, 2 = Block, 3 = Emergency
     */
    void setBackpressureLevel(int level);
    
    // === Prometheus Export ===
    
    /**
     * Export metrics in Prometheus text format
     */
    std::string exportPrometheus() const;
    
    /**
     * Get metric value by name (for testing/debugging)
     */
    double getMetricJson::Value(const std::string& name) const;
    
    /**
     * Reset all metrics (for testing)
     */
    void reset();
    
    // === Alert Thresholds ===
    
    /**
     * Check if compaction debt exceeds warning threshold (1GB)
     */
    bool isCompactionDebtWarning() const;
    
    /**
     * Check if compaction debt exceeds critical threshold (2GB)
     */
    bool isCompactionDebtCritical() const;
    
    /**
     * Check if p99 block connect time exceeds threshold (500ms)
     */
    bool isBlockConnectP99Critical() const;
    
    /**
     * Get current reorg rate (reorgs per hour)
     */
    double getCurrentReorgRate() const;
    
private:
    mutable std::mutex mutex_;
    
    // Timer metrics (histograms)
    struct TimerMetric {
        std::atomic<uint64_t> count{0};
        std::atomic<double> sum{0.0};
        std::vector<double> samples; // For percentile calculation
        mutable std::mutex samples_mutex;
        
        void record(double value) {
            count++;
            sum += value;
            std::lock_guard<std::mutex> lock(samples_mutex);
            samples.push_back(value);
            // Keep only recent samples for percentile calculation
            if (samples.size() > 10000) {
                samples.erase(samples.begin(), samples.begin() + 5000);
            }
        }
        
        double getPercentile(double p) const {
            std::lock_guard<std::mutex> lock(samples_mutex);
            if (samples.empty()) return 0.0;
            
            auto sorted_samples = samples;
            std::sort(sorted_samples.begin(), sorted_samples.end());
            
            size_t index = static_cast<size_t>(p * (sorted_samples.size() - 1));
            return sorted_samples[index];
        }
    };
    
    TimerMetric block_connect_duration_;
    TimerMetric db_write_duration_;
    
    // Counter metrics
    std::atomic<uint64_t> write_batches_total_{0};
    std::atomic<uint64_t> utxos_created_total_{0};
    std::atomic<uint64_t> reorg_events_total_{0};
    
    // Gauge metrics
    std::atomic<uint64_t> utxo_set_size_{0};
    std::atomic<uint64_t> db_size_bytes_{0};
    std::atomic<uint64_t> compaction_debt_bytes_{0};
    std::atomic<int> backpressure_level_{0};
    
    // Reorg rate tracking
    struct ReorgEvent {
        std::chrono::system_clock::time_point timestamp;
    };
    std::vector<ReorgEvent> recent_reorgs_;
    mutable std::mutex reorgs_mutex_;
    
    // Helper methods
    std::string formatPrometheusMetric(const std::string& name, 
                                     const std::string& type,
                                     double value,
                                     const std::string& help = "") const;
    
    std::string formatPrometheusHistogram(const std::string& name,
                                        const TimerMetric& metric,
                                        const std::string& help = "") const;
    
    void cleanupOldReorgs();
};

/**
 * Global production metrics instance
 */
extern std::unique_ptr<ProductionMetrics> g_production_metrics;

/**
 * Initialize production metrics
 */
void InitializeProductionMetrics();

/**
 * Shutdown production metrics
 */
void ShutdownProductionMetrics();

/**
 * Convenience macros for metrics recording
 */
#define RECORD_BLOCK_CONNECT_DURATION(duration_ms) \
    do { \
        if (g_production_metrics) { \
            g_production_metrics->recordBlockConnectDuration(duration_ms); \
        } \
    } while(0)

#define RECORD_DB_WRITE_DURATION(duration_ms) \
    do { \
        if (g_production_metrics) { \
            g_production_metrics->recordDbWriteDuration(duration_ms); \
        } \
    } while(0)

#define INCREMENT_WRITE_BATCHES() \
    do { \
        if (g_production_metrics) { \
            g_production_metrics->incrementWriteBatches(); \
        } \
    } while(0)

#define INCREMENT_UTXOS_CREATED(count) \
    do { \
        if (g_production_metrics) { \
            g_production_metrics->incrementUtxosCreated(count); \
        } \
    } while(0)

#define INCREMENT_REORG_EVENTS() \
    do { \
        if (g_production_metrics) { \
            g_production_metrics->incrementReorgEvents(); \
        } \
    } while(0)

#define SET_UTXO_SET_SIZE(size) \
    do { \
        if (g_production_metrics) { \
            g_production_metrics->setUtxoSetSize(size); \
        } \
    } while(0)

#define SET_DB_SIZE_BYTES(size) \
    do { \
        if (g_production_metrics) { \
            g_production_metrics->setDbSizeBytes(size); \
        } \
    } while(0)

#define SET_COMPACTION_DEBT_BYTES(debt) \
    do { \
        if (g_production_metrics) { \
            g_production_metrics->setCompactionDebtBytes(debt); \
        } \
    } while(0)

#define SET_BACKPRESSURE_LEVEL(level) \
    do { \
        if (g_production_metrics) { \
            g_production_metrics->setBackpressureLevel(level); \
        } \
    } while(0)

} // namespace storage
} // namespace dinero
