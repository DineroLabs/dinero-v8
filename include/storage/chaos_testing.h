#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <functional>
#include <random>
#include <atomic>
#include <chrono>

namespace dinero {
namespace storage {

/**
 * Chaos testing failure types
 */
enum class ChaosFailureType {
    WRITE_FAILURE,          // Simulate write operation failure
    READ_FAILURE,           // Simulate read operation failure
    CORRUPTION,             // Simulate data corruption
    NETWORK_PARTITION,      // Simulate network issues
    DISK_FULL,             // Simulate disk space exhaustion
    SLOW_OPERATION,        // Simulate slow I/O operations
    CRASH_SIMULATION,      // Simulate process crashes
    MEMORY_PRESSURE,       // Simulate memory exhaustion
    LOCK_CONTENTION,       // Simulate lock contention
    BATCH_FAILURE          // Simulate batch operation failure
};

/**
 * Chaos testing configuration
 */
struct ChaosConfig {
    bool enabled = false;
    double failure_probability = 0.01;  // 1% failure rate by default
    std::vector<ChaosFailureType> enabled_failures;
    uint32_t min_delay_ms = 0;
    uint32_t max_delay_ms = 1000;
    uint32_t failure_duration_ms = 5000;
    bool fail_pre_tip_writes = true;    // Focus on pre-tip write failures
    bool fail_post_tip_writes = false;
    std::string target_backend = "";    // Empty = all backends
};

/**
 * Chaos testing failure injection point
 */
struct ChaosInjectionPoint {
    std::string operation_name;
    ChaosFailureType failure_type;
    std::string backend_name;
    std::chrono::system_clock::time_point timestamp;
    std::string failure_reason;
    uint32_t delay_ms = 0;
    bool should_fail = false;
};

/**
 * Chaos testing statistics
 */
struct ChaosStats {
    uint64_t total_operations = 0;
    uint64_t failed_operations = 0;
    uint64_t delayed_operations = 0;
    std::unordered_map<ChaosFailureType, uint64_t> failure_counts;
    std::chrono::system_clock::time_point start_time;
    std::chrono::system_clock::time_point last_failure;
};

/**
 * Chaos testing manager for storage operations
 * 
 * Provides controlled failure injection for testing storage system
 * resilience, with focus on pre-tip write failures and recovery.
 */
class ChaosTestingManager {
public:
    ChaosTestingManager();
    ~ChaosTestingManager();
    
    // === Configuration Management ===
    
    /**
     * Enable chaos testing with configuration
     */
    void enable(const ChaosConfig& config);
    
    /**
     * Disable chaos testing
     */
    void disable();
    
    /**
     * Check if chaos testing is enabled
     */
    bool isEnabled() const;
    
    /**
     * Update chaos configuration
     */
    void updateConfig(const ChaosConfig& config);
    
    /**
     * Get current configuration
     */
    ChaosConfig getConfig() const;
    
    // === Failure Injection ===
    
    /**
     * Check if operation should fail (main injection point)
     */
    ChaosInjectionPoint shouldInjectFailure(const std::string& operation,
                                           const std::string& backend = "");
    
    /**
     * Inject failure for pre-tip write operations
     */
    ChaosInjectionPoint checkPreTipWriteFailure(const std::string& operation,
                                               const std::string& backend = "");
    
    /**
     * Inject failure for post-tip write operations
     */
    ChaosInjectionPoint checkPostTipWriteFailure(const std::string& operation,
                                                const std::string& backend = "");
    
    /**
     * Inject failure for read operations
     */
    ChaosInjectionPoint checkReadFailure(const std::string& operation,
                                        const std::string& backend = "");
    
    /**
     * Inject failure for batch operations
     */
    ChaosInjectionPoint checkBatchFailure(const std::string& operation,
                                         size_t batch_size,
                                         const std::string& backend = "");
    
    // === Specific Failure Types ===
    
    /**
     * Simulate write failure
     */
    bool shouldFailWrite(const std::string& key, const std::string& backend = "");
    
    /**
     * Simulate read failure
     */
    bool shouldFailRead(const std::string& key, const std::string& backend = "");
    
    /**
     * Simulate corruption
     */
    bool shouldCorruptData(const std::string& key, const std::string& backend = "");
    
    /**
     * Simulate slow operation
     */
    uint32_t getOperationDelay(const std::string& operation, const std::string& backend = "");
    
    /**
     * Simulate disk full condition
     */
    bool shouldSimulateDiskFull(const std::string& backend = "");
    
    /**
     * Simulate memory pressure
     */
    bool shouldSimulateMemoryPressure(const std::string& backend = "");
    
    // === Statistics and Monitoring ===
    
    /**
     * Record operation attempt
     */
    void recordOperation(const std::string& operation, const std::string& backend = "");
    
    /**
     * Record failure injection
     */
    void recordFailure(const ChaosInjectionPoint& injection);
    
    /**
     * Get chaos testing statistics
     */
    ChaosStats getStatistics() const;
    
    /**
     * Reset statistics
     */
    void resetStatistics();
    
    /**
     * Generate chaos testing report
     */
    std::string generateReport() const;
    
    // === Advanced Features ===
    
    /**
     * Set custom failure probability for specific operation
     */
    void setOperationFailureProbability(const std::string& operation, double probability);
    
    /**
     * Set custom failure probability for specific backend
     */
    void setBackendFailureProbability(const std::string& backend, double probability);
    
