#pragma once

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <unordered_map>
#include <mutex>

namespace dinero {
namespace storage {

/**
 * High-resolution timer for measuring operation latencies
 */
class StorageTimer {
public:
    StorageTimer() : start_time_(std::chrono::high_resolution_clock::now()) {}
    
    // Get elapsed time in microseconds
    uint64_t elapsedMicros() const {
        auto now = std::chrono::high_resolution_clock::now();
        return std::chrono::duration_cast<std::chrono::microseconds>(now - start_time_).count();
    }
    
    // Get elapsed time in milliseconds
    uint64_t elapsedMillis() const {
        return elapsedMicros() / 1000;
    }
    
    // Reset timer
    void reset() {
        start_time_ = std::chrono::high_resolution_clock::now();
    }

private:
    std::chrono::high_resolution_clock::time_point start_time_;
};

/**
 * RAII timer that automatically records duration on destruction
 */
class ScopedTimer {
public:
    using Callback = std::function<void(uint64_t micros)>;
    
    explicit ScopedTimer(Callback callback) : callback_(std::move(callback)) {}
    
    ~ScopedTimer() {
        if (callback_) {
            callback_(timer_.elapsedMicros());
        }
    }
    
    uint64_t elapsedMicros() const { return timer_.elapsedMicros(); }

private:
    StorageTimer timer_;
    Callback callback_;
};

/**
 * Thread-safe counter with atomic operations
 */
class StorageCounter {
public:
    StorageCounter() : value_(0) {}
    
    void increment(uint64_t delta = 1) { value_ += delta; }
    void decrement(uint64_t delta = 1) { value_ -= delta; }
    void set(uint64_t value) { value_ = value; }
    void reset() { value_ = 0; }
    
    uint64_t get() const { return value_.load(); }
    
    // Atomic increment and return new value
    uint64_t incrementAndGet(uint64_t delta = 1) {
        return value_ += delta;
    }

private:
    std::atomic<uint64_t> value_;
};

/**
 * Thread-safe gauge for current values
 */
class StorageGauge {
public:
    StorageGauge() : value_(0) {}
    
    void set(double value) { value_ = value; }
    void add(double delta) { 
        double current = value_.load();
        while (!value_.compare_exchange_weak(current, current + delta)) {
            // Retry on failure
        }
    }
    
    double get() const { return value_.load(); }

private:
    std::atomic<double> value_;
};

/**
 * Histogram for tracking distribution of values
 */
class StorageHistogram {
public:
    explicit StorageHistogram(const std::vector<double>& buckets);
    
    void observe(double value);
    void reset();
    
    // Get bucket counts
    std::vector<uint64_t> getBucketCounts() const;
    std::vector<double> getBuckets() const { return buckets_; }
    
    // Statistics
    uint64_t getCount() const { return count_.get(); }
    double getSum() const { return sum_.get(); }
    double getMean() const;
    
    // Percentiles (approximate)
    double getPercentile(double p) const;

private:
    std::vector<double> buckets_;
    std::vector<StorageCounter> bucket_counts_;
    StorageCounter count_;
    StorageGauge sum_;
    mutable std::mutex mutex_;
};

/**
 * Per-operation metrics collection
 */
struct OperationMetrics {
    // Counters
    StorageCounter total_operations;
    StorageCounter successful_operations;
    StorageCounter failed_operations;
    StorageCounter bytes_processed;
    
    // Latency histogram (microseconds)
    std::unique_ptr<StorageHistogram> latency_histogram;
    
    // Gauges
    StorageGauge current_operations; // In-flight operations
    StorageGauge avg_latency_micros;
    StorageGauge p95_latency_micros;
    StorageGauge p99_latency_micros;
    
    OperationMetrics();
    void recordOperation(uint64_t latency_micros, bool success, uint64_t bytes = 0);
    void updatePercentiles();
};

/**
 * Comprehensive storage metrics collector
 */
class StorageMetricsCollector {
public:
    StorageMetricsCollector();
    ~StorageMetricsCollector() = default;
    
    // Operation timing
    std::unique_ptr<ScopedTimer> timeOperation(const std::string& operation, uint64_t bytes = 0);
    void recordLatency(const std::string& operation, uint64_t micros, bool success = true, uint64_t bytes = 0);
    
