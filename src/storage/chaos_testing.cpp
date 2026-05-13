#include "storage/chaos_testing.h"
#include <fstream>
#include <sstream>
#include <iostream>
#include <thread>
#include <cstdlib>
#include "compat/jsoncpp_compat.h"

namespace dinero {
namespace storage {

// Global instance
std::unique_ptr<ChaosTestingManager> g_chaos_manager;

ChaosTestingManager::ChaosTestingManager() 
    : rng_(std::random_device{}()),
      probability_dist_(0.0, 1.0),
      delay_dist_(0, 1000) {
    stats_.start_time = std::chrono::system_clock::now();
}

ChaosTestingManager::~ChaosTestingManager() = default;

void ChaosTestingManager::enable(const ChaosConfig& config) {
    std::lock_guard<std::mutex> lock(mutex_);
    config_ = config;
    enabled_ = true;
    updateDelayDistribution();
    
    // Reset statistics when enabling
    stats_ = ChaosStats{};
    stats_.start_time = std::chrono::system_clock::now();
}

void ChaosTestingManager::disable() {
    std::lock_guard<std::mutex> lock(mutex_);
    enabled_ = false;
}

bool ChaosTestingManager::isEnabled() const {
    return enabled_.load();
}

void ChaosTestingManager::updateConfig(const ChaosConfig& config) {
    std::lock_guard<std::mutex> lock(mutex_);
    config_ = config;
    updateDelayDistribution();
}

ChaosConfig ChaosTestingManager::getConfig() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return config_;
}

ChaosInjectionPoint ChaosTestingManager::shouldInjectFailure(const std::string& operation,
                                                           const std::string& backend) {
    if (!isEnabled()) {
        return {};
    }
    
    recordOperation(operation, backend);
    
    ChaosInjectionPoint injection;
    injection.operation_name = operation;
    injection.backend_name = backend;
    injection.timestamp = std::chrono::system_clock::now();
    
    // Check if this backend is targeted
    if (!isBackendTargeted(backend)) {
        return injection;
    }
    
    // Determine failure type based on operation
    ChaosFailureType failure_type = ChaosFailureType::WRITE_FAILURE;
    if (operation.find("read") != std::string::npos || operation.find("get") != std::string::npos) {
        failure_type = ChaosFailureType::READ_FAILURE;
    } else if (operation.find("batch") != std::string::npos) {
        failure_type = ChaosFailureType::BATCH_FAILURE;
    }
    
    injection.failure_type = failure_type;
    
    // Check if this failure type is enabled
    if (!isOperationEnabled(failure_type)) {
        return injection;
    }
    
    // Check probability
    double effective_probability = getEffectiveFailureProbability(operation, backend);
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (probability_dist_(rng_) < effective_probability) {
        injection.should_fail = true;
        injection.failure_reason = "Chaos testing failure injection";
        
        // Add delay if configured
        if (config_.max_delay_ms > config_.min_delay_ms) {
            std::uniform_int_distribution<uint32_t> delay_range(config_.min_delay_ms, config_.max_delay_ms);
            injection.delay_ms = delay_range(rng_);
        }
        
        recordFailure(injection);
        notifyFailure(injection);
    }
    
    return injection;
}

ChaosInjectionPoint ChaosTestingManager::checkPreTipWriteFailure(const std::string& operation,
                                                               const std::string& backend) {
    if (!isEnabled() || !config_.fail_pre_tip_writes) {
        return {};
    }
    
    // Pre-tip writes are critical operations before blockchain tip updates
    if (operation.find("pre_tip") != std::string::npos || 
        operation.find("prepare_tip") != std::string::npos ||
        operation.find("validate_tip") != std::string::npos) {
        
        return shouldInjectFailure(operation, backend);
    }
    
    return {};
}

ChaosInjectionPoint ChaosTestingManager::checkPostTipWriteFailure(const std::string& operation,
                                                                const std::string& backend) {
    if (!isEnabled() || !config_.fail_post_tip_writes) {
        return {};
    }
    
    // Post-tip writes are operations after blockchain tip updates
    if (operation.find("post_tip") != std::string::npos || 
        operation.find("finalize_tip") != std::string::npos ||
        operation.find("commit_tip") != std::string::npos) {
        
        return shouldInjectFailure(operation, backend);
    }
    
    return {};
}

ChaosInjectionPoint ChaosTestingManager::checkReadFailure(const std::string& operation,
                                                        const std::string& backend) {
    if (!isEnabled()) {
        return {};
    }
    
    return shouldInjectFailureInternal(operation, backend, ChaosFailureType::READ_FAILURE);
}

ChaosInjectionPoint ChaosTestingManager::checkBatchFailure(const std::string& operation,
                                                         size_t batch_size,
                                                         const std::string& backend) {
    if (!isEnabled()) {
        return {};
    }
    
    // Higher probability for larger batches
    double size_multiplier = std::min(2.0, 1.0 + (batch_size / 1000.0));
    
    std::lock_guard<std::mutex> lock(mutex_);
    double original_probability = config_.failure_probability;
    config_.failure_probability *= size_multiplier;
    
    auto injection = shouldInjectFailureInternal(operation, backend, ChaosFailureType::BATCH_FAILURE);
    
    config_.failure_probability = original_probability;
    return injection;
}

bool ChaosTestingManager::shouldFailWrite(const std::string& key, const std::string& backend) {
    auto injection = shouldInjectFailureInternal("write_" + key, backend, ChaosFailureType::WRITE_FAILURE);
    return injection.should_fail;
}

bool ChaosTestingManager::shouldFailRead(const std::string& key, const std::string& backend) {
    auto injection = shouldInjectFailureInternal("read_" + key, backend, ChaosFailureType::READ_FAILURE);
    return injection.should_fail;
}

bool ChaosTestingManager::shouldCorruptData(const std::string& key, const std::string& backend) {
    auto injection = shouldInjectFailureInternal("corrupt_" + key, backend, ChaosFailureType::CORRUPTION);
    return injection.should_fail;
}

uint32_t ChaosTestingManager::getOperationDelay(const std::string& operation, const std::string& backend) {
    auto injection = shouldInjectFailureInternal(operation, backend, ChaosFailureType::SLOW_OPERATION);
    return injection.delay_ms;
}

bool ChaosTestingManager::shouldSimulateDiskFull(const std::string& backend) {
    auto injection = shouldInjectFailureInternal("disk_operation", backend, ChaosFailureType::DISK_FULL);
    return injection.should_fail;
}

bool ChaosTestingManager::shouldSimulateMemoryPressure(const std::string& backend) {
    auto injection = shouldInjectFailureInternal("memory_operation", backend, ChaosFailureType::MEMORY_PRESSURE);
    return injection.should_fail;
}

void ChaosTestingManager::recordOperation(const std::string& operation, const std::string& backend) {
    std::lock_guard<std::mutex> lock(mutex_);
    stats_.total_operations++;
}

void ChaosTestingManager::recordFailure(const ChaosInjectionPoint& injection) {
    std::lock_guard<std::mutex> lock(mutex_);
    stats_.failed_operations++;
    stats_.failure_counts[injection.failure_type]++;
    stats_.last_failure = injection.timestamp;
    
    if (injection.delay_ms > 0) {
        stats_.delayed_operations++;
    }
}

ChaosStats ChaosTestingManager::getStatistics() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return stats_;
}

