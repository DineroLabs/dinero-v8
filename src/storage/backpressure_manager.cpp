#include "storage/backpressure_manager.h"
#include <algorithm>
#include <thread>
#include <iostream>
#include <mutex>
#include <cmath>

#ifdef DIN_ENABLE_ROCKSDB
#include <rocksdb/db.h>
#endif

namespace dinero {
namespace storage {

BackpressureManager::BackpressureManager(const BackpressureConfig& config, BackpressureSensors sensors)
    : config_(config), sensors_(sensors), ema_debt_mb_(0.0) {
    status_.store(BackpressureStatus{});
    auto now = std::chrono::steady_clock::now();
    level_enter_time_ = now;
    level_exit_time_ = now;
}

void BackpressureManager::updateConfig(const BackpressureConfig& config) {
    std::lock_guard<std::mutex> lock(mutex_);
    config_ = config;
    updateBackpressureLevel();
}

void BackpressureManager::updateMetrics(size_t compaction_debt_mb, double disk_usage_percent,
                                       size_t memory_usage_mb, int open_fd_count) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    BackpressureStatus current_status = status_.load();
    
    // Use sensors if available, otherwise use provided values
    if (sensors_.get_compaction_debt) {
        current_status.compaction_debt_mb = sensors_.get_compaction_debt() / (1024 * 1024);
    } else {
        current_status.compaction_debt_mb = compaction_debt_mb;
    }
    
    if (sensors_.get_disk_usage) {
        current_status.disk_usage_percent = sensors_.get_disk_usage();
    } else {
        current_status.disk_usage_percent = disk_usage_percent;
    }
    
    if (sensors_.get_memory_usage) {
        current_status.memory_usage_mb = sensors_.get_memory_usage();
    } else {
        current_status.memory_usage_mb = memory_usage_mb;
    }
    
    if (sensors_.get_fd_count) {
        current_status.open_fd_count = sensors_.get_fd_count();
    } else {
        current_status.open_fd_count = open_fd_count;
    }
    
    current_status.last_update = std::chrono::steady_clock::now();
    
    // Apply EMA smoothing to compaction debt (α=0.2)
    const double alpha = 0.2;
    ema_debt_mb_ = alpha * current_status.compaction_debt_mb + (1.0 - alpha) * ema_debt_mb_;
    
    // Store updated status
    status_.store(current_status);
    
