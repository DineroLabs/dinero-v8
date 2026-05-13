#include "storage/alert_thresholds.h"
#include "storage/production_metrics.h"
#include <sys/statvfs.h>
#include <sstream>
#include <iostream>

namespace dinero {
namespace storage {

// Global instance
std::unique_ptr<AlertThresholds> g_alert_thresholds;

AlertThresholds::AlertThresholds() = default;
AlertThresholds::~AlertThresholds() = default;

void AlertThresholds::initialize() {
    // Compaction debt warning: > 1GB for 10 minutes
    alert_configs_.append({
        "compaction_debt_warning",
        AlertSeverity::WARNING,
        std::chrono::minutes(10),
        [this]() { return checkCompactionDebtWarning(); },
        "Compaction debt exceeds 1GB",
        "Consider manual compaction during low traffic"
    });
    
    // Compaction debt critical: > 2GB for 10 minutes
    alert_configs_.append({
        "compaction_debt_critical", 
        AlertSeverity::CRITICAL,
        std::chrono::minutes(10),
        [this]() { return checkCompactionDebtCritical(); },
        "Compaction debt exceeds 2GB",
        "Immediate manual compaction required"
    });
    
    // Block connect p99 critical: > 500ms for 5 minutes
    alert_configs_.append({
        "block_connect_p99_critical",
        AlertSeverity::CRITICAL,
        std::chrono::minutes(5),
        [this]() { return checkBlockConnectP99Critical(); },
        "p99 block connect time exceeds 500ms",
        "Check storage performance and consider scaling"
    });
    
    // Disk usage critical: > 90%
    alert_configs_.append({
        "disk_usage_critical",
        AlertSeverity::CRITICAL,
        std::chrono::seconds(0), // Immediate
        [this]() { return checkDiskUsageCritical(); },
        "Disk usage exceeds 90%",
        "Free up disk space immediately"
    });
    
    // Reorg rate warning: > 3/hour
    alert_configs_.append({
        "reorg_rate_warning",
        AlertSeverity::WARNING,
        std::chrono::minutes(5),
        [this]() { return checkReorgRateWarning(); },
        "Reorg rate exceeds 3 per hour",
        "Investigate network conditions and peer connectivity"
    });
}

void AlertThresholds::checkAlerts() {
    auto now = std::chrono::system_clock::now();
    
    for (const auto& config : alert_configs_) {
        auto& state = alert_states_[config.name];
        bool condition_met = config.condition();
        
        if (condition_met) {
            if (!state.active) {
                // First time condition is met
                state.first_triggered = now;
                state.active = true;
            }
            
            // Check if duration threshold is met
            auto elapsed = now - state.first_triggered;
            if (elapsed >= config.duration) {
                triggerAlert(config);
            }
        } else {
            if (state.active) {
                // Condition no longer met, resolve alert
                resolveAlert(config.name);
                state.active = false;
            }
        }
        
        state.last_checked = now;
    }
}

void AlertThresholds::setAlertCallback(std::function<void(const AlertConfig&)> callback) {
    alert_callback_ = callback;
}

std::string AlertThresholds::getAlertStatus() const {
    std::stringstream ss;
    ss << "=== Alert Status ===\n";
    
    int active_warnings = 0;
    int active_criticals = 0;
    
    for (const auto& [name, state] : alert_states_) {
        if (state.active) {
            // Find the config for this alert
            auto config_it = std::find_if(alert_configs_.begin(), alert_configs_.end(),
                                        [&name](const AlertConfig& config) {
                                            return config.name == name;
                                        });
            
            if (config_it != alert_configs_.end()) {
                if (config_it->severity == AlertSeverity::WARNING) {
                    active_warnings++;
                } else {
                    active_criticals++;
                }
                
                ss << "🚨 " << (config_it->severity == AlertSeverity::CRITICAL ? "CRITICAL" : "WARNING")
                   << ": " << config_it->description << "\n";
            }
        }
    }
    
    if (active_warnings == 0 && active_criticals == 0) {
        ss << "✅ No active alerts\n";
    } else {
        ss << "Active: " << active_criticals << " critical, " << active_warnings << " warnings\n";
    }
    
    return ss.str();
}

bool AlertThresholds::shouldRefuseNewBlocks() const {
    return checkDiskUsageRefusal();
}

bool AlertThresholds::checkCompactionDebtWarning() {
    if (!g_production_metrics) return false;
    return g_production_metrics->isCompactionDebtWarning();
}

bool AlertThresholds::checkCompactionDebtCritical() {
    if (!g_production_metrics) return false;
    return g_production_metrics->isCompactionDebtCritical();
}

bool AlertThresholds::checkBlockConnectP99Critical() {
    if (!g_production_metrics) return false;
    return g_production_metrics->isBlockConnectP99Critical();
}

bool AlertThresholds::checkDiskUsageCritical() {
    return getDiskUsagePercent() > 90.0;
}

bool AlertThresholds::checkDiskUsageRefusal() {
    return getDiskUsagePercent() > 95.0;
}

bool AlertThresholds::checkReorgRateWarning() {
    if (!g_production_metrics) return false;
    return g_production_metrics->getCurrentReorgRate() > 3.0;
}

double AlertThresholds::getDiskUsagePercent() const {
    // Get disk usage for data directory
    const char* data_dir = "/var/lib/dinero"; // TODO: Get from config
    
    struct statvfs stat;
    if (statvfs(data_dir, &stat) != 0) {
        return 0.0; // Error getting disk stats
    }
    
    uint64_t total_bytes = stat.f_blocks * stat.f_frsize;
    uint64_t available_bytes = stat.f_bavail * stat.f_frsize;
    uint64_t used_bytes = total_bytes - available_bytes;
    
    if (total_bytes == 0) return 0.0;
    
    return (static_cast<double>(used_bytes) / total_bytes) * 100.0;
}

void AlertThresholds::triggerAlert(const AlertConfig& config) {
    if (alert_callback_) {
        alert_callback_(config);
    }
    
    // Default console logging
    std::string severity_str = (config.severity == AlertSeverity::CRITICAL) ? "CRITICAL" : "WARNING";
    std::cout << "🚨 ALERT [" << severity_str << "] " << config.name 
              << ": " << config.description << std::endl;
    std::cout << "   Action: " << config.action << std::endl;
}

void AlertThresholds::resolveAlert(const std::string& alert_name) {
    std::cout << "✅ RESOLVED: " << alert_name << std::endl;
}

// Global functions
void InitializeAlertThresholds() {
    g_alert_thresholds = std::make_unique<AlertThresholds>();
    g_alert_thresholds->initialize();
}

void ShutdownAlertThresholds() {
    g_alert_thresholds.reset();
}

} // namespace storage
} // namespace dinero
