#include "storage/production_metrics.h"
#include <sstream>
#include <algorithm>
#include <iomanip>

namespace dinero {
namespace storage {

// Global instance
std::unique_ptr<ProductionMetrics> g_production_metrics;

ProductionMetrics::ProductionMetrics() = default;
ProductionMetrics::~ProductionMetrics() = default;

void ProductionMetrics::recordBlockConnectDuration(double duration_ms) {
    block_connect_duration_.record(duration_ms);
}

void ProductionMetrics::recordDbWriteDuration(double duration_ms) {
    db_write_duration_.record(duration_ms);
}

void ProductionMetrics::incrementWriteBatches() {
    write_batches_total_++;
}

void ProductionMetrics::incrementUtxosCreated(uint64_t count) {
    utxos_created_total_ += count;
}

void ProductionMetrics::incrementReorgEvents() {
    reorg_events_total_++;
    
    // Track reorg timing for rate calculation
    std::lock_guard<std::mutex> lock(reorgs_mutex_);
    recent_reorgs_.append({std::chrono::system_clock::now()});
    cleanupOldReorgs();
}

void ProductionMetrics::setUtxoSetSize(uint64_t size) {
    utxo_set_size_ = size;
}

void ProductionMetrics::setDbSizeBytes(uint64_t size) {
    db_size_bytes_ = size;
}

void ProductionMetrics::setCompactionDebtBytes(uint64_t debt) {
    compaction_debt_bytes_ = debt;
}

void ProductionMetrics::setBackpressureLevel(int level) {
    backpressure_level_ = level;
}

std::string ProductionMetrics::exportPrometheus() const {
    std::stringstream ss;
    
    // Timer metrics (histograms)
    ss << formatPrometheusHistogram("din_storage_block_connect_ms", 
                                   block_connect_duration_,
                                   "Time taken to connect a block to storage");
    
    ss << formatPrometheusHistogram("din_storage_db_write_ms",
                                   db_write_duration_,
                                   "Time taken for database write operations");
    
    // Counter metrics
    ss << formatPrometheusMetric("din_storage_write_batches_total", "counter",
                                write_batches_total_.load(),
                                "Total number of write batches processed");
    
    ss << formatPrometheusMetric("din_utxos_created_total", "counter",
                                utxos_created_total_.load(),
                                "Total number of UTXOs created");
    
    ss << formatPrometheusMetric("din_reorg_events_total", "counter",
                                reorg_events_total_.load(),
                                "Total number of blockchain reorganization events");
    
    // Gauge metrics
    ss << formatPrometheusMetric("din_utxo_set_size", "gauge",
                                utxo_set_size_.load(),
                                "Current size of the UTXO set");
    
    ss << formatPrometheusMetric("din_db_size_bytes", "gauge",
                                db_size_bytes_.load(),
                                "Current database size in bytes");
    
    ss << formatPrometheusMetric("din_compaction_debt_bytes", "gauge",
                                compaction_debt_bytes_.load(),
                                "Current compaction debt in bytes");
    
    ss << formatPrometheusMetric("din_backpressure_level", "gauge",
                                backpressure_level_.load(),
                                "Current backpressure level (0=None, 1=Throttle, 2=Block, 3=Emergency)");
    
    return ss.str();
}

double ProductionMetrics::getMetricJson::Value(const std::string& name) const {
    if (name == "din_storage_write_batches_total") {
        return static_cast<double>(write_batches_total_.load());
    } else if (name == "din_utxos_created_total") {
        return static_cast<double>(utxos_created_total_.load());
    } else if (name == "din_reorg_events_total") {
        return static_cast<double>(reorg_events_total_.load());
    } else if (name == "din_utxo_set_size") {
        return static_cast<double>(utxo_set_size_.load());
    } else if (name == "din_db_size_bytes") {
        return static_cast<double>(db_size_bytes_.load());
    } else if (name == "din_compaction_debt_bytes") {
        return static_cast<double>(compaction_debt_bytes_.load());
    } else if (name == "din_backpressure_level") {
        return static_cast<double>(backpressure_level_.load());
    } else if (name == "din_storage_block_connect_ms_p99") {
        return block_connect_duration_.getPercentile(0.99);
    } else if (name == "din_storage_db_write_ms_avg") {
        uint64_t count = db_write_duration_.count.load();
        if (count == 0) return 0.0;
        return db_write_duration_.sum.load() / count;
    }
    return 0.0;
}

void ProductionMetrics::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    
    // Reset timers
    block_connect_duration_.count = 0;
    block_connect_duration_.sum = 0.0;
    {
        std::lock_guard<std::mutex> samples_lock(block_connect_duration_.samples_mutex);
        block_connect_duration_.samples.clear();
    }
    
