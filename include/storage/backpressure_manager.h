#pragma once

#include <atomic>
#include <chrono>
#include <memory>
#include <functional>

namespace dinero {
namespace storage {

/**
 * Backpressure thresholds and configuration
 */
struct BackpressureConfig {
    // Compaction debt thresholds (in MB)
    size_t warning_threshold_mb = 512;     // Start warning at 512MB debt
    size_t throttle_threshold_mb = 1024;   // Start throttling at 1GB debt
    size_t block_threshold_mb = 2048;      // Block writes at 2GB debt
    
    // Disk usage thresholds (percentage)
    double disk_warning_percent = 85.0;    // Warn at 85% disk usage
    double disk_critical_percent = 95.0;   // Critical at 95% disk usage
    
    // Memory thresholds (in MB)
    size_t memory_warning_mb = 1024;       // Warn at 1GB memory usage
    size_t memory_critical_mb = 2048;      // Critical at 2GB memory usage
    
    // File descriptor thresholds
    int fd_warning_count = 800;            // Warn at 800 open FDs
    int fd_critical_count = 950;           // Critical at 950 open FDs
    
    // Throttling parameters
    std::chrono::milliseconds min_throttle_delay{10};   // Minimum delay
    std::chrono::milliseconds max_throttle_delay{1000}; // Maximum delay
    
    // Backpressure response timeouts
    std::chrono::seconds compaction_timeout{30};        // Wait for compaction
    std::chrono::seconds emergency_timeout{5};          // Emergency response time
};

/**
 * Current backpressure state
 */
enum class BackpressureLevel {
    NONE = 0,      // No backpressure
    WARNING = 1,   // Warning level - log alerts
    THROTTLE = 2,  // Throttle level - slow down writes
    BLOCK = 3,     // Block level - reject new writes
    EMERGENCY = 4  // Emergency level - halt operations
};

/**
 * Backpressure metrics and status
 */
struct BackpressureStatus {
    BackpressureLevel level = BackpressureLevel::NONE;
    
    // Resource usage
    size_t compaction_debt_mb = 0;
    double disk_usage_percent = 0.0;
    size_t memory_usage_mb = 0;
    int open_fd_count = 0;
    
    // Throttling state
    std::chrono::milliseconds current_delay{0};
    size_t throttled_operations = 0;
    size_t blocked_operations = 0;
    
    // Timing
    std::chrono::steady_clock::time_point last_update;
    std::chrono::steady_clock::time_point level_change_time;
    
    BackpressureStatus() {
        auto now = std::chrono::steady_clock::now();
        last_update = now;
        level_change_time = now;
    }
};

/**
 * Callback for backpressure events
 */
using BackpressureCallback = std::function<void(BackpressureLevel old_level, BackpressureLevel new_level, const BackpressureStatus& status)>;

/**
 * Sensor interface for testing backpressure behavior
 */
struct BackpressureSensors {
    std::function<uint64_t()> get_compaction_debt;
    std::function<double()> get_disk_usage;
    std::function<size_t()> get_memory_usage;
    std::function<int()> get_fd_count;
    
    static BackpressureSensors RocksOrLevelDefault();
};

/**
 * Manages backpressure based on storage resource usage
 */
class BackpressureManager {
public:
    explicit BackpressureManager(const BackpressureConfig& config = BackpressureConfig{}, 
                                BackpressureSensors sensors = BackpressureSensors::RocksOrLevelDefault());
    ~BackpressureManager() = default;
    
    // Configuration
    void updateConfig(const BackpressureConfig& config);
    const BackpressureConfig& getConfig() const { return config_; }
    
    // Status monitoring
    void updateMetrics(size_t compaction_debt_mb, double disk_usage_percent, 
                      size_t memory_usage_mb, int open_fd_count);
    
    BackpressureStatus getStatus() const;
    BackpressureLevel getCurrentLevel() const;
    
    // Backpressure enforcement
    bool shouldThrottle() const;
    bool shouldBlock() const;
    std::chrono::milliseconds getThrottleDelay() const;
    
    // Apply backpressure (returns false if operation should be blocked)
    bool checkAndApplyBackpressure();
    
    // Force compaction trigger
    void triggerEmergencyCompaction();
    
    // Event callbacks
    void setCallback(BackpressureCallback callback);
    
    // Statistics
    size_t getTotalThrottledOperations() const { return status_.load().throttled_operations; }
    size_t getTotalBlockedOperations() const { return status_.load().blocked_operations; }
    
    // Reset counters
    void resetCounters();

private:
    void updateBackpressureLevel();
    BackpressureLevel calculateLevel() const;
    std::chrono::milliseconds calculateThrottleDelay(BackpressureLevel level) const;
    
    // RocksDB compaction debt with version fallback
    static uint64_t pending_compaction_bytes(void* db_ptr, bool is_rocksdb);
    
    // Hysteresis and smoothing
    double ema_debt_mb_ = 0.0;
    std::chrono::steady_clock::time_point level_enter_time_;
    std::chrono::steady_clock::time_point level_exit_time_;
    
    BackpressureConfig config_;
    BackpressureSensors sensors_;
    mutable std::atomic<BackpressureStatus> status_;
    BackpressureCallback callback_;
    
    // Thread safety
    mutable std::mutex mutex_;
};

/**
 * RAII helper for applying backpressure to operations
 */
class BackpressureGuard {
public:
    explicit BackpressureGuard(BackpressureManager& manager);
    ~BackpressureGuard() = default;
    
    // Check if operation should proceed
    bool shouldProceed() const { return proceed_; }
    
    // Get applied delay
    std::chrono::milliseconds getAppliedDelay() const { return applied_delay_; }

private:
    bool proceed_;
    std::chrono::milliseconds applied_delay_;
};

} // namespace storage
} // namespace dinero
