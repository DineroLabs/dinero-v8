#include "storage/storage_metrics.h"
#include <algorithm>
#include <sstream>
#include <iomanip>
#include <cmath>

namespace dinero {
namespace storage {

// Global metrics instance
std::unique_ptr<StorageMetricsCollector> g_storage_metrics;

// StorageHistogram implementation
StorageHistogram::StorageHistogram(const std::vector<double>& buckets) 
    : buckets_(buckets), bucket_counts_(buckets.size()) {
    std::sort(buckets_.begin(), buckets_.end());
}

void StorageHistogram::observe(double value) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    count_.increment();
    sum_.add(value);
    
    // Find appropriate bucket
    for (size_t i = 0; i < buckets_.size(); i++) {
        if (value <= buckets_[i]) {
            bucket_counts_[i].increment();
            break;
        }
    }
}

void StorageHistogram::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    
    count_.reset();
    sum_.set(0.0);
    for (auto& bucket : bucket_counts_) {
        bucket.reset();
    }
}

std::vector<uint64_t> StorageHistogram::getBucketCounts() const {
    std::lock_guard<std::mutex> lock(mutex_);
    
    std::vector<uint64_t> counts;
    counts.reserve(bucket_counts_.size());
    for (const auto& bucket : bucket_counts_) {
        counts.append(bucket.get());
    }
    return counts;
}

double StorageHistogram::getMean() const {
    uint64_t count = count_.get();
    return count > 0 ? sum_.get() / count : 0.0;
}

double StorageHistogram::getPercentile(double p) const {
    std::lock_guard<std::mutex> lock(mutex_);
    
    uint64_t total_count = count_.get();
    if (total_count == 0) return 0.0;
    
    uint64_t target_count = static_cast<uint64_t>(total_count * p / 100.0);
    uint64_t cumulative = 0;
    
    for (size_t i = 0; i < bucket_counts_.size(); i++) {
        cumulative += bucket_counts_[i].get();
        if (cumulative >= target_count) {
            return buckets_[i];
        }
    }
    
    return buckets_.empty() ? 0.0 : buckets_.back();
}

// OperationMetrics implementation
OperationMetrics::OperationMetrics() {
    // Create latency histogram with microsecond buckets
    std::vector<double> latency_buckets = {
        10, 25, 50, 100, 250, 500, 1000,      // 10μs to 1ms
        2500, 5000, 10000, 25000, 50000,     // 2.5ms to 50ms
        100000, 250000, 500000, 1000000,     // 100ms to 1s
        2500000, 5000000, 10000000           // 2.5s to 10s
    };
    latency_histogram = std::make_unique<StorageHistogram>(latency_buckets);
}

void OperationMetrics::recordOperation(uint64_t latency_micros, bool success, uint64_t bytes) {
    total_operations.increment();
    
    if (success) {
        successful_operations.increment();
    } else {
        failed_operations.increment();
    }
    
    if (bytes > 0) {
        bytes_processed.increment(bytes);
    }
    
    latency_histogram->observe(static_cast<double>(latency_micros));
    
    // Update running averages (simple exponential moving average)
    double current_avg = avg_latency_micros.get();
    double alpha = 0.1; // Smoothing factor
    avg_latency_micros.set(alpha * latency_micros + (1.0 - alpha) * current_avg);
}

void OperationMetrics::updatePercentiles() {
    p95_latency_micros.set(latency_histogram->getPercentile(95.0));
    p99_latency_micros.set(latency_histogram->getPercentile(99.0));
}

// StorageMetricsCollector implementation
StorageMetricsCollector::StorageMetricsCollector() {
    // Initialize with common operations
    getOrCreateOperationMetrics("block_write");
    getOrCreateOperationMetrics("block_read");
    getOrCreateOperationMetrics("utxo_write");
    getOrCreateOperationMetrics("utxo_read");
    getOrCreateOperationMetrics("utxo_delete");
    getOrCreateOperationMetrics("batch_commit");
    getOrCreateOperationMetrics("tip_update");
    getOrCreateOperationMetrics("compaction");
    getOrCreateOperationMetrics("iterator_seek");
    getOrCreateOperationMetrics("iterator_next");
}

