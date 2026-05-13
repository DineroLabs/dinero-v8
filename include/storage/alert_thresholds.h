#pragma once

#include <string>
#include <chrono>
#include <functional>

namespace dinero {
namespace storage {

/**
 * Alert severity levels
 */
enum class AlertSeverity {
    WARNING,
    CRITICAL
};

/**
 * Alert configuration for production monitoring
 */
struct AlertConfig {
    std::string name;
    AlertSeverity severity;
    std::chrono::seconds duration;
    std::function<bool()> condition;
    std::string description;
    std::string action;
};

/**
 * Production alert threshold manager
 * 
 * Implements the exact alert thresholds from the go-live checklist:
 * - Compaction debt > 1 GB (WARN) / 2 GB (CRIT) 10m
 * - p99 connect > 500 ms (CRIT) 5m  
 * - Disk usage > 90% (CRIT), > 95% refuse new blocks
 * - Reorgs > 3/hour (WARN)
 */
class AlertThresholds {
public:
    AlertThresholds();
    ~AlertThresholds();
    
    /**
     * Initialize alert monitoring
     */
    void initialize();
    
    /**
     * Check all alert conditions
     */
    void checkAlerts();
    
    /**
     * Set alert callback for notifications
     */
    void setAlertCallback(std::function<void(const AlertConfig&)> callback);
    
    /**
     * Get current alert status
     */
    std::string getAlertStatus() const;
    
    /**
     * Check if new blocks should be refused due to disk usage
     */
    bool shouldRefuseNewBlocks() const;
    
private:
    std::function<void(const AlertConfig&)> alert_callback_;
    std::vector<AlertConfig> alert_configs_;
    
    // Alert state tracking
    struct AlertState {
        bool active = false;
        std::chrono::system_clock::time_point first_triggered;
        std::chrono::system_clock::time_point last_checked;
    };
    std::unordered_map<std::string, AlertState> alert_states_;
    
    // Alert condition implementations
    bool checkCompactionDebtWarning();
    bool checkCompactionDebtCritical();
    bool checkBlockConnectP99Critical();
    bool checkDiskUsageCritical();
    bool checkDiskUsageRefusal();
    bool checkReorgRateWarning();
    
    // Helper methods
    double getDiskUsagePercent() const;
    void triggerAlert(const AlertConfig& config);
    void resolveAlert(const std::string& alert_name);
};

/**
 * Global alert thresholds instance
 */
extern std::unique_ptr<AlertThresholds> g_alert_thresholds;

/**
 * Initialize alert monitoring
 */
void InitializeAlertThresholds();

/**
 * Shutdown alert monitoring
 */
void ShutdownAlertThresholds();

} // namespace storage
} // namespace dinero