void ChaosTestingManager::resetStatistics() {
    std::lock_guard<std::mutex> lock(mutex_);
    stats_ = ChaosStats{};
    stats_.start_time = std::chrono::system_clock::now();
}

std::string ChaosTestingManager::generateReport() const {
    std::lock_guard<std::mutex> lock(mutex_);
    
    std::stringstream report;
    report << "=== Chaos Testing Report ===\n";
    report << "Enabled: " << (enabled_ ? "Yes" : "No") << "\n";
    
    if (!enabled_) {
        return report.str();
    }
    
    auto now = std::chrono::system_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::seconds>(now - stats_.start_time);
    
    report << "Duration: " << duration.count() << " seconds\n";
    report << "Total Operations: " << stats_.total_operations << "\n";
    report << "Failed Operations: " << stats_.failed_operations << "\n";
    report << "Delayed Operations: " << stats_.delayed_operations << "\n";
    
    if (stats_.total_operations > 0) {
        double failure_rate = (double)stats_.failed_operations / stats_.total_operations * 100.0;
        report << "Failure Rate: " << std::fixed << std::setprecision(2) << failure_rate << "%\n";
    }
    
    report << "\nFailure Breakdown:\n";
    for (const auto& [failure_type, count] : stats_.failure_counts) {
        std::string type_name;
        switch (failure_type) {
            case ChaosFailureType::WRITE_FAILURE: type_name = "Write Failures"; break;
            case ChaosFailureType::READ_FAILURE: type_name = "Read Failures"; break;
            case ChaosFailureType::CORRUPTION: type_name = "Corruption"; break;
            case ChaosFailureType::NETWORK_PARTITION: type_name = "Network Partition"; break;
            case ChaosFailureType::DISK_FULL: type_name = "Disk Full"; break;
            case ChaosFailureType::SLOW_OPERATION: type_name = "Slow Operations"; break;
            case ChaosFailureType::CRASH_SIMULATION: type_name = "Crash Simulation"; break;
            case ChaosFailureType::MEMORY_PRESSURE: type_name = "Memory Pressure"; break;
            case ChaosFailureType::LOCK_CONTENTION: type_name = "Lock Contention"; break;
            case ChaosFailureType::BATCH_FAILURE: type_name = "Batch Failures"; break;
        }
        report << "  " << type_name << ": " << count << "\n";
    }
    
    report << "\nConfiguration:\n";
    report << "  Failure Probability: " << config_.failure_probability << "\n";
    report << "  Pre-tip Write Failures: " << (config_.fail_pre_tip_writes ? "Enabled" : "Disabled") << "\n";
    report << "  Post-tip Write Failures: " << (config_.fail_post_tip_writes ? "Enabled" : "Disabled") << "\n";
    report << "  Target Backend: " << (config_.target_backend.empty() ? "All" : config_.target_backend) << "\n";
    report << "  Delay Range: " << config_.min_delay_ms << "-" << config_.max_delay_ms << "ms\n";
    
    return report.str();
}