std::unique_ptr<ScopedTimer> StorageMetricsCollector::timeOperation(const std::string& operation, uint64_t bytes) {
    return std::make_unique<ScopedTimer>([this, operation, bytes](uint64_t micros) {
        recordLatency(operation, micros, true, bytes);
    });
}

void StorageMetricsCollector::recordLatency(const std::string& operation, uint64_t micros, bool success, uint64_t bytes) {
    auto* metrics = getOrCreateOperationMetrics(operation);
    if (metrics) {
        metrics->recordOperation(micros, success, bytes);
    }
}

void StorageMetricsCollector::recordBlockWrite(uint64_t latency_micros, bool success, uint64_t block_size) {
    recordLatency("block_write", latency_micros, success, block_size);
}

void StorageMetricsCollector::recordBlockRead(uint64_t latency_micros, bool success, uint64_t block_size) {
    recordLatency("block_read", latency_micros, success, block_size);
}

void StorageMetricsCollector::recordTipUpdate(uint64_t latency_micros, bool success, bool sync) {
    std::string operation = sync ? "tip_update_sync" : "tip_update_async";
    recordLatency(operation, latency_micros, success);
}

void StorageMetricsCollector::recordUTXOWrite(uint64_t latency_micros, bool success, uint64_t utxo_count) {
    recordLatency("utxo_write", latency_micros, success, utxo_count);
}

void StorageMetricsCollector::recordUTXORead(uint64_t latency_micros, bool success, uint64_t utxo_count) {
    recordLatency("utxo_read", latency_micros, success, utxo_count);
}

void StorageMetricsCollector::recordUTXODelete(uint64_t latency_micros, bool success, uint64_t utxo_count) {
    recordLatency("utxo_delete", latency_micros, success, utxo_count);
}

void StorageMetricsCollector::recordBatchCommit(uint64_t latency_micros, bool success, uint64_t batch_size, bool sync) {
    std::string operation = sync ? "batch_commit_sync" : "batch_commit_async";
    recordLatency(operation, latency_micros, success, batch_size);
}

void StorageMetricsCollector::recordBatchAbort(uint64_t batch_size) {
    recordLatency("batch_abort", 0, true, batch_size);
}

void StorageMetricsCollector::recordCompaction(uint64_t latency_micros, bool success, uint64_t bytes_compacted) {
    recordLatency("compaction", latency_micros, success, bytes_compacted);
}

void StorageMetricsCollector::recordCompactionDebt(uint64_t debt_bytes) {
    compaction_debt_bytes_.set(static_cast<double>(debt_bytes));
}

void StorageMetricsCollector::recordIteratorSeek(uint64_t latency_micros, bool success) {
    recordLatency("iterator_seek", latency_micros, success);
}

void StorageMetricsCollector::recordIteratorNext(uint64_t latency_micros, bool success) {
    recordLatency("iterator_next", latency_micros, success);
}

void StorageMetricsCollector::updateMemoryUsage(uint64_t bytes) {
    memory_usage_bytes_.set(static_cast<double>(bytes));
}

void StorageMetricsCollector::updateDiskUsage(uint64_t bytes) {
    disk_usage_bytes_.set(static_cast<double>(bytes));
}

void StorageMetricsCollector::updateFileDescriptorCount(int count) {
    fd_count_.set(static_cast<double>(count));
}

void StorageMetricsCollector::recordError(const std::string& operation, const std::string& error_type) {
    std::lock_guard<std::mutex> lock(error_mutex_);
    
    std::string key = operation + ":" + error_type;
    error_counters_[key].increment();
    
    // Also record as failed operation
    recordLatency(operation, 0, false);
}

OperationMetrics* StorageMetricsCollector::getOperationMetrics(const std::string& operation) {
    std::lock_guard<std::mutex> lock(metrics_mutex_);
    
    auto it = operation_metrics_.find(operation);
    return (it != operation_metrics_.end()) ? it->second.get() : nullptr;
}

std::unordered_map<std::string, std::unique_ptr<OperationMetrics>> StorageMetricsCollector::getAllMetrics() const {
    std::lock_guard<std::mutex> lock(metrics_mutex_);
    
    std::unordered_map<std::string, std::unique_ptr<OperationMetrics>> result;
    for (const auto& pair : operation_metrics_) {
        // Note: This creates a shallow copy - in practice you'd want deep copy or different approach
        result[pair.first] = std::unique_ptr<OperationMetrics>(nullptr);
    }
    return result;
}

