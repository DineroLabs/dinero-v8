#pragma once

#include <string>
#include <vector>
#include <memory>
#include <chrono>
#include <functional>
#include "storage_interface.h"

namespace dinero {
namespace storage {

/**
 * Backup metadata information
 */
struct BackupInfo {
    std::string backup_id;
    std::string backup_path;
    std::string backend_type;
    std::string source_directory;
    std::chrono::system_clock::time_point created_at;
    uint64_t backup_size_bytes = 0;
    uint64_t block_height = 0;
    std::string chain_tip_hash;
    bool is_complete = false;
    bool is_verified = false;
    std::string checksum;
    std::vector<std::string> file_list;
    std::string metadata_json;
};

/**
 * Backup operation result
 */
enum class BackupResult {
    SUCCESS,
    FAILED,
    INVALID_PATH,
    INSUFFICIENT_SPACE,
    PERMISSION_DENIED,
    CORRUPTION_DETECTED,
    VERIFICATION_FAILED,
    ALREADY_EXISTS,
    NOT_FOUND,
    INCOMPLETE
};

/**
 * Restore operation options
 */
struct RestoreOptions {
    bool verify_before_restore = true;
    bool backup_existing = true;
    bool force_overwrite = false;
    bool validate_chain_consistency = true;
    std::string backup_existing_suffix = ".pre-restore";
    std::function<void(const std::string&)> progress_callback;
};

/**
 * Backup operation options
 */
struct BackupOptions {
    bool create_checkpoint = true;
    bool verify_backup = true;
    bool compress_backup = false;
    bool include_logs = false;
    std::string backup_id;
    std::function<void(const std::string&)> progress_callback;
};

/**
 * Storage backup and restore manager
 * 
 * Provides consistent backup and restore operations for different storage backends:
 * - RocksDB: Uses checkpoint API for consistent snapshots
 * - LevelDB: Uses file-level copying with proper locking
 * - SQLite: Uses backup API for transactional consistency
 */
class BackupManager {
public:
    explicit BackupManager(StorageInterface* storage);
    ~BackupManager();
    
    // === Backup Operations ===
    
    /**
     * Create a backup of the storage
     * 
     * @param backup_path Directory to store the backup
     * @param options Backup configuration options
     * @return BackupResult indicating success or failure
     */
    BackupResult createBackup(const std::string& backup_path, const BackupOptions& options = {});
    
    /**
     * Create incremental backup (if supported by backend)
     * 
     * @param backup_path Directory to store the backup
     * @param base_backup_path Previous backup to use as base
     * @param options Backup configuration options
     * @return BackupResult indicating success or failure
     */
    BackupResult createIncrementalBackup(const std::string& backup_path, 
                                       const std::string& base_backup_path,
                                       const BackupOptions& options = {});
    
    /**
     * Verify backup integrity
     * 
     * @param backup_path Path to backup directory
     * @return BackupResult indicating verification result
     */
    BackupResult verifyBackup(const std::string& backup_path);
    
    // === Restore Operations ===
    
    /**
     * Restore storage from backup
     * 
     * @param backup_path Path to backup directory
     * @param target_path Target directory for restored data
     * @param options Restore configuration options
     * @return BackupResult indicating success or failure
     */
    BackupResult restoreFromBackup(const std::string& backup_path,
                                 const std::string& target_path,
                                 const RestoreOptions& options = {});
    
    /**
     * List available backups in directory
     * 
     * @param backup_root_path Root directory containing backups
     * @return Vector of BackupInfo for discovered backups
     */
    std::vector<BackupInfo> listBackups(const std::string& backup_root_path);
    
    /**
     * Get backup information
     * 
     * @param backup_path Path to specific backup
     * @return BackupInfo with metadata, or empty if not found
     */
    BackupInfo getBackupInfo(const std::string& backup_path);
    
    // === Maintenance Operations ===
    
    /**
     * Clean up old backups based on retention policy
     * 
     * @param backup_root_path Root directory containing backups
     * @param max_backups Maximum number of backups to keep
     * @param max_age_days Maximum age in days for backups
     * @return Number of backups cleaned up
     */
    int cleanupOldBackups(const std::string& backup_root_path, 
                         int max_backups = 10, 
                         int max_age_days = 30);
    
    /**
     * Estimate backup size before creating
     * 
     * @return Estimated backup size in bytes
     */
    uint64_t estimateBackupSize();
    
    /**
     * Check available disk space for backup
     * 
     * @param backup_path Target backup directory
     * @return Available space in bytes
     */
    uint64_t getAvailableSpace(const std::string& backup_path);
    
    // === Configuration ===
    
    /**
     * Set backup compression level (0-9, 0=none, 9=max)
     */
    void setCompressionLevel(int level);
    
    /**
     * Set verification level for backups
     */
    void setVerificationLevel(int level);
    
    /**
     * Enable/disable progress callbacks
     */
    void setProgressReporting(bool enabled);
    
private:
    StorageInterface* storage_;
    int compression_level_ = 0;
    int verification_level_ = 1;
    bool progress_reporting_ = false;
    
    // Backend-specific backup implementations
    BackupResult createRocksDBBackup(const std::string& backup_path, const BackupOptions& options);
    BackupResult createLevelDBBackup(const std::string& backup_path, const BackupOptions& options);
    BackupResult createSQLiteBackup(const std::string& backup_path, const BackupOptions& options);
    
    // Backend-specific restore implementations
    BackupResult restoreRocksDBBackup(const std::string& backup_path, const std::string& target_path, const RestoreOptions& options);
    BackupResult restoreLevelDBBackup(const std::string& backup_path, const std::string& target_path, const RestoreOptions& options);
    BackupResult restoreSQLiteBackup(const std::string& backup_path, const std::string& target_path, const RestoreOptions& options);
    
    // Utility methods
    BackupInfo createBackupMetadata(const std::string& backup_path, const BackupOptions& options);
    bool saveBackupMetadata(const BackupInfo& info);
    BackupInfo loadBackupMetadata(const std::string& backup_path);
    std::string calculateChecksum(const std::string& directory);
    bool verifyChecksum(const std::string& directory, const std::string& expected_checksum);
    uint64_t calculateDirectorySize(const std::string& directory);
    std::vector<std::string> getFileList(const std::string& directory);
    bool copyDirectory(const std::string& source, const std::string& destination);
    bool createDirectoryStructure(const std::string& path);
    void reportProgress(const std::string& message);
    
    // Chain state validation
    bool validateChainConsistency(const std::string& data_path);
    uint64_t getCurrentBlockHeight();
    std::string getCurrentChainTip();
};

/**
 * Backup manager factory
 */
class BackupManagerFactory {
public:
    /**
     * Create backup manager for storage instance
     */
    static std::unique_ptr<BackupManager> create(StorageInterface* storage);
    
    /**
     * Create backup manager with custom configuration
     */
    static std::unique_ptr<BackupManager> create(StorageInterface* storage, 
                                               int compression_level,
                                               int verification_level);
};

} // namespace storage
} // namespace dinero
