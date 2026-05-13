#pragma once

#include "storage/backpressure_manager.h"
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <mutex>

namespace dinero {
namespace storage {

/**
 * Alert severity levels
 */
enum class AlertSeverity {
    INFO = 0,
    WARNING = 1,
    CRITICAL = 2,
    EMERGENCY = 3
};

/**
 * Alert types for different storage conditions
 */
enum class AlertType {
    COMPACTION_DEBT,
    DISK_USAGE,
    MEMORY_USAGE,
    FILE_DESCRIPTORS,
    WRITE_LATENCY,
    READ_LATENCY,
    CORRUPTION_DETECTED,
    BACKUP_FAILURE,
    STORAGE_UNAVAILABLE
};

/**
 * Storage alert information
 */
struct StorageAlert {
    AlertType type;
    AlertSeverity severity;
    std::string message;
    std::chrono::steady_clock::time_point timestamp;
    std::string component; // e.g., "leveldb", "rocksdb", "storage_factory"
    
    // Alert-specific data
    double current_value = 0.0;
    double threshold_value = 0.0;
    std::string additional_info;
    
    StorageAlert(AlertType t, AlertSeverity s, const std::string& msg, const std::string& comp = "")
        : type(t), severity(s), message(msg), timestamp(std::chrono::steady_clock::now()), component(comp) {}
};

/**
 * Guardrail configuration for storage monitoring
 */
struct GuardrailConfig {
    // Compaction debt guardrails (MB)
    size_t compaction_debt_warning_mb = 256;
    size_t compaction_debt_critical_mb = 512;
    size_t compaction_debt_emergency_mb = 1024;
    
    // Disk usage guardrails (percentage)
    double disk_usage_warning_percent = 80.0;
    double disk_usage_critical_percent = 90.0;
    double disk_usage_emergency_percent = 95.0;
    
    // Memory usage guardrails (MB)
    size_t memory_usage_warning_mb = 512;
    size_t memory_usage_critical_mb = 1024;
    size_t memory_usage_emergency_mb = 2048;
    
    // File descriptor guardrails
    int fd_warning_count = 700;
    int fd_critical_count = 850;
    int fd_emergency_count = 950;
    
    // Performance guardrails (milliseconds)
    std::chrono::milliseconds write_latency_warning{100};
    std::chrono::milliseconds write_latency_critical{500};
    std::chrono::milliseconds read_latency_warning{50};
    std::chrono::milliseconds read_latency_critical{200};
    
    // Database size guardrails (MB)
    size_t db_size_warning_mb = 10240;  // 10GB
    size_t db_size_critical_mb = 51200; // 50GB
    
    // Alert rate limiting
    std::chrono::minutes alert_cooldown{5}; // Minimum time between same alerts
    size_t max_alerts_per_hour = 100;
    
    // Auto-recovery settings
    bool enable_auto_compaction = true;
    bool enable_emergency_cleanup = true;
    std::chrono::minutes auto_recovery_interval{10};
};

/**
 * Storage metrics for monitoring
 */
struct StorageMetrics {
    // Resource usage
    size_t compaction_debt_mb = 0;
    double disk_usage_percent = 0.0;
    size_t memory_usage_mb = 0;
    int open_fd_count = 0;
    size_t db_size_mb = 0;
    
    // Performance metrics
    std::chrono::milliseconds avg_write_latency{0};
    std::chrono::milliseconds avg_read_latency{0};
    std::chrono::milliseconds max_write_latency{0};
    std::chrono::milliseconds max_read_latency{0};
    
    // Operation counters
    uint64_t total_writes = 0;
    uint64_t total_reads = 0;
    uint64_t failed_writes = 0;
    uint64_t failed_reads = 0;
    
    // Health indicators
    bool is_healthy = true;
    bool compaction_running = false;
    bool backup_in_progress = false;
    std::chrono::steady_clock::time_point last_successful_backup;
    
    // Update timestamp
    std::chrono::steady_clock::time_point last_update;
    