StorageMetricsCollector::Summary StorageMetricsCollector::getSummary() const {
    Summary summary;
    
    {
        std::lock_guard<std::mutex> lock(metrics_mutex_);
        
        for (const auto& pair : operation_metrics_) {
            const auto& metrics = pair.second;
            summary.total_operations += metrics->total_operations.get();
            summary.successful_operations += metrics->successful_operations.get();
            summary.failed_operations += metrics->failed_operations.get();
            summary.total_bytes_processed += metrics->bytes_processed.get();
            
            // Weighted average latency
            uint64_t ops = metrics->total_operations.get();
            if (ops > 0) {
                summary.avg_latency_micros += static_cast<uint64_t>(metrics->avg_latency_micros.get() * ops);
            }
        }
        
        if (summary.total_operations > 0) {
            summary.success_rate = static_cast<double>(summary.successful_operations) / summary.total_operations * 100.0;
            summary.avg_latency_micros /= summary.total_operations;
        }
    }
    
    // Resource usage
    summary.current_memory_bytes = static_cast<uint64_t>(memory_usage_bytes_.get());
    summary.current_disk_bytes = static_cast<uint64_t>(disk_usage_bytes_.get());
    summary.current_fd_count = static_cast<int>(fd_count_.get());
    summary.compaction_debt_bytes = static_cast<uint64_t>(compaction_debt_bytes_.get());
    
    // Error counts
    {
        std::lock_guard<std::mutex> lock(error_mutex_);
        for (const auto& pair : error_counters_) {
            summary.error_counts[pair.first] = pair.second.get();
        }
    }
    
    return summary;
}

void StorageMetricsCollector::reset() {
    {
        std::lock_guard<std::mutex> lock(metrics_mutex_);
        for (auto& pair : operation_metrics_) {
            pair.second->total_operations.reset();
            pair.second->successful_operations.reset();
            pair.second->failed_operations.reset();
            pair.second->bytes_processed.reset();
            pair.second->latency_histogram->reset();
            pair.second->avg_latency_micros.set(0.0);
            pair.second->p95_latency_micros.set(0.0);
            pair.second->p99_latency_micros.set(0.0);
        }
    }
    
    {
        std::lock_guard<std::mutex> lock(error_mutex_);
        for (auto& pair : error_counters_) {
            pair.second.reset();
        }
    }
    
    memory_usage_bytes_.set(0.0);
    disk_usage_bytes_.set(0.0);
    fd_count_.set(0.0);
    compaction_debt_bytes_.set(0.0);
}

void StorageMetricsCollector::updatePercentiles() {
    std::lock_guard<std::mutex> lock(metrics_mutex_);
    
    for (auto& pair : operation_metrics_) {
        pair.second->updatePercentiles();
    }
}

std::string StorageMetricsCollector::exportPrometheus() const {
    std::ostringstream output;
    
    {
        std::lock_guard<std::mutex> lock(metrics_mutex_);
        
        for (const auto& pair : operation_metrics_) {
            const std::string& operation = pair.first;
            const auto& metrics = pair.second;
            
            // Counter metrics
            output << "storage_operations_total{operation=\"" << operation << "\"} " 
                   << metrics->total_operations.get() << "\n";
            
            output << "storage_operations_successful_total{operation=\"" << operation << "\"} " 
                   << metrics->successful_operations.get() << "\n";
            
            output << "storage_operations_failed_total{operation=\"" << operation << "\"} " 
                   << metrics->failed_operations.get() << "\n";
            
            output << "storage_bytes_processed_total{operation=\"" << operation << "\"} " 
                   << metrics->bytes_processed.get() << "\n";
            
            // Gauge metrics
            output << "storage_operation_latency_microseconds{operation=\"" << operation << "\",quantile=\"0.50\"} " 
                   << metrics->avg_latency_micros.get() << "\n";
            
            output << "storage_operation_latency_microseconds{operation=\"" << operation << "\",quantile=\"0.95\"} " 
                   << metrics->p95_latency_micros.get() << "\n";
            
            output << "storage_operation_latency_microseconds{operation=\"" << operation << "\",quantile=\"0.99\"} " 
                   << metrics->p99_latency_micros.get() << "\n";
            
            // Histogram buckets
            auto buckets = metrics->latency_histogram->getBuckets();
            auto counts = metrics->latency_histogram->getBucketCounts();
            
            for (size_t i = 0; i < buckets.size(); i++) {
                output << "storage_operation_latency_microseconds_bucket{operation=\"" << operation 
                       << "\",le=\"" << buckets[i] << "\"} " << counts[i] << "\n";
            }
        }
    }
    
    // Resource metrics
    output << "storage_memory_usage_bytes " << memory_usage_bytes_.get() << "\n";
    output << "storage_disk_usage_bytes " << disk_usage_bytes_.get() << "\n";
    output << "storage_file_descriptors " << fd_count_.get() << "\n";
    output << "storage_compaction_debt_bytes " << compaction_debt_bytes_.get() << "\n";
    
    // Error metrics
    {
        std::lock_guard<std::mutex> lock(error_mutex_);
        for (const auto& pair : error_counters_) {
            output << "storage_errors_total{error=\"" << pair.first << "\"} " 
                   << pair.second.get() << "\n";
        }
    }
    
    return output.str();
}