    /**
     * Add custom failure scenario
     */
    void addFailureScenario(const std::string& name,
                           std::function<bool()> condition,
                           ChaosFailureType failure_type);
    
    /**
     * Remove failure scenario
     */
    void removeFailureScenario(const std::string& name);
    
    /**
     * Trigger specific failure type
     */
    void triggerFailure(ChaosFailureType failure_type,
                       const std::string& operation = "",
                       const std::string& backend = "");
    
    // === Recovery Testing ===
    
    /**
     * Test recovery from write failure
     */
    bool testWriteFailureRecovery(const std::string& key,
                                 const std::string& value,
                                 const std::string& backend = "");
    
    /**
     * Test recovery from corruption
     */
    bool testCorruptionRecovery(const std::string& key,
                               const std::string& backend = "");
    
    /**
     * Test recovery from batch failure
     */
    bool testBatchFailureRecovery(const std::vector<std::string>& keys,
                                 const std::string& backend = "");
    
    // === Environment Integration ===
    
    /**
     * Load configuration from environment variables
     */
    void loadFromEnvironment();
    
    /**
     * Load configuration from file
     */
    void loadFromFile(const std::string& config_file);
    
    /**
     * Save configuration to file
     */
    void saveToFile(const std::string& config_file) const;
    
    // === Callback Management ===
    
    /**
     * Set failure callback for notifications
     */
    void setFailureCallback(std::function<void(const ChaosInjectionPoint&)> callback);
    
    /**
     * Set recovery callback for notifications
     */
    void setRecoveryCallback(std::function<void(const std::string&)> callback);
    
private:
    mutable std::mutex mutex_;
    std::atomic<bool> enabled_{false};
    ChaosConfig config_;
    ChaosStats stats_;
    
    // Random number generation
    std::mt19937 rng_;
    std::uniform_real_distribution<double> probability_dist_;
    std::uniform_int_distribution<uint32_t> delay_dist_;
    
    // Custom failure probabilities
    std::unordered_map<std::string, double> operation_probabilities_;
    std::unordered_map<std::string, double> backend_probabilities_;
    
    // Custom failure scenarios
    struct FailureScenario {
        std::function<bool()> condition;
        ChaosFailureType failure_type;
    };
    std::unordered_map<std::string, FailureScenario> failure_scenarios_;
    
    // Callbacks
    std::function<void(const ChaosInjectionPoint&)> failure_callback_;
    std::function<void(const std::string&)> recovery_callback_;
    
    // Internal methods
    bool shouldInjectFailureInternal(const std::string& operation,
                                   const std::string& backend,
                                   ChaosFailureType failure_type);
    
    double getEffectiveFailureProbability(const std::string& operation,
                                        const std::string& backend) const;
    
    bool isOperationEnabled(ChaosFailureType failure_type) const;
    bool isBackendTargeted(const std::string& backend) const;
    
    void updateDelayDistribution();
    void notifyFailure(const ChaosInjectionPoint& injection);
    void notifyRecovery(const std::string& message);
};

/**
 * Global chaos testing instance
 */
extern std::unique_ptr<ChaosTestingManager> g_chaos_manager;

/**
 * Initialize chaos testing system
 */
void InitializeChaosTesting(const ChaosConfig& config = ChaosConfig{});

/**
 * Shutdown chaos testing system
 */
void ShutdownChaosTesting();

/**
 * Chaos testing macros for easy integration
 */
#define CHAOS_CHECK_FAILURE(operation, backend) \
    do { \
        if (g_chaos_manager && g_chaos_manager->isEnabled()) { \
            auto injection = g_chaos_manager->shouldInjectFailure(operation, backend); \
            if (injection.should_fail) { \
                if (injection.delay_ms > 0) { \
                    std::this_thread::sleep_for(std::chrono::milliseconds(injection.delay_ms)); \
                } \
                throw std::runtime_error("Chaos testing failure: " + injection.failure_reason); \
            } \
        } \
    } while(0)

#define CHAOS_CHECK_PRE_TIP_WRITE(operation, backend) \
    do { \
        if (g_chaos_manager && g_chaos_manager->isEnabled()) { \
            auto injection = g_chaos_manager->checkPreTipWriteFailure(operation, backend); \
            if (injection.should_fail) { \
                if (injection.delay_ms > 0) { \
                    std::this_thread::sleep_for(std::chrono::milliseconds(injection.delay_ms)); \
                } \
                throw std::runtime_error("Pre-tip write failure: " + injection.failure_reason); \
            } \
        } \
    } while(0)

#define CHAOS_RECORD_OPERATION(operation, backend) \
    do { \
        if (g_chaos_manager) { \
            g_chaos_manager->recordOperation(operation, backend); \
        } \
    } while(0)

/**
 * Environment variable configuration
 */
#define CHAOS_ENV_ENABLED "DINERO_CHAOS_ENABLED"
#define CHAOS_ENV_PROBABILITY "DINERO_CHAOS_PROBABILITY"
#define CHAOS_ENV_PRE_TIP_WRITES "DINERO_CHAOS_PRE_TIP_WRITES"
#define CHAOS_ENV_TARGET_BACKEND "DINERO_CHAOS_TARGET_BACKEND"
#define CHAOS_ENV_CONFIG_FILE "DINERO_CHAOS_CONFIG_FILE"

} // namespace storage
} // namespace dinero