void ChaosTestingManager::setOperationFailureProbability(const std::string& operation, double probability) {
    std::lock_guard<std::mutex> lock(mutex_);
    operation_probabilities_[operation] = probability;
}

void ChaosTestingManager::setBackendFailureProbability(const std::string& backend, double probability) {
    std::lock_guard<std::mutex> lock(mutex_);
    backend_probabilities_[backend] = probability;
}

void ChaosTestingManager::addFailureScenario(const std::string& name,
                                           std::function<bool()> condition,
                                           ChaosFailureType failure_type) {
    std::lock_guard<std::mutex> lock(mutex_);
    failure_scenarios_[name] = {condition, failure_type};
}

void ChaosTestingManager::removeFailureScenario(const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);
    failure_scenarios_.erase(name);
}

void ChaosTestingManager::triggerFailure(ChaosFailureType failure_type,
                                       const std::string& operation,
                                       const std::string& backend) {
    ChaosInjectionPoint injection;
    injection.operation_name = operation;
    injection.backend_name = backend;
    injection.failure_type = failure_type;
    injection.should_fail = true;
    injection.failure_reason = "Manually triggered failure";
    injection.timestamp = std::chrono::system_clock::now();
    
    recordFailure(injection);
    notifyFailure(injection);
}

void ChaosTestingManager::loadFromEnvironment() {
    const char* enabled_env = std::getenv(CHAOS_ENV_ENABLED);
    if (enabled_env && std::string(enabled_env) == "true") {
        ChaosConfig config;
        config.enabled = true;
        
        const char* probability_env = std::getenv(CHAOS_ENV_PROBABILITY);
        if (probability_env) {
            config.failure_probability = std::stod(probability_env);
        }
        
        const char* pre_tip_env = std::getenv(CHAOS_ENV_PRE_TIP_WRITES);
        if (pre_tip_env) {
            config.fail_pre_tip_writes = (std::string(pre_tip_env) == "true");
        }
        
        const char* backend_env = std::getenv(CHAOS_ENV_TARGET_BACKEND);
        if (backend_env) {
            config.target_backend = backend_env;
        }
        
        enable(config);
    }
    
    // Load from config file if specified
    const char* config_file_env = std::getenv(CHAOS_ENV_CONFIG_FILE);
    if (config_file_env) {
        loadFromFile(config_file_env);
    }
}

