#include "storage/storage_guardrails.h"
#include <iostream>
#include <sstream>
#include <fstream>
#include <algorithm>
#include <iomanip>

#ifdef __linux__
#include <syslog.h>
#endif

namespace dinero {
namespace storage {

StorageGuardrails::StorageGuardrails(const GuardrailConfig& config)
    : config_(config), hour_start_(std::chrono::steady_clock::now()) {
}

void StorageGuardrails::updateConfig(const GuardrailConfig& config) {
    std::lock_guard<std::mutex> lock(metrics_mutex_);
    config_ = config;
}

void StorageGuardrails::updateMetrics(const StorageMetrics& metrics) {
    {
        std::lock_guard<std::mutex> lock(metrics_mutex_);
        current_metrics_ = metrics;
        current_metrics_.last_update = std::chrono::steady_clock::now();
    }
    
    // Check guardrails with updated metrics
    checkGuardrails(metrics);
    
    // Update backpressure manager if connected
    if (backpressure_manager_) {
        backpressure_manager_->updateMetrics(
            metrics.compaction_debt_mb,
            metrics.disk_usage_percent,
            metrics.memory_usage_mb,
            metrics.open_fd_count
        );
    }
}

StorageMetrics StorageGuardrails::getCurrentMetrics() const {
    std::lock_guard<std::mutex> lock(metrics_mutex_);
    return current_metrics_;
}

void StorageGuardrails::addAlertHandler(AlertHandler handler) {
    std::lock_guard<std::mutex> lock(alerts_mutex_);
    alert_handlers_.push_back(std::move(handler));
}

void StorageGuardrails::removeAllAlertHandlers() {
    std::lock_guard<std::mutex> lock(alerts_mutex_);
    alert_handlers_.clear();
}

void StorageGuardrails::triggerAlert(AlertType type, AlertSeverity severity, const std::string& message,
                                   const std::string& component, const std::string& additional_info) {
    StorageAlert alert(type, severity, message, component);
    alert.additional_info = additional_info;
    emitAlert(alert);
}

bool StorageGuardrails::isHealthy() const {
    std::lock_guard<std::mutex> lock(metrics_mutex_);
    return current_metrics_.is_healthy;
}

std::vector<StorageAlert> StorageGuardrails::getActiveAlerts() const {
    std::lock_guard<std::mutex> lock(alerts_mutex_);
    
    std::vector<StorageAlert> active_alerts;
    auto now = std::chrono::steady_clock::now();
    auto cutoff = now - std::chrono::minutes(30); // Active = last 30 minutes
    
    for (const auto& alert : alert_history_) {
        if (alert.timestamp >= cutoff && alert.severity >= AlertSeverity::WARNING) {
            active_alerts.append(alert);
        }
    }
    
    return active_alerts;
}

std::vector<StorageAlert> StorageGuardrails::getRecentAlerts(std::chrono::minutes lookback) const {
    std::lock_guard<std::mutex> lock(alerts_mutex_);
    
    std::vector<StorageAlert> recent_alerts;
    auto now = std::chrono::steady_clock::now();
    auto cutoff = now - lookback;
    
    for (const auto& alert : alert_history_) {
        if (alert.timestamp >= cutoff) {
            recent_alerts.append(alert);
        }
    }
    
    return recent_alerts;
}

void StorageGuardrails::enableAutoRecovery(bool enable) {
    auto_recovery_enabled_ = enable;
}

size_t StorageGuardrails::getActiveAlertCount() const {
    return getActiveAlerts().size();
}

void StorageGuardrails::setBackpressureManager(std::shared_ptr<BackpressureManager> manager) {
    backpressure_manager_ = manager;
}

void StorageGuardrails::clearOldAlerts(std::chrono::hours retention) {
    std::lock_guard<std::mutex> lock(alerts_mutex_);
    
    auto now = std::chrono::steady_clock::now();
    auto cutoff = now - retention;
    
    alert_history_.erase(
        std::remove_if(alert_history_.begin(), alert_history_.end(),
                      [cutoff](const StorageAlert& alert) {
                          return alert.timestamp < cutoff;
                      }),
        alert_history_.end()
    );
}

void StorageGuardrails::resetCounters() {
    std::lock_guard<std::mutex> lock(metrics_mutex_);
    
    current_metrics_.total_writes = 0;
    current_metrics_.total_reads = 0;
    current_metrics_.failed_writes = 0;
    current_metrics_.failed_reads = 0;
    
    total_alerts_generated_ = 0;
    alerts_this_hour_ = 0;
    hour_start_ = std::chrono::steady_clock::now();
}

void StorageGuardrails::checkGuardrails(const StorageMetrics& metrics) {
    checkCompactionDebt(metrics.compaction_debt_mb);
    checkDiskUsage(metrics.disk_usage_percent);
    checkMemoryUsage(metrics.memory_usage_mb);
    checkFileDescriptors(metrics.open_fd_count);
    checkPerformance(metrics);
    checkDatabaseSize(metrics.db_size_mb);
}

void StorageGuardrails::checkCompactionDebt(size_t debt_mb) {
    AlertSeverity severity = AlertSeverity::INFO;
    
    if (debt_mb >= config_.compaction_debt_emergency_mb) {
        severity = AlertSeverity::EMERGENCY;
    } else if (debt_mb >= config_.compaction_debt_critical_mb) {
        severity = AlertSeverity::CRITICAL;
    } else if (debt_mb >= config_.compaction_debt_warning_mb) {
        severity = AlertSeverity::WARNING;
    } else {
        return; // No alert needed
    }
    
    std::ostringstream msg;
    msg << "Compaction debt high: " << debt_mb << "MB";
    
    StorageAlert alert(AlertType::COMPACTION_DEBT, severity, msg.str(), "storage");
    alert.current_value = debt_mb;
    alert.threshold_value = (severity == AlertSeverity::EMERGENCY) ? config_.compaction_debt_emergency_mb :
                           (severity == AlertSeverity::CRITICAL) ? config_.compaction_debt_critical_mb :
                           config_.compaction_debt_warning_mb;
    
    emitAlert(alert);
}

void StorageGuardrails::checkDiskUsage(double usage_percent) {
    AlertSeverity severity = AlertSeverity::INFO;
    
    if (usage_percent >= config_.disk_usage_emergency_percent) {
        severity = AlertSeverity::EMERGENCY;
    } else if (usage_percent >= config_.disk_usage_critical_percent) {
        severity = AlertSeverity::CRITICAL;
    } else if (usage_percent >= config_.disk_usage_warning_percent) {
        severity = AlertSeverity::WARNING;
    } else {
        return; // No alert needed
    }
    
    std::ostringstream msg;
    msg << "Disk usage high: " << std::fixed << std::setprecision(1) << usage_percent << "%";
    
    StorageAlert alert(AlertType::DISK_USAGE, severity, msg.str(), "storage");
    alert.current_value = usage_percent;
    alert.threshold_value = (severity == AlertSeverity::EMERGENCY) ? config_.disk_usage_emergency_percent :
                           (severity == AlertSeverity::CRITICAL) ? config_.disk_usage_critical_percent :
                           config_.disk_usage_warning_percent;
    
    emitAlert(alert);
}

void StorageGuardrails::checkMemoryUsage(size_t usage_mb) {
    AlertSeverity severity = AlertSeverity::INFO;
    
    if (usage_mb >= config_.memory_usage_emergency_mb) {
        severity = AlertSeverity::EMERGENCY;
    } else if (usage_mb >= config_.memory_usage_critical_mb) {
        severity = AlertSeverity::CRITICAL;
    } else if (usage_mb >= config_.memory_usage_warning_mb) {
        severity = AlertSeverity::WARNING;
    } else {
        return; // No alert needed
    }
    
    std::ostringstream msg;
    msg << "Memory usage high: " << usage_mb << "MB";
    
    StorageAlert alert(AlertType::MEMORY_USAGE, severity, msg.str(), "storage");
    alert.current_value = usage_mb;
    alert.threshold_value = (severity == AlertSeverity::EMERGENCY) ? config_.memory_usage_emergency_mb :
                           (severity == AlertSeverity::CRITICAL) ? config_.memory_usage_critical_mb :
                           config_.memory_usage_warning_mb;
    
    emitAlert(alert);
}

void StorageGuardrails::checkFileDescriptors(int fd_count) {
    AlertSeverity severity = AlertSeverity::INFO;
    
    if (fd_count >= config_.fd_emergency_count) {
        severity = AlertSeverity::EMERGENCY;
    } else if (fd_count >= config_.fd_critical_count) {
        severity = AlertSeverity::CRITICAL;
    } else if (fd_count >= config_.fd_warning_count) {
        severity = AlertSeverity::WARNING;
    } else {
        return; // No alert needed
    }
    
    std::ostringstream msg;
    msg << "File descriptor usage high: " << fd_count;
    
    StorageAlert alert(AlertType::FILE_DESCRIPTORS, severity, msg.str(), "storage");
    alert.current_value = fd_count;
    alert.threshold_value = (severity == AlertSeverity::EMERGENCY) ? config_.fd_emergency_count :
                           (severity == AlertSeverity::CRITICAL) ? config_.fd_critical_count :
                           config_.fd_warning_count;
    
    emitAlert(alert);
}

void StorageGuardrails::checkPerformance(const StorageMetrics& metrics) {
    // Check write latency
    if (metrics.avg_write_latency >= config_.write_latency_critical) {
        std::ostringstream msg;
        msg << "Write latency critical: " << metrics.avg_write_latency.count() << "ms";
        StorageAlert alert(AlertType::WRITE_LATENCY, AlertSeverity::CRITICAL, msg.str(), "storage");
        alert.current_value = metrics.avg_write_latency.count();
        alert.threshold_value = config_.write_latency_critical.count();
        emitAlert(alert);
    } else if (metrics.avg_write_latency >= config_.write_latency_warning) {
        std::ostringstream msg;
        msg << "Write latency high: " << metrics.avg_write_latency.count() << "ms";
        StorageAlert alert(AlertType::WRITE_LATENCY, AlertSeverity::WARNING, msg.str(), "storage");
        alert.current_value = metrics.avg_write_latency.count();
        alert.threshold_value = config_.write_latency_warning.count();
        emitAlert(alert);
    }
    
    // Check read latency
    if (metrics.avg_read_latency >= config_.read_latency_critical) {
        std::ostringstream msg;
        msg << "Read latency critical: " << metrics.avg_read_latency.count() << "ms";
        StorageAlert alert(AlertType::READ_LATENCY, AlertSeverity::CRITICAL, msg.str(), "storage");
        alert.current_value = metrics.avg_read_latency.count();
        alert.threshold_value = config_.read_latency_critical.count();
        emitAlert(alert);
    } else if (metrics.avg_read_latency >= config_.read_latency_warning) {
        std::ostringstream msg;
        msg << "Read latency high: " << metrics.avg_read_latency.count() << "ms";
        StorageAlert alert(AlertType::READ_LATENCY, AlertSeverity::WARNING, msg.str(), "storage");
        alert.current_value = metrics.avg_read_latency.count();
        alert.threshold_value = config_.read_latency_warning.count();
        emitAlert(alert);
    }
}

void StorageGuardrails::checkDatabaseSize(size_t size_mb) {
    AlertSeverity severity = AlertSeverity::INFO;
    
    if (size_mb >= config_.db_size_critical_mb) {
        severity = AlertSeverity::CRITICAL;
    } else if (size_mb >= config_.db_size_warning_mb) {
        severity = AlertSeverity::WARNING;
    } else {
        return; // No alert needed
    }
    
    std::ostringstream msg;
    msg << "Database size large: " << size_mb << "MB";
    
    StorageAlert alert(AlertType::DISK_USAGE, severity, msg.str(), "storage");
    alert.current_value = size_mb;
    alert.threshold_value = (severity == AlertSeverity::CRITICAL) ? config_.db_size_critical_mb :
                           config_.db_size_warning_mb;
    alert.additional_info = "Consider enabling pruning or archiving old data";
    
    emitAlert(alert);
}

void StorageGuardrails::emitAlert(const StorageAlert& alert) {
    if (!shouldEmitAlert(alert.type, alert.severity)) {
        return; // Rate limited
    }
    
    // Update rate limiting
    {
        std::lock_guard<std::mutex> lock(rate_limit_mutex_);
        last_alert_times_[{alert.type, alert.severity}] = alert.timestamp;
        
        // Check hourly rate limit
        auto now = std::chrono::steady_clock::now();
        if (now - hour_start_ >= std::chrono::hours(1)) {
            alerts_this_hour_ = 0;
            hour_start_ = now;
        }
        
        if (alerts_this_hour_ >= config_.max_alerts_per_hour) {
            return; // Hourly rate limit exceeded
        }
        
        alerts_this_hour_++;
    }
    
    // Store alert in history
    {
        std::lock_guard<std::mutex> lock(alerts_mutex_);
        alert_history_.append(alert);
        
        // Limit history size
        if (alert_history_.size() > 10000) {
            alert_history_.erase(alert_history_.begin(), alert_history_.begin() + 1000);
        }
    }
    
    total_alerts_generated_++;
    
    // Trigger auto-recovery if enabled and appropriate
    if (auto_recovery_enabled_ && alert.severity >= AlertSeverity::CRITICAL) {
        performAutoRecovery(alert);
    }
    
    // Notify all handlers
    {
        std::lock_guard<std::mutex> lock(alerts_mutex_);
        for (const auto& handler : alert_handlers_) {
            try {
                handler(alert);
            } catch (const std::exception& e) {
                std::cerr << "Alert handler failed: " << e.what() << std::endl;
            }
        }
    }
}

bool StorageGuardrails::shouldEmitAlert(AlertType type, AlertSeverity severity) const {
    std::lock_guard<std::mutex> lock(rate_limit_mutex_);
    
    auto key = std::make_pair(type, severity);
    auto it = last_alert_times_.find(key);
    
    if (it == last_alert_times_.end()) {
        return true; // First alert of this type/severity
    }
    
    auto now = std::chrono::steady_clock::now();
    auto elapsed = now - it->second;
    
    return elapsed >= config_.alert_cooldown;
}

void StorageGuardrails::performAutoRecovery(const StorageAlert& alert) {
    switch (alert.type) {
        case AlertType::COMPACTION_DEBT:
            if (config_.enable_auto_compaction) {
                triggerEmergencyCompaction();
            }
            break;
            
        case AlertType::DISK_USAGE:
        case AlertType::MEMORY_USAGE:
            if (config_.enable_emergency_cleanup) {
                triggerEmergencyCleanup();
            }
            break;
            
        default:
            // No auto-recovery for other alert types
            break;
    }
}

void StorageGuardrails::triggerEmergencyCompaction() {
    std::cout << "GUARDRAILS: Triggering emergency compaction due to high compaction debt" << std::endl;
    
    // Trigger backpressure manager emergency compaction
    if (backpressure_manager_) {
        backpressure_manager_->triggerEmergencyCompaction();
    }
    
    // Log the action
    triggerAlert(AlertType::COMPACTION_DEBT, AlertSeverity::INFO, 
                "Emergency compaction triggered", "auto_recovery");
}

void StorageGuardrails::triggerEmergencyCleanup() {
    std::cout << "GUARDRAILS: Triggering emergency cleanup due to resource pressure" << std::endl;
    
    // In a real implementation, this would:
    // 1. Clear old log files
    // 2. Compact databases
    // 3. Free cached data
    // 4. Trigger garbage collection
    
    triggerAlert(AlertType::DISK_USAGE, AlertSeverity::INFO,
                "Emergency cleanup triggered", "auto_recovery");
}

// Alert handler implementations
namespace alert_handlers {

AlertHandler createConsoleHandler(bool include_timestamp) {
    return [include_timestamp](const StorageAlert& alert) {
        std::string severity_str;
        switch (alert.severity) {
            case AlertSeverity::INFO: severity_str = "INFO"; break;
            case AlertSeverity::WARNING: severity_str = "WARN"; break;
            case AlertSeverity::CRITICAL: severity_str = "CRIT"; break;
            case AlertSeverity::EMERGENCY: severity_str = "EMRG"; break;
        }
        
        std::string type_str;
        switch (alert.type) {
            case AlertType::COMPACTION_DEBT: type_str = "COMPACTION"; break;
            case AlertType::DISK_USAGE: type_str = "DISK"; break;
            case AlertType::MEMORY_USAGE: type_str = "MEMORY"; break;
            case AlertType::FILE_DESCRIPTORS: type_str = "FDS"; break;
            case AlertType::WRITE_LATENCY: type_str = "WRITE_LAT"; break;
            case AlertType::READ_LATENCY: type_str = "READ_LAT"; break;
            case AlertType::CORRUPTION_DETECTED: type_str = "CORRUPT"; break;
            case AlertType::BACKUP_FAILURE: type_str = "BACKUP"; break;
            case AlertType::STORAGE_UNAVAILABLE: type_str = "UNAVAIL"; break;
        }
        
        std::ostringstream output;
        
        if (include_timestamp) {
            auto time_t = std::chrono::system_clock::to_time_t(
                std::chrono::system_clock::now()
            );
            output << std::put_time(std::localtime(&time_t), "%Y-%m-%d %H:%M:%S") << " ";
        }
        
        output << "[" << severity_str << "] [" << type_str << "] ";
        if (!alert.component.empty()) {
            output << "[" << alert.component << "] ";
        }
        output << alert.message;
        
        if (!alert.additional_info.empty()) {
            output << " (" << alert.additional_info << ")";
        }
        
        std::cout << output.str() << std::endl;
    };
}

AlertHandler createFileHandler(const std::string& log_file) {
    return [log_file](const StorageAlert& alert) {
        std::ofstream file(log_file, std::ios::app);
        if (!file.is_open()) {
            return;
        }
        
        auto time_t = std::chrono::system_clock::to_time_t(
            std::chrono::system_clock::now()
        );
        
        file << std::put_time(std::localtime(&time_t), "%Y-%m-%d %H:%M:%S")
             << " [" << static_cast<int>(alert.severity) << "] "
             << "[" << static_cast<int>(alert.type) << "] "
             << alert.component << " " << alert.message;
        
        if (!alert.additional_info.empty()) {
            file << " " << alert.additional_info;
        }
        
        file << std::endl;
    };
}

AlertHandler createSyslogHandler(const std::string& program_name) {
    return [program_name](const StorageAlert& alert) {
#ifdef __linux__
        int priority = LOG_INFO;
        switch (alert.severity) {
            case AlertSeverity::INFO: priority = LOG_INFO; break;
            case AlertSeverity::WARNING: priority = LOG_WARNING; break;
            case AlertSeverity::CRITICAL: priority = LOG_CRIT; break;
            case AlertSeverity::EMERGENCY: priority = LOG_EMERG; break;
        }
        
        openlog(program_name.c_str(), LOG_PID, LOG_DAEMON);
        syslog(priority, "%s: %s", alert.component.c_str(), alert.message.c_str());
        closelog();
#else
        // Fallback to console on non-Linux systems
        createConsoleHandler(true)(alert);
#endif
    };
}

AlertHandler createMetricsHandler() {
    return [](const StorageAlert& alert) {
        // In a real implementation, this would export metrics to Prometheus
        // For now, just log in a metrics-friendly format
        std::cout << "storage_alert_total{type=\"" << static_cast<int>(alert.type)
                  << "\",severity=\"" << static_cast<int>(alert.severity)
                  << "\",component=\"" << alert.component << "\"} 1" << std::endl;
    };
}

} // namespace alert_handlers

} // namespace storage
} // namespace dinero
