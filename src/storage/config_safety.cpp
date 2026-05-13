#include "storage/config_safety.h"
#include "storage/storage_factory.h"
#include "storage/storage_metrics.h"
#include <filesystem>
#include <iostream>
#include <sstream>
#include <algorithm>
#include <sys/statvfs.h>
#include <unistd.h>

namespace dinero {
namespace storage {

// Global instance
std::unique_ptr<ConfigSafety> g_config_safety;

ConfigSafety::ConfigSafety() {
    initializeDefaultFallbacks();
}

ConfigSafety::~ConfigSafety() = default;

void ConfigSafety::setSafetyLevel(SafetyLevel level) {
    safety_level_ = level;
}

SafetyLevel ConfigSafety::getSafetyLevel() const {
    return safety_level_;
}

bool ConfigSafety::areFallbacksAllowed() const {
    return safety_level_ != SafetyLevel::STRICT;
}

std::vector<ConfigIssue> ConfigSafety::validateConfig(const StorageConfig& config) {
    std::vector<ConfigIssue> issues;
    
    // Validate backend configuration
    auto backend_issues = validateBackendConfig(config);
    issues.insert(issues.end(), backend_issues.begin(), backend_issues.end());
    
    // Validate path configuration
    auto path_issues = validatePathConfig(config);
    issues.insert(issues.end(), path_issues.begin(), path_issues.end());
    
    // Validate security configuration
    auto security_issues = validateSecurityConfig(config);
    issues.insert(issues.end(), security_issues.begin(), security_issues.end());
    
    // Validate performance configuration
    auto perf_issues = validatePerformanceConfig(config);
    issues.insert(issues.end(), perf_issues.begin(), perf_issues.end());
    
    // Check for deprecated options
    auto deprecated_issues = checkDeprecatedOptions(config);
    issues.insert(issues.end(), deprecated_issues.begin(), deprecated_issues.end());
    
    return issues;
}

std::vector<ConfigIssue> ConfigSafety::validateBackendAvailability(const std::string& backend) {
    std::vector<ConfigIssue> issues;
    
    auto available_backends = StorageFactory::getAvailableBackends();
    bool backend_available = std::find(available_backends.begin(), available_backends.end(), backend) != available_backends.end();
    
    if (!backend_available) {
        bool blocks_startup = (safety_level_ == SafetyLevel::STRICT);
        
        issues.append(createIssue(
            blocks_startup ? ValidationResult::CRITICAL_ERROR : ValidationResult::WARNING,
            "backend",
            "backend_type",
            "Requested backend '" + backend + "' is not available",
            "Use one of: " + [&]() {
                std::string suggestion;
                for (size_t i = 0; i < available_backends.size(); i++) {
                    if (i > 0) suggestion += ", ";
                    suggestion += available_backends[i];
                }
                return suggestion;
            }(),
            blocks_startup
        ));
    }
    
    return issues;
}

std::vector<ConfigIssue> ConfigSafety::validateSecurityConfig(const StorageConfig& config) {
    std::vector<ConfigIssue> issues;
    
    // Check if fallbacks are disabled in production
    if (config.allow_fallback && safety_level_ == SafetyLevel::STRICT) {
        issues.append(createIssue(
            ValidationResult::CRITICAL_ERROR,
            "security",
            "allow_fallback",
            "Fallbacks are not allowed in STRICT safety mode",
            "Set allow_fallback=false for production environments",
            true
        ));
    }
    
    // Check data directory security
    if (!config.data_dir.empty() && !isSecureDataDirectory(config.data_dir)) {
        issues.append(createIssue(
            ValidationResult::WARNING,
            "security",
            "data_dir",
            "Data directory may not be secure: " + config.data_dir,
            "Ensure proper file permissions (750) and ownership"
        ));
    }
    
    // Check encryption settings
    if (!isEncryptionEnabled(config)) {
        ValidationResult severity = (safety_level_ == SafetyLevel::STRICT) ? 
            ValidationResult::ERROR : ValidationResult::WARNING;
        
        issues.append(createIssue(
            severity,
            "security",
            "encryption",
            "At-rest encryption is not enabled",
            "Enable encryption for production environments"
        ));
    }
    
    return issues;
}

std::vector<ConfigIssue> ConfigSafety::validatePerformanceConfig(const StorageConfig& config) {
    std::vector<ConfigIssue> issues;
    
    // Check memory configuration
    if (config.cache_size_mb < 64) {
        issues.append(createIssue(
            ValidationResult::WARNING,
            "performance",
            "cache_size_mb",
            "Cache size is very small: " + std::to_string(config.cache_size_mb) + "MB",
            "Consider increasing cache size to at least 256MB for better performance"
        ));
    }
    
    // Check write buffer size
    if (config.write_buffer_size_mb < 16) {
        issues.append(createIssue(
            ValidationResult::WARNING,
            "performance",
            "write_buffer_size_mb",
            "Write buffer size is small: " + std::to_string(config.write_buffer_size_mb) + "MB",
            "Consider increasing write buffer size for better write performance"
        ));
    }
    
    // Check if sync is disabled (dangerous for durability)
    if (!config.sync_writes) {
        ValidationResult severity = (safety_level_ == SafetyLevel::STRICT) ? 
            ValidationResult::ERROR : ValidationResult::WARNING;
        
        issues.append(createIssue(
            severity,
            "performance",
            "sync_writes",
            "Synchronous writes are disabled - data loss risk",
            "Enable sync_writes for production environments"
        ));
    }
    
    return issues;
}

std::vector<ConfigIssue> ConfigSafety::checkDeprecatedOptions(const StorageConfig& config) {
    std::vector<ConfigIssue> issues;
    
    // Check for deprecated backend names
    if (config.backend == "sqlite3") {
        issues.append(createIssue(
            ValidationResult::WARNING,
            "deprecated",
            "backend",
            "Backend name 'sqlite3' is deprecated",
            "Use 'sqlite' instead"
        ));
    }
    
    return issues;
}

bool ConfigSafety::isFallbackAllowed(const std::string& requested_backend, 
                                    const std::string& fallback_backend) {
    if (!areFallbacksAllowed()) {
        return false;
    }
    
    auto it = fallback_chains_.find(requested_backend);
    if (it == fallback_chains_.end()) {
        return false;
    }
    
    const auto& chain = it->second;
    return std::find(chain.begin(), chain.end(), fallback_backend) != chain.end();
}

std::vector<std::string> ConfigSafety::getFallbackChain(const std::string& backend) {
    auto it = fallback_chains_.find(backend);
    if (it != fallback_chains_.end()) {
        return it->second;
    }
    return {};
}

void ConfigSafety::setFallbackChain(const std::string& backend, 
                                   const std::vector<std::string>& fallbacks) {
    fallback_chains_[backend] = fallbacks;
}

void ConfigSafety::disableFallbacks(const std::string& backend) {
    fallback_chains_[backend].clear();
}

bool ConfigSafety::doesConfigBlockStartup(const std::vector<ConfigIssue>& issues) {
    return std::any_of(issues.begin(), issues.end(), 
                      [](const ConfigIssue& issue) { return issue.blocks_startup; });
}

std::string ConfigSafety::generateSafetyReport(const std::vector<ConfigIssue>& issues) {
    std::stringstream report;
    
    report << "=== Storage Configuration Safety Report ===\n";
    report << "Safety Level: ";
    switch (safety_level_) {
        case SafetyLevel::STRICT: report << "STRICT"; break;
        case SafetyLevel::SAFE: report << "SAFE"; break;
        case SafetyLevel::PERMISSIVE: report << "PERMISSIVE"; break;
        case SafetyLevel::UNSAFE: report << "UNSAFE"; break;
    }
    report << "\n";
    
    if (issues.empty()) {
        report << "✓ No configuration issues found\n";
        return report.str();
    }
    
    // Count issues by severity
    int critical = 0, errors = 0, warnings = 0;
    for (const auto& issue : issues) {
        switch (issue.severity) {
            case ValidationResult::CRITICAL_ERROR: critical++; break;
            case ValidationResult::ERROR: errors++; break;
            case ValidationResult::WARNING: warnings++; break;
            default: break;
        }
    }
    
    report << "Issues found: " << critical << " critical, " << errors << " errors, " << warnings << " warnings\n\n";
    
    // Report issues by severity
    for (const auto& severity : {ValidationResult::CRITICAL_ERROR, ValidationResult::ERROR, ValidationResult::WARNING}) {
        bool has_issues = false;
        for (const auto& issue : issues) {
            if (issue.severity == severity) {
                if (!has_issues) {
                    switch (severity) {
                        case ValidationResult::CRITICAL_ERROR: report << "CRITICAL ERRORS:\n"; break;
                        case ValidationResult::ERROR: report << "ERRORS:\n"; break;
                        case ValidationResult::WARNING: report << "WARNINGS:\n"; break;
                        default: break;
                    }
                    has_issues = true;
                }
                
                report << "  [" << issue.category << "] " << issue.message << "\n";
                if (!issue.suggestion.empty()) {
                    report << "    → " << issue.suggestion << "\n";
                }
                report << "\n";
            }
        }
    }
    
    bool blocks_startup = doesConfigBlockStartup(issues);
    if (blocks_startup) {
        report << "❌ STARTUP BLOCKED: Critical configuration issues must be resolved\n";
    } else {
        report << "✓ Startup allowed with warnings\n";
    }
    
    return report.str();
}

bool ConfigSafety::isProductionReady(const StorageConfig& config) {
    auto issues = getProductionChecklist(config);
    
    // Production ready if no critical errors or errors
    return std::none_of(issues.begin(), issues.end(), [](const ConfigIssue& issue) {
        return issue.severity == ValidationResult::CRITICAL_ERROR || 
               issue.severity == ValidationResult::ERROR;
    });
}

std::vector<ConfigIssue> ConfigSafety::getProductionChecklist(const StorageConfig& config) {
    std::vector<ConfigIssue> issues;
    
    // Must have fallbacks disabled
    if (config.allow_fallback) {
        issues.append(createIssue(
            ValidationResult::ERROR,
            "production",
            "allow_fallback",
            "Fallbacks should be disabled in production",
            "Set allow_fallback=false"
        ));
    }
    
    // Must have sync enabled
    if (!config.sync_writes) {
        issues.append(createIssue(
            ValidationResult::ERROR,
            "production",
            "sync_writes",
            "Synchronous writes must be enabled in production",
            "Set sync_writes=true"
        ));
    }
    
    // Should have encryption enabled
    if (!isEncryptionEnabled(config)) {
        issues.append(createIssue(
            ValidationResult::WARNING,
            "production",
            "encryption",
            "Encryption should be enabled in production",
            "Configure at-rest encryption"
        ));
    }
    
    // Check system requirements
    auto sys_issues = checkSystemRequirements();
    issues.insert(issues.end(), sys_issues.begin(), sys_issues.end());
    
    return issues;
}

StorageConfig ConfigSafety::getProductionTemplate() {
    StorageConfig config;
    config.backend = "rocksdb";
    config.allow_fallback = false;  // STRICT: No fallbacks in production
    config.sync_writes = true;
    config.cache_size_mb = 512;
    config.write_buffer_size_mb = 64;
    config.max_open_files = 1000;
    config.compression_enabled = true;
    config.bloom_filter_enabled = true;
    return config;
}

StorageConfig ConfigSafety::getDevelopmentTemplate() {
    StorageConfig config;
    config.backend = "leveldb";
    config.allow_fallback = true;
    config.sync_writes = false;  // Faster for development
    config.cache_size_mb = 128;
    config.write_buffer_size_mb = 16;
    config.max_open_files = 100;
    return config;
}

StorageConfig ConfigSafety::getTestingTemplate() {
    StorageConfig config;
    config.backend = "memory";
    config.allow_fallback = true;
    config.sync_writes = false;
    config.cache_size_mb = 64;
    config.write_buffer_size_mb = 8;
    return config;
}

// Private methods

std::vector<ConfigIssue> ConfigSafety::validateBackendConfig(const StorageConfig& config) {
    std::vector<ConfigIssue> issues;
    
    // Validate backend availability
    auto backend_issues = validateBackendAvailability(config.backend);
    issues.insert(issues.end(), backend_issues.begin(), backend_issues.end());
    
    // Check backend-specific requirements
    if (config.backend == "rocksdb") {
        if (config.cache_size_mb < 128) {
            issues.append(createIssue(
                ValidationResult::WARNING,
                "backend",
                "cache_size_mb",
                "RocksDB performs better with larger cache sizes",
                "Consider setting cache_size_mb to at least 256"
            ));
        }
    }
    
    return issues;
}

std::vector<ConfigIssue> ConfigSafety::validatePathConfig(const StorageConfig& config) {
    std::vector<ConfigIssue> issues;
    
    if (config.data_dir.empty()) {
        issues.append(createIssue(
            ValidationResult::ERROR,
            "path",
            "data_dir",
            "Data directory is not specified",
            "Set data_dir to a valid directory path",
            true
        ));
        return issues;
    }
    
    // Check if directory exists or can be created
    std::error_code ec;
    if (!std::filesystem::exists(config.data_dir, ec)) {
        if (!std::filesystem::create_directories(config.data_dir, ec)) {
            issues.append(createIssue(
                ValidationResult::ERROR,
                "path",
                "data_dir",
                "Cannot create data directory: " + config.data_dir,
                "Ensure parent directory exists and has proper permissions",
                true
            ));
        }
    }
    
    // Check permissions
    if (!hasProperPermissions(config.data_dir)) {
        issues.append(createIssue(
            ValidationResult::WARNING,
            "path",
            "data_dir",
            "Data directory may have insecure permissions",
            "Set directory permissions to 750 (rwxr-x---)"
        ));
    }
    
    // Check disk space
    if (!hasMinimumDiskSpace(config.data_dir, 1024 * 1024 * 1024)) { // 1GB minimum
        issues.append(createIssue(
            ValidationResult::WARNING,
            "path",
            "data_dir",
            "Low disk space in data directory",
            "Ensure at least 10GB free space for blockchain data"
        ));
    }
    
    return issues;
}

void ConfigSafety::initializeDefaultFallbacks() {
    // Default fallback chains
    fallback_chains_["rocksdb"] = {"leveldb", "sqlite"};
    fallback_chains_["leveldb"] = {"sqlite"};
    fallback_chains_["sqlite"] = {"memory"};
    fallback_chains_["memory"] = {}; // No fallbacks for memory backend
}

ConfigIssue ConfigSafety::createIssue(ValidationResult severity, 
                                     const std::string& category,
                                     const std::string& key,
                                     const std::string& message,
                                     const std::string& suggestion,
                                     bool blocks_startup) {
    ConfigIssue issue;
    issue.severity = severity;
    issue.category = category;
    issue.key = key;
    issue.message = message;
    issue.suggestion = suggestion;
    issue.blocks_startup = blocks_startup;
    return issue;
}

bool ConfigSafety::isSecureDataDirectory(const std::string& path) {
    return hasProperPermissions(path);
}

bool ConfigSafety::hasProperPermissions(const std::string& path) {
    std::error_code ec;
    auto perms = std::filesystem::status(path, ec).permissions();
    if (ec) return false;
    
    // Check that others don't have read/write/execute permissions
    return (perms & std::filesystem::perms::others_all) == std::filesystem::perms::none;
}

bool ConfigSafety::isEncryptionEnabled(const StorageConfig& config) {
    // Check environment variables for encryption settings
    return std::getenv("DINERO_ROCKSDB_ENCRYPTION") != nullptr ||
           std::getenv("DINERO_ENCRYPTION_KEY_FILE") != nullptr;
}

bool ConfigSafety::hasMinimumDiskSpace(const std::string& path, uint64_t required_bytes) {
    struct statvfs stat;
    if (statvfs(path.c_str(), &stat) != 0) {
        return false;
    }
    
    uint64_t available_bytes = stat.f_bavail * stat.f_frsize;
    return available_bytes >= required_bytes;
}

std::vector<ConfigIssue> ConfigSafety::checkSystemRequirements() {
    std::vector<ConfigIssue> issues;
    
    // Check minimum memory (2GB recommended)
    if (!hasMinimumMemory(2ULL * 1024 * 1024 * 1024)) {
        issues.append(createIssue(
            ValidationResult::WARNING,
            "system",
            "memory",
            "System has less than 2GB RAM",
            "Consider adding more memory for better performance"
        ));
    }
    
    return issues;
}

bool ConfigSafety::hasMinimumMemory(uint64_t required_bytes) {
    long pages = sysconf(_SC_PHYS_PAGES);
    long page_size = sysconf(_SC_PAGE_SIZE);
    if (pages < 0 || page_size < 0) return false;
    
    uint64_t total_memory = static_cast<uint64_t>(pages) * static_cast<uint64_t>(page_size);
    return total_memory >= required_bytes;
}

// Global functions

void InitializeConfigSafety(SafetyLevel level) {
    g_config_safety = std::make_unique<ConfigSafety>();
    g_config_safety->setSafetyLevel(level);
}

void ShutdownConfigSafety() {
    g_config_safety.reset();
}

std::vector<ConfigIssue> ValidateStorageConfig(const StorageConfig& config) {
    if (!g_config_safety) {
        return {};
    }
    return g_config_safety->validateConfig(config);
}

bool ShouldProceedWithStartup(const StorageConfig& config) {
    if (!g_config_safety) {
        return true; // No safety checks if not initialized
    }
    
    auto issues = g_config_safety->validateConfig(config);
    return !g_config_safety->doesConfigBlockStartup(issues);
}

void PrintSafetyReport(const std::vector<ConfigIssue>& issues) {
    if (!g_config_safety) {
        std::cout << "Configuration safety not initialized\n";
        return;
    }
    
    std::string report = g_config_safety->generateSafetyReport(issues);
    std::cout << report << std::endl;
}

} // namespace storage
} // namespace dinero