    db_write_duration_.count = 0;
    db_write_duration_.sum = 0.0;
    {
        std::lock_guard<std::mutex> samples_lock(db_write_duration_.samples_mutex);
        db_write_duration_.samples.clear();
    }
    
    // Reset counters
    write_batches_total_ = 0;
    utxos_created_total_ = 0;
    reorg_events_total_ = 0;
    
    // Reset gauges
    utxo_set_size_ = 0;
    db_size_bytes_ = 0;
    compaction_debt_bytes_ = 0;
    backpressure_level_ = 0;
    
    // Reset reorg tracking
    {
        std::lock_guard<std::mutex> reorgs_lock(reorgs_mutex_);
        recent_reorgs_.clear();
    }
}

bool ProductionMetrics::isCompactionDebtWarning() const {
    return compaction_debt_bytes_.load() > (1ULL * 1024 * 1024 * 1024); // 1GB
}

bool ProductionMetrics::isCompactionDebtCritical() const {
    return compaction_debt_bytes_.load() > (2ULL * 1024 * 1024 * 1024); // 2GB
}

bool ProductionMetrics::isBlockConnectP99Critical() const {
    return block_connect_duration_.getPercentile(0.99) > 500.0; // 500ms
}

double ProductionMetrics::getCurrentReorgRate() const {
    std::lock_guard<std::mutex> lock(reorgs_mutex_);
    
    auto now = std::chrono::system_clock::now();
    auto one_hour_ago = now - std::chrono::hours(1);
    
    // Count reorgs in the last hour
    int reorgs_last_hour = 0;
    for (const auto& reorg : recent_reorgs_) {
        if (reorg.timestamp >= one_hour_ago) {
            reorgs_last_hour++;
        }
    }
    
    return static_cast<double>(reorgs_last_hour);
}

std::string ProductionMetrics::formatPrometheusMetric(const std::string& name,
                                                    const std::string& type,
                                                    double value,
                                                    const std::string& help) const {
    std::stringstream ss;
    
    if (!help.empty()) {
        ss << "# HELP " << name << " " << help << "\n";
    }
    ss << "# TYPE " << name << " " << type << "\n";
    ss << name << " " << std::fixed << std::setprecision(0) << value << "\n";
    
    return ss.str();
}

std::string ProductionMetrics::formatPrometheusHistogram(const std::string& name,
                                                       const TimerMetric& metric,
                                                       const std::string& help) const {
    std::stringstream ss;
    
    if (!help.empty()) {
        ss << "# HELP " << name << " " << help << "\n";
    }
    ss << "# TYPE " << name << " histogram\n";
    
    // Histogram buckets (standard latency buckets in milliseconds)
    std::vector<double> buckets = {1, 5, 10, 25, 50, 100, 250, 500, 1000, 2500, 5000, 10000};
    
    uint64_t cumulative_count = 0;
    for (double bucket : buckets) {
        // Count samples <= bucket
        std::lock_guard<std::mutex> lock(metric.samples_mutex);
        uint64_t bucket_count = 0;
        for (double sample : metric.samples) {
            if (sample <= bucket) {
                bucket_count++;
            }
        }
        cumulative_count = bucket_count;
        
        ss << name << "_bucket{le=\"" << bucket << "\"} " << cumulative_count << "\n";
    }
    
    // +Inf bucket
    ss << name << "_bucket{le=\"+Inf\"} " << metric.count.load() << "\n";
    
    // Sum and count
    ss << name << "_sum " << std::fixed << std::setprecision(2) << metric.sum.load() << "\n";
    ss << name << "_count " << metric.count.load() << "\n";
    
    return ss.str();
}

void ProductionMetrics::cleanupOldReorgs() {
    auto now = std::chrono::system_clock::now();
    auto cutoff = now - std::chrono::hours(24); // Keep 24 hours of history
    
    recent_reorgs_.erase(
        std::remove_if(recent_reorgs_.begin(), recent_reorgs_.end(),
                      [cutoff](const ReorgEvent& event) {
                          return event.timestamp < cutoff;
                      }),
        recent_reorgs_.end()
    );
}

// Global functions
void InitializeProductionMetrics() {
    g_production_metrics = std::make_unique<ProductionMetrics>();
}

void ShutdownProductionMetrics() {
    g_production_metrics.reset();
}

} // namespace storage
} // namespace dinero