std::string StorageMetricsCollector::exportJSON() const {
    std::ostringstream output;
    output << "{\n";
    
    // Operations
    output << "  \"operations\": {\n";
    bool first_op = true;
    
    {
        std::lock_guard<std::mutex> lock(metrics_mutex_);
        
        for (const auto& pair : operation_metrics_) {
            if (!first_op) output << ",\n";
            first_op = false;
            
            const std::string& operation = pair.first;
            const auto& metrics = pair.second;
            
            output << "    \"" << operation << "\": {\n";
            output << "      \"total_operations\": " << metrics->total_operations.get() << ",\n";
            output << "      \"successful_operations\": " << metrics->successful_operations.get() << ",\n";
            output << "      \"failed_operations\": " << metrics->failed_operations.get() << ",\n";
            output << "      \"bytes_processed\": " << metrics->bytes_processed.get() << ",\n";
            output << "      \"avg_latency_micros\": " << std::fixed << std::setprecision(2) 
                   << metrics->avg_latency_micros.get() << ",\n";
            output << "      \"p95_latency_micros\": " << std::fixed << std::setprecision(2) 
                   << metrics->p95_latency_micros.get() << ",\n";
            output << "      \"p99_latency_micros\": " << std::fixed << std::setprecision(2) 
                   << metrics->p99_latency_micros.get() << "\n";
            output << "    }";
        }
    }
    
    output << "\n  },\n";
    
    // Resources
    output << "  \"resources\": {\n";
    output << "    \"memory_usage_bytes\": " << static_cast<uint64_t>(memory_usage_bytes_.get()) << ",\n";
    output << "    \"disk_usage_bytes\": " << static_cast<uint64_t>(disk_usage_bytes_.get()) << ",\n";
    output << "    \"file_descriptors\": " << static_cast<int>(fd_count_.get()) << ",\n";
    output << "    \"compaction_debt_bytes\": " << static_cast<uint64_t>(compaction_debt_bytes_.get()) << "\n";
    output << "  },\n";
    
    // Errors
    output << "  \"errors\": {\n";
    bool first_error = true;
    
    {
        std::lock_guard<std::mutex> lock(error_mutex_);
        for (const auto& pair : error_counters_) {
            if (!first_error) output << ",\n";
            first_error = false;
            
            output << "    \"" << pair.first << "\": " << pair.second.get();
        }
    }
    
    output << "\n  }\n";
    output << "}\n";
    
    return output.str();
}

OperationMetrics* StorageMetricsCollector::getOrCreateOperationMetrics(const std::string& operation) {
    std::lock_guard<std::mutex> lock(metrics_mutex_);
    
    auto it = operation_metrics_.find(operation);
    if (it == operation_metrics_.end()) {
        operation_metrics_[operation] = std::make_unique<OperationMetrics>();
        return operation_metrics_[operation].get();
    }
    
    return it->second.get();
}

// Global functions
void InitializeStorageMetrics() {
    if (!g_storage_metrics) {
        g_storage_metrics = std::make_unique<StorageMetricsCollector>();
    }
}

void ShutdownStorageMetrics() {
    g_storage_metrics.reset();
}

} // namespace storage
} // namespace dinero