    StorageMetrics() {
        last_update = std::chrono::steady_clock::now();
        last_successful_backup = std::chrono::steady_clock::now();
    }
};

/**
 * Alert handler callback
 */
using AlertHandler = std::function<void(const StorageAlert& alert)>;

/**
 * Storage guardrails monitor and alert system
 */
class StorageGuardrails {
public:
    explicit StorageGuardrails(const GuardrailConfig& config = GuardrailConfig{});
    ~StorageGuardrails() = default;
    
    // Configuration
    void updateConfig(const GuardrailConfig& config);
    const GuardrailConfig& getConfig() const { return config_; }
    
    // Metrics updates
    void updateMetrics(const StorageMetrics& metrics);
    StorageMetrics getCurrentMetrics() const;
    
    // Alert management
    void addAlertHandler(AlertHandler handler);
    void removeAllAlertHandlers();
    
    // Manual alerts
    void triggerAlert(AlertType type, AlertSeverity severity, const std::string& message,
                     const std::string& component = "", const std::string& additional_info = "");
    
    // Health checks
    bool isHealthy() const;
    std::vector<StorageAlert> getActiveAlerts() const;
    std::vector<StorageAlert> getRecentAlerts(std::chrono::minutes lookback = std::chrono::minutes{60}) const;
    
    // Auto-recovery actions
    void enableAutoRecovery(bool enable = true);
    bool isAutoRecoveryEnabled() const { return auto_recovery_enabled_; }
    
    // Statistics
    size_t getTotalAlertsGenerated() const { return total_alerts_generated_; }
    size_t getActiveAlertCount() const;
    
    // Integration with backpressure manager
    void setBackpressureManager(std::shared_ptr<BackpressureManager> manager);
    
    // Maintenance
    void clearOldAlerts(std::chrono::hours retention = std::chrono::hours{24});
    void resetCounters();

private:
    void checkGuardrails(const StorageMetrics& metrics);
    void checkCompactionDebt(size_t debt_mb);
    void checkDiskUsage(double usage_percent);
    void checkMemoryUsage(size_t usage_mb);
    void checkFileDescriptors(int fd_count);
    void checkPerformance(const StorageMetrics& metrics);
    void checkDatabaseSize(size_t size_mb);
    
    void emitAlert(const StorageAlert& alert);
    bool shouldEmitAlert(AlertType type, AlertSeverity severity) const;
    
    void performAutoRecovery(const StorageAlert& alert);
    void triggerEmergencyCompaction();
    void triggerEmergencyCleanup();
    
    AlertSeverity calculateSeverity(AlertType type, double current_value) const;
    
    GuardrailConfig config_;
    StorageMetrics current_metrics_;
    std::vector<AlertHandler> alert_handlers_;
    std::shared_ptr<BackpressureManager> backpressure_manager_;
    
    // Alert tracking
    std::vector<StorageAlert> alert_history_;
    std::atomic<size_t> total_alerts_generated_{0};
    std::atomic<bool> auto_recovery_enabled_{true};
    
    // Rate limiting
    mutable std::mutex rate_limit_mutex_;
    std::map<std::pair<AlertType, AlertSeverity>, std::chrono::steady_clock::time_point> last_alert_times_;
    std::atomic<size_t> alerts_this_hour_{0};
    std::chrono::steady_clock::time_point hour_start_;
    
    // Thread safety
    mutable std::mutex metrics_mutex_;
    mutable std::mutex alerts_mutex_;
};

/**
 * Default alert handlers for common integrations
 */
namespace alert_handlers {

/**
 * Console logger alert handler
 */
AlertHandler createConsoleHandler(bool include_timestamp = true);

/**
 * File logger alert handler
 */
AlertHandler createFileHandler(const std::string& log_file);

/**
 * Syslog alert handler (Linux/macOS)
 */
AlertHandler createSyslogHandler(const std::string& program_name = "dinero");

/**
 * Metrics export handler (for Prometheus integration)
 */
AlertHandler createMetricsHandler();

} // namespace alert_handlers

} // namespace storage
} // namespace dinero