    // Update backpressure level based on new metrics
    updateBackpressureLevel();
}

BackpressureStatus BackpressureManager::getStatus() const {
    return status_.load();
}

BackpressureLevel BackpressureManager::getCurrentLevel() const {
    return status_.load().level;
}

bool BackpressureManager::shouldThrottle() const {
    BackpressureLevel level = getCurrentLevel();
    return level >= BackpressureLevel::THROTTLE;
}

bool BackpressureManager::shouldBlock() const {
    BackpressureLevel level = getCurrentLevel();
    return level >= BackpressureLevel::BLOCK;
}

std::chrono::milliseconds BackpressureManager::getThrottleDelay() const {
    BackpressureStatus current_status = status_.load();
    return current_status.current_delay;
}

bool BackpressureManager::checkAndApplyBackpressure() {
    BackpressureStatus current_status = status_.load();
    
    if (current_status.level >= BackpressureLevel::BLOCK) {
        // Increment blocked operations counter
        current_status.blocked_operations++;
        status_.store(current_status);
        
        std::cout << "BACKPRESSURE: Operation blocked due to " 
                  << static_cast<int>(current_status.level) << " level backpressure" << std::endl;
        return false;
    }
    
    if (current_status.level >= BackpressureLevel::THROTTLE) {
        // Apply throttling delay
        auto delay = current_status.current_delay;
        if (delay.count() > 0) {
            std::this_thread::sleep_for(delay);
        }
        
        // Increment throttled operations counter
        current_status.throttled_operations++;
        status_.store(current_status);
        
        std::cout << "BACKPRESSURE: Operation throttled by " 
                  << delay.count() << "ms" << std::endl;
    }
    
    return true;
}

void BackpressureManager::triggerEmergencyCompaction() {
    std::cout << "BACKPRESSURE: Emergency compaction triggered!" << std::endl;
    
    // In a real implementation, this would trigger background compaction
    // For now, we just log the event and update the level
    std::lock_guard<std::mutex> lock(mutex_);
    updateBackpressureLevel();
}

void BackpressureManager::setCallback(BackpressureCallback callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    callback_ = std::move(callback);
}

void BackpressureManager::resetCounters() {
    std::lock_guard<std::mutex> lock(mutex_);
    BackpressureStatus current_status = status_.load();
    current_status.throttled_operations = 0;
    current_status.blocked_operations = 0;
    status_.store(current_status);
}

void BackpressureManager::updateBackpressureLevel() {
    BackpressureStatus current_status = status_.load();
    BackpressureLevel old_level = current_status.level;
    BackpressureLevel new_level = calculateLevel();
    
    if (old_level != new_level) {
        current_status.level = new_level;
        current_status.level_change_time = std::chrono::steady_clock::now();
        current_status.current_delay = calculateThrottleDelay(new_level);
        
        status_.store(current_status);
        
        // Log level change
        std::cout << "BACKPRESSURE: Level changed from " << static_cast<int>(old_level)
                  << " to " << static_cast<int>(new_level) << std::endl;
        
        // Trigger callback if set
        if (callback_) {
            callback_(old_level, new_level, current_status);
        }
    } else {
        // Update delay even if level hasn't changed (for progressive throttling)
        current_status.current_delay = calculateThrottleDelay(new_level);
        status_.store(current_status);
    }
}

BackpressureLevel BackpressureManager::calculateLevel() const {
    BackpressureStatus current_status = status_.load();
    auto now = std::chrono::steady_clock::now();
    
    // Use EMA smoothed debt for hysteresis calculations
    double smoothed_debt = ema_debt_mb_;
    
    // Check emergency conditions first
    if (current_status.disk_usage_percent >= config_.disk_critical_percent ||
        current_status.memory_usage_mb >= config_.memory_critical_mb ||
        current_status.open_fd_count >= config_.fd_critical_count) {
        return BackpressureLevel::EMERGENCY;
    }
    
    // Hysteresis for BLOCK level
    // Enter BLOCK when smoothed debt > 2GB for 5s; exit when < 1.5GB for 10s
    bool block_enter = smoothed_debt > config_.block_threshold_mb;
    bool block_exit = smoothed_debt < (config_.block_threshold_mb * 0.75);
    
    if (block_enter && (now - level_enter_time_) > std::chrono::seconds(5)) {
        return BackpressureLevel::BLOCK;
    }
    if (current_status.level == BackpressureLevel::BLOCK && 
        (!block_exit || (now - level_exit_time_) < std::chrono::seconds(10))) {
        return BackpressureLevel::BLOCK;
    }
    
    // Hysteresis for THROTTLE level  
    // Enter THROTTLE when smoothed debt > 1GB for 5s; exit when < 512MB for 10s
    bool throttle_enter = smoothed_debt > config_.throttle_threshold_mb ||
                         current_status.disk_usage_percent >= config_.disk_warning_percent ||
                         current_status.memory_usage_mb >= config_.memory_warning_mb ||
                         current_status.open_fd_count >= config_.fd_warning_count;
    bool throttle_exit = smoothed_debt < (config_.throttle_threshold_mb * 0.5);
    
    if (throttle_enter && (now - level_enter_time_) > std::chrono::seconds(5)) {
        return BackpressureLevel::THROTTLE;
    }
    if (current_status.level == BackpressureLevel::THROTTLE && 
        (!throttle_exit || (now - level_exit_time_) < std::chrono::seconds(10))) {
        return BackpressureLevel::THROTTLE;
    }
    
    // Check warning conditions
    if (smoothed_debt >= config_.warning_threshold_mb) {
        return BackpressureLevel::WARNING;
    }
    
    return BackpressureLevel::NONE;
}

std::chrono::milliseconds BackpressureManager::calculateThrottleDelay(BackpressureLevel level) const {
    if (level < BackpressureLevel::THROTTLE) {
        return std::chrono::milliseconds{0};
    }
    
    BackpressureStatus current_status = status_.load();
    
    // Calculate delay based on compaction debt severity
    double debt_ratio = static_cast<double>(current_status.compaction_debt_mb) / config_.throttle_threshold_mb;
    debt_ratio = std::min(debt_ratio, 10.0); // Cap at 10x threshold
    
    // Progressive delay: starts at min_throttle_delay, scales up with debt
    auto base_delay = config_.min_throttle_delay;
    auto max_delay = config_.max_throttle_delay;
    
    auto calculated_delay = std::chrono::milliseconds{
        static_cast<long>(base_delay.count() * debt_ratio)
    };
    
    return std::min(calculated_delay, max_delay);
}

// RocksDB compaction debt with version fallback
uint64_t BackpressureManager::pending_compaction_bytes(void* db_ptr, bool is_rocksdb) {
#ifdef DIN_ENABLE_ROCKSDB
    if (is_rocksdb && db_ptr) {
        rocksdb::DB* db = static_cast<rocksdb::DB*>(db_ptr);
        uint64_t v = 0;
        
        // Try newer property name first
        if (db->GetAggregatedIntProperty("rocksdb.estimate-pending-compaction-bytes", &v)) {
            return v;
        }
        
        // Fallback to older property name
        if (db->GetAggregatedIntProperty("rocksdb.estimated-pending-compaction-bytes", &v)) {
            return v;
        }
        
        // Check actual delayed write rate as escalation signal
        uint64_t bw = 0;
        if (db->GetAggregatedIntProperty("rocksdb.actual-delayed-write-rate", &bw) && bw > 0) {
            // If writes are being delayed, estimate high compaction debt
            return 2ULL * 1024 * 1024 * 1024; // 2GB estimate
        }
    }
#endif
    return 0;
}

// Default sensors implementation
BackpressureSensors BackpressureSensors::RocksOrLevelDefault() {
    BackpressureSensors sensors;
    
    // Default implementations that return reasonable values
    sensors.get_compaction_debt = []() -> uint64_t {
        // In production, this would query the actual storage backend
        return 0;
    };
    
    sensors.get_disk_usage = []() -> double {
        // In production, this would check actual disk usage
        return 50.0; // 50% default
    };
    
    sensors.get_memory_usage = []() -> size_t {
        // In production, this would check actual memory usage
        return 512; // 512MB default
    };
    
    sensors.get_fd_count = []() -> int {
        // In production, this would check actual FD count
        return 100; // 100 FDs default
    };
    
    return sensors;
}

// BackpressureGuard implementation
BackpressureGuard::BackpressureGuard(BackpressureManager& manager)
    : proceed_(true), applied_delay_{0} {
    
    // Check if we should proceed with the operation
    if (manager.shouldBlock()) {
        proceed_ = false;
        return;
    }
    
    // Apply throttling if needed
    if (manager.shouldThrottle()) {
        applied_delay_ = manager.getThrottleDelay();
        if (applied_delay_.count() > 0) {
            std::this_thread::sleep_for(applied_delay_);
        }
    }
    
    // Update manager statistics
    manager.checkAndApplyBackpressure();
}

} // namespace storage
} // namespace dinero