    // Block-level metrics
    void recordBlockWrite(uint64_t latency_micros, bool success, uint64_t block_size);
    void recordBlockRead(uint64_t latency_micros, bool success, uint64_t block_size);
    void recordTipUpdate(uint64_t latency_micros, bool success, bool sync);
    
    // UTXO metrics
    void recordUTXOWrite(uint64_t latency_micros, bool success, uint64_t utxo_count);
    void recordUTXORead(uint64_t latency_micros, bool success, uint64_t utxo_count);
    void recordUTXODelete(uint64_t latency_micros, bool success, uint64_t utxo_count);
    
    // Batch operation metrics
    void recordBatchCommit(uint64_t latency_micros, bool success, uint64_t batch_size, bool sync);
    void recordBatchAbort(uint64_t batch_size);
    
    // Compaction metrics
    void recordCompaction(uint64_t latency_micros, bool success, uint64_t bytes_compacted);
    void recordCompactionDebt(uint64_t debt_bytes);
    
    // Iterator metrics
    void recordIteratorSeek(uint64_t latency_micros, bool success);
    void recordIteratorNext(uint64_t latency_micros, bool success);
    
    // Resource metrics
    void updateMemoryUsage(uint64_t bytes);
    void updateDiskUsage(uint64_t bytes);
    void updateFileDescriptorCount(int count);
    
    // Backup/Restore metrics
    void recordBackup(uint64_t latency_micros, bool success, uint64_t backup_size_bytes);
    void recordRestore(uint64_t latency_micros, bool success, uint64_t restore_size_bytes);
    
    // Error tracking
    void recordError(const std::string& operation, const std::string& error_type);
    
    // Metrics retrieval
    OperationMetrics* getOperationMetrics(const std::string& operation);
    std::unordered_map<std::string, std::unique_ptr<OperationMetrics>> getAllMetrics() const;
    
    // Summary statistics
    struct Summary {
        uint64_t total_operations = 0;
        uint64_t successful_operations = 0;
        uint64_t failed_operations = 0;
        uint64_t total_bytes_processed = 0;
        double success_rate = 0.0;
        uint64_t avg_latency_micros = 0;
        uint64_t p95_latency_micros = 0;
        uint64_t p99_latency_micros = 0;
        
        // Resource usage
        uint64_t current_memory_bytes = 0;
        uint64_t current_disk_bytes = 0;
        int current_fd_count = 0;
        uint64_t compaction_debt_bytes = 0;
        
        // Error counts
        std::unordered_map<std::string, uint64_t> error_counts;
    };
    
    Summary getSummary() const;
    
    // Maintenance
    void reset();
    void updatePercentiles(); // Recalculate all percentiles
    
    // Export for monitoring systems
    std::string exportPrometheus() const;
    std::string exportJSON() const;

private:
    std::unordered_map<std::string, std::unique_ptr<OperationMetrics>> operation_metrics_;
    
    // Resource gauges
    StorageGauge memory_usage_bytes_;
    StorageGauge disk_usage_bytes_;
    StorageGauge fd_count_;
    StorageGauge compaction_debt_bytes_;
    
    // Error counters
    std::unordered_map<std::string, StorageCounter> error_counters_;
    
    // Thread safety
    mutable std::mutex metrics_mutex_;
    mutable std::mutex error_mutex_;
    
    OperationMetrics* getOrCreateOperationMetrics(const std::string& operation);
};

/**
 * Global metrics instance
 */
extern std::unique_ptr<StorageMetricsCollector> g_storage_metrics;

/**
 * Initialize global metrics collector
 */
void InitializeStorageMetrics();

/**
 * Shutdown global metrics collector
 */
void ShutdownStorageMetrics();

/**
 * Convenience macros for timing operations
 */
#define STORAGE_TIME_OPERATION(op) \
    auto timer = g_storage_metrics ? g_storage_metrics->timeOperation(op) : nullptr

#define STORAGE_TIME_OPERATION_BYTES(op, bytes) \
    auto timer = g_storage_metrics ? g_storage_metrics->timeOperation(op, bytes) : nullptr

#define STORAGE_RECORD_ERROR(op, error) \
    if (g_storage_metrics) g_storage_metrics->recordError(op, error)

} // namespace storage
} // namespace dinero
