#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <functional>
#include "storage_interface.h"

// Windows wingdi.h defines ERROR as a macro
#ifdef ERROR
#undef ERROR
#endif

namespace dinero {
namespace storage {

/**
 * Configuration safety levels
 */
enum class SafetyLevel {
    STRICT,     // No fallbacks, fail fast on any configuration issue
    SAFE,       // Limited fallbacks with explicit warnings
    PERMISSIVE, // Allow fallbacks with warnings
    UNSAFE      // Allow all fallbacks (not recommended for production)
};

/**
 * Configuration validation result
 */
enum class ValidationResult {
    VALID,
    WARNING,
    ERROR,
    CRITICAL_ERROR
};

/**
 * Configuration issue details
 */
struct ConfigIssue {
    ValidationResult severity;
    std::string category;       // e.g., "backend", "security", "performance"
    std::string key;           // Configuration key that has the issue
    std::string message;       // Human-readable description
    std::string suggestion;    // Recommended fix
    bool blocks_startup = false; // Whether this prevents startup
};

/**
 * Storage configuration validator and safety manager
 * 
 * Provides comprehensive validation of storage configuration with
 * configurable safety levels and clear warnings for production use.
 */
class ConfigSafety {
public:
    ConfigSafety();
    ~ConfigSafety();
    
    // === Safety Level Management ===
    
    /**
     * Set configuration safety level
     */
    void setSafetyLevel(SafetyLevel level);
    
    /**
     * Get current safety level
     */
    SafetyLevel getSafetyLevel() const;
    
    /**
     * Check if fallbacks are allowed at current safety level
     */
    bool areFallbacksAllowed() const;
    
    // === Configuration Validation ===
    
    /**
     * Validate storage configuration
     */
    std::vector<ConfigIssue> validateConfig(const StorageConfig& config);
    
    /**
     * Validate backend availability
     */
    std::vector<ConfigIssue> validateBackendAvailability(const std::string& backend);
    
    /**
     * Validate security configuration
     */
    std::vector<ConfigIssue> validateSecurityConfig(const StorageConfig& config);
    
    /**
     * Validate performance configuration
     */
    std::vector<ConfigIssue> validatePerformanceConfig(const StorageConfig& config);
    
    /**
     * Check for deprecated configuration options
     */
    std::vector<ConfigIssue> checkDeprecatedOptions(const StorageConfig& config);
    
    // === Fallback Management ===
    
    /**
     * Check if fallback is allowed for specific backend
     */
    bool isFallbackAllowed(const std::string& requested_backend, 
                          const std::string& fallback_backend);
    
    /**
     * Get allowed fallback chain for backend
     */
    std::vector<std::string> getFallbackChain(const std::string& backend);
    
    /**
     * Set custom fallback chain
     */
    void setFallbackChain(const std::string& backend, 
                         const std::vector<std::string>& fallbacks);
    
    /**
     * Disable fallbacks for specific backend
     */
    void disableFallbacks(const std::string& backend);
    
    // === Warning and Error Handling ===
    
    /**
     * Set warning callback for configuration issues
     */
    void setWarningCallback(std::function<void(const ConfigIssue&)> callback);
    
    /**
     * Set error callback for critical issues
     */
    void setErrorCallback(std::function<void(const ConfigIssue&)> callback);
    
    /**
     * Check if configuration blocks startup
     */
    bool doesConfigBlockStartup(const std::vector<ConfigIssue>& issues);
    
    /**
     * Generate startup safety report
     */
    std::string generateSafetyReport(const std::vector<ConfigIssue>& issues);
    
    // === Production Readiness ===
    
    /**
     * Check if configuration is production ready
     */
    bool isProductionReady(const StorageConfig& config);
    
    /**
     * Get production readiness checklist
     */
    std::vector<ConfigIssue> getProductionChecklist(const StorageConfig& config);
    
    /**
     * Validate environment variables
     */
    std::vector<ConfigIssue> validateEnvironment();
    
    /**
     * Check system requirements
     */
    std::vector<ConfigIssue> checkSystemRequirements();
    
    // === Configuration Recommendations ===
    
    /**
     * Get recommended configuration for environment
     */
    StorageConfig getRecommendedConfig(const std::string& environment);
    
