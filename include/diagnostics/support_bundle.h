#pragma once
#include <string>
#include <vector>
#include <map>
#include <cstdint>

namespace dinero {
namespace diagnostics {

/**
 * Support Bundle Generator
 * 
 * Creates comprehensive diagnostic packages for troubleshooting
 * Dinero node issues. Includes logs, configuration, system info,
 * and network diagnostics while protecting sensitive data.
 */

struct SystemInfo {
    std::string os_name;
    std::string os_version;
    std::string architecture;
    std::string cpu_model;
    uint64_t total_memory_mb;
    uint64_t available_memory_mb;
    std::string dinero_version;
    std::string build_date;
    std::string qt_version;
};

struct NetworkDiagnostics {
    std::vector<std::string> reachable_seeds;
    std::vector<std::string> unreachable_seeds;
    int connected_peers;
    int attempted_connections;
    std::string public_ip;
    bool upnp_enabled;
    bool port_forwarded;
    std::vector<std::string> dns_results;
};

struct NodeStatus {
    bool daemon_running;
    bool rpc_responsive;
    bool websocket_connected;
    int block_height;
    std::string best_block_hash;
    double difficulty;
    bool mining_active;
    int mining_threads;
    std::string mining_address;
    double hashrate;
    std::string sync_status;
};

struct WalletInfo {
    bool wallet_loaded;
    bool wallet_encrypted;
    int address_count;
    std::string first_address_preview;  // First 10 chars only for privacy
    bool hd_enabled;
    std::string derivation_path;
    uint32_t coin_type;
};

struct LogSummary {
    std::vector<std::string> recent_errors;
    std::vector<std::string> recent_warnings;
    std::string log_file_path;
    uint64_t log_file_size_bytes;
    int error_count_24h;
    int warning_count_24h;
};

/**
 * Support Bundle Contents
 */
struct SupportBundle {
    std::string bundle_id;
    std::string creation_time;
    std::string user_description;
    
    SystemInfo system;
    NetworkDiagnostics network;
    NodeStatus node;
    WalletInfo wallet;
    LogSummary logs;
    
    std::map<std::string, std::string> config_files;  // Sanitized config
    std::vector<std::string> recent_log_entries;      // Last 1000 lines
    std::string bundle_file_path;
    
    // Privacy protection
    bool contains_sensitive_data = false;
    std::vector<std::string> sanitized_fields;
};

/**
 * Support Bundle Generator
 */
class SupportBundleGenerator {
public:
    /**
     * Generate a complete support bundle
     */
    static SupportBundle generateBundle(const std::string& datadir,
                                      const std::string& user_description = "",
                                      bool include_full_logs = false);
    
    /**
     * Save bundle to ZIP file
     */
    static std::string saveBundleToFile(const SupportBundle& bundle, 
                                      const std::string& output_dir = "");
    
    /**
     * Generate quick diagnostics (no file creation)
     */
    static SupportBundle quickDiagnostics(const std::string& datadir);
    
    /**
     * Validate bundle completeness
     */
    static bool validateBundle(const SupportBundle& bundle);
    
private:
    static SystemInfo collectSystemInfo();
    static NetworkDiagnostics collectNetworkDiagnostics();
    static NodeStatus collectNodeStatus(const std::string& datadir);
    static WalletInfo collectWalletInfo(const std::string& datadir);
    static LogSummary collectLogSummary(const std::string& datadir);
    static std::map<std::string, std::string> collectConfigFiles(const std::string& datadir);
    static std::vector<std::string> collectRecentLogs(const std::string& datadir, int max_lines = 1000);
    
    // Privacy protection
    static std::string sanitizeConfig(const std::string& config_content);
    static std::string sanitizeLogEntry(const std::string& log_entry);
    static std::string redactSensitiveData(const std::string& text);
};

/**
 * Node Reset and Recovery Tools
 */
class NodeRecovery {
public:
    enum class ResetLevel {
        SOFT_RESET,      // Restart daemon only
        CONFIG_RESET,    // Reset configuration to defaults
        BLOCKCHAIN_RESET, // Delete blockchain data (keep wallet)
        FULL_RESET       // Reset everything except wallet
    };
    
    struct ResetOptions {
        ResetLevel level = ResetLevel::SOFT_RESET;
        bool preserve_wallet = true;
        bool preserve_mining_config = true;
        bool preserve_network_config = false;
        bool create_backup = true;
        std::string backup_location;
    };
    
    struct ResetResult {
        bool success = false;
        std::string error_message;
        std::vector<std::string> files_removed;
        std::vector<std::string> files_backed_up;
        std::string backup_path;
        std::vector<std::string> recovery_steps;
    };
    
    /**
     * Perform node reset with specified options
     */
    static ResetResult performReset(const std::string& datadir, const ResetOptions& options);
    
    /**
     * Create backup before reset
     */
    static std::string createBackup(const std::string& datadir, const std::string& backup_dir = "");
    
    /**
     * Restore from backup
     */
    static bool restoreFromBackup(const std::string& backup_path, const std::string& datadir);
    
    /**
     * Check if recovery is needed
     */
    static bool needsRecovery(const std::string& datadir);
    
    /**
     * Get recovery recommendations
     */
    static std::vector<std::string> getRecoveryRecommendations(const std::string& datadir);
    
private:
    static bool isWalletCorrupted(const std::string& datadir);
    static bool isBlockchainCorrupted(const std::string& datadir);
    static bool isConfigCorrupted(const std::string& datadir);
};

/**
 * Crash Detection and Auto-Recovery
 */
class CrashRecovery {
public:
    struct CrashInfo {
        std::string crash_time;
        std::string crash_type;
        std::string error_message;
        std::string stack_trace;
        int restart_count;
        bool auto_recovery_attempted;
    };
    
    /**
     * Initialize crash detection
     */
    static void initializeCrashDetection(const std::string& datadir);
    
    /**
     * Record crash information
     */
    static void recordCrash(const std::string& datadir, const CrashInfo& crash);
    
    /**
     * Check for previous crashes
     */
    static std::vector<CrashInfo> getPreviousCrashes(const std::string& datadir);
    
    /**
     * Attempt automatic recovery
     */
    static bool attemptAutoRecovery(const std::string& datadir);
    
    /**
     * Check if too many crashes (disable auto-recovery)
     */
    static bool tooManyCrashes(const std::string& datadir, int time_window_hours = 24);
};

} // namespace diagnostics
} // namespace dinero

/**
 * USAGE EXAMPLES:
 * 
 * // Generate support bundle
 * auto bundle = SupportBundleGenerator::generateBundle(datadir, "Mining not working");
 * std::string bundle_file = SupportBundleGenerator::saveBundleToFile(bundle);
 * 
 * // Quick diagnostics
 * auto quick = SupportBundleGenerator::quickDiagnostics(datadir);
 * 
 * // Node reset
 * NodeRecovery::ResetOptions options;
 * options.level = NodeRecovery::ResetLevel::CONFIG_RESET;
 * options.preserve_wallet = true;
 * auto result = NodeRecovery::performReset(datadir, options);
 * 
 * // Crash recovery
 * if (CrashRecovery::needsRecovery(datadir)) {
 *     CrashRecovery::attemptAutoRecovery(datadir);
 * }
 */