void ChaosTestingManager::loadFromFile(const std::string& config_file) {
    std::ifstream file(config_file);
    if (!file.is_open()) {
        return;
    }
    
    try {
        Json::Value j;
        file >> j;
        
        ChaosConfig config;
        config.enabled = j.isMember("enabled") ? "enabled" : false;
        config.failure_probability = j.isMember("failure_probability") ? "failure_probability" : 0.01;
        config.fail_pre_tip_writes = j.isMember("fail_pre_tip_writes") ? "fail_pre_tip_writes" : true;
        config.fail_post_tip_writes = j.isMember("fail_post_tip_writes") ? "fail_post_tip_writes" : false;
        config.target_backend = j.isMember("target_backend") ? "target_backend" : "";
        config.min_delay_ms = j.isMember("min_delay_ms") ? "min_delay_ms" : 0;
        config.max_delay_ms = j.isMember("max_delay_ms") ? "max_delay_ms" : 1000;
        config.failure_duration_ms = j.isMember("failure_duration_ms") ? "failure_duration_ms" : 5000;
        
        if (config.enabled) {
            enable(config);
        }
    } catch (const std::exception& e) {
        std::cerr << "Failed to load chaos config from " << config_file << ": " << e.what() << std::endl;
    }
}

void ChaosTestingManager::saveToFile(const std::string& config_file) const {
    std::lock_guard<std::mutex> lock(mutex_);
    
    Json::Value j;
    j["enabled"] = enabled_.load();
    j["failure_probability"] = config_.failure_probability;
    j["fail_pre_tip_writes"] = config_.fail_pre_tip_writes;
    j["fail_post_tip_writes"] = config_.fail_post_tip_writes;
    j["target_backend"] = config_.target_backend;
    j["min_delay_ms"] = config_.min_delay_ms;
    j["max_delay_ms"] = config_.max_delay_ms;
    j["failure_duration_ms"] = config_.failure_duration_ms;
    
    std::ofstream file(config_file);
    if (file.is_open()) {
        file << jJson::StyledWriter().write();
    }
}

void ChaosTestingManager::setFailureCallback(std::function<void(const ChaosInjectionPoint&)> callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    failure_callback_ = callback;
}

void ChaosTestingManager::setRecoveryCallback(std::function<void(const std::string&)> callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    recovery_callback_ = callback;
}

// Private methods

bool ChaosTestingManager::shouldInjectFailureInternal(const std::string& operation,
                                                    const std::string& backend,
                                                    ChaosFailureType failure_type) {
    if (!isEnabled() || !isOperationEnabled(failure_type) || !isBackendTargeted(backend)) {
        return false;
    }
    
    double effective_probability = getEffectiveFailureProbability(operation, backend);
    
    std::lock_guard<std::mutex> lock(mutex_);
    return probability_dist_(rng_) < effective_probability;
}

double ChaosTestingManager::getEffectiveFailureProbability(const std::string& operation,
                                                         const std::string& backend) const {
    double base_probability = config_.failure_probability;
    
    // Check operation-specific probability
    auto op_it = operation_probabilities_.find(operation);
    if (op_it != operation_probabilities_.end()) {
        base_probability = op_it->second;
    }
    
    // Check backend-specific probability
    auto backend_it = backend_probabilities_.find(backend);
    if (backend_it != backend_probabilities_.end()) {
        base_probability = std::max(base_probability, backend_it->second);
    }
    
    return base_probability;
}

bool ChaosTestingManager::isOperationEnabled(ChaosFailureType failure_type) const {
    if (config_.enabled_failures.empty()) {
        return true; // All failures enabled if none specified
    }
    
    return std::find(config_.enabled_failures.begin(), config_.enabled_failures.end(), failure_type) 
           != config_.enabled_failures.end();
}

bool ChaosTestingManager::isBackendTargeted(const std::string& backend) const {
    return config_.target_backend.empty() || config_.target_backend == backend;
}

void ChaosTestingManager::updateDelayDistribution() {
    delay_dist_ = std::uniform_int_distribution<uint32_t>(config_.min_delay_ms, config_.max_delay_ms);
}

void ChaosTestingManager::notifyFailure(const ChaosInjectionPoint& injection) {
    if (failure_callback_) {
        failure_callback_(injection);
    }
}

void ChaosTestingManager::notifyRecovery(const std::string& message) {
    if (recovery_callback_) {
        recovery_callback_(message);
    }
}

// Global functions

void InitializeChaosTesting(const ChaosConfig& config) {
    g_chaos_manager = std::make_unique<ChaosTestingManager>();
    
    // Load from environment first
    g_chaos_manager->loadFromEnvironment();
    
    // Override with provided config if enabled
    if (config.enabled) {
        g_chaos_manager->enable(config);
    }
}

void ShutdownChaosTesting() {
    g_chaos_manager.reset();
}

} // namespace storage
} // namespace dinero