    /**
     * Suggest configuration improvements
     */
    std::vector<ConfigIssue> suggestImprovements(const StorageConfig& config);
    
    /**
     * Get security hardening recommendations
     */
    std::vector<ConfigIssue> getSecurityRecommendations(const StorageConfig& config);
    
    // === Compliance and Standards ===
    
    /**
     * Check compliance with security standards
     */
    std::vector<ConfigIssue> checkSecurityCompliance(const StorageConfig& config);
    
    /**
     * Validate against best practices
     */
    std::vector<ConfigIssue> validateBestPractices(const StorageConfig& config);
    
    /**
     * Check for common misconfigurations
     */
    std::vector<ConfigIssue> checkCommonMisconfigurations(const StorageConfig& config);
    
    // === Configuration Templates ===
    
    /**
     * Get development configuration template
     */
    static StorageConfig getDevelopmentTemplate();
    
    /**
     * Get testing configuration template
     */
    static StorageConfig getTestingTemplate();
    
    /**
     * Get production configuration template
     */
    static StorageConfig getProductionTemplate();
    
    /**
     * Get high-availability configuration template
     */
    static StorageConfig getHighAvailabilityTemplate();
    
private:
    SafetyLevel safety_level_ = SafetyLevel::SAFE;
    std::unordered_map<std::string, std::vector<std::string>> fallback_chains_;
    std::function<void(const ConfigIssue&)> warning_callback_;
    std::function<void(const ConfigIssue&)> error_callback_;
    
    // Internal validation methods
    std::vector<ConfigIssue> validateBackendConfig(const StorageConfig& config);
    std::vector<ConfigIssue> validatePathConfig(const StorageConfig& config);
    std::vector<ConfigIssue> validateMemoryConfig(const StorageConfig& config);
    std::vector<ConfigIssue> validateNetworkConfig(const StorageConfig& config);
    
    // Security validation helpers
    bool isSecureDataDirectory(const std::string& path);
    bool hasProperPermissions(const std::string& path);
    bool isEncryptionEnabled(const StorageConfig& config);
    
    // System requirement checks
    bool hasMinimumDiskSpace(const std::string& path, uint64_t required_bytes);
    bool hasMinimumMemory(uint64_t required_bytes);
    bool hasSupportedFilesystem(const std::string& path);
    
    // Default fallback chains
    void initializeDefaultFallbacks();
    
    // Issue creation helpers
    ConfigIssue createIssue(ValidationResult severity, 
                           const std::string& category,
                           const std::string& key,
                           const std::string& message,
                           const std::string& suggestion = "",
                           bool blocks_startup = false);
};

/**
 * Global configuration safety instance
 */
extern std::unique_ptr<ConfigSafety> g_config_safety;

/**
 * Initialize configuration safety system
 */
void InitializeConfigSafety(SafetyLevel level = SafetyLevel::SAFE);

/**
 * Shutdown configuration safety system
 */
void ShutdownConfigSafety();

/**
 * Validate configuration with current safety settings
 */
std::vector<ConfigIssue> ValidateStorageConfig(const StorageConfig& config);

/**
 * Check if storage startup should proceed with given configuration
 */
bool ShouldProceedWithStartup(const StorageConfig& config);

/**
 * Print configuration safety report to console
 */
void PrintSafetyReport(const std::vector<ConfigIssue>& issues);

/**
 * Configuration safety macros for common checks
 */
#define CONFIG_REQUIRE(condition, message) \
    do { \
        if (!(condition)) { \
            throw std::runtime_error("Configuration requirement failed: " message); \
        } \
    } while(0)

#define CONFIG_WARN_IF(condition, message) \
    do { \
        if ((condition) && g_config_safety) { \
            ConfigIssue issue; \
            issue.severity = ValidationResult::WARNING; \
            issue.message = message; \
            /* Report warning */ \
        } \
    } while(0)

#define CONFIG_ERROR_IF(condition, message) \
    do { \
        if ((condition) && g_config_safety) { \
            ConfigIssue issue; \
            issue.severity = ValidationResult::ERROR; \
            issue.message = message; \
            issue.blocks_startup = true; \
            /* Report error */ \
        } \
    } while(0)

} // namespace storage
} // namespace dinero
