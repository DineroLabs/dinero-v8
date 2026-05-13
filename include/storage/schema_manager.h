#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <functional>
#include <chrono>
#include "storage_interface.h"

namespace dinero {
namespace storage {

/**
 * Schema version information
 */
struct SchemaVersion {
    uint32_t major = 1;
    uint32_t minor = 0;
    uint32_t patch = 0;
    std::string suffix;  // e.g., "beta", "rc1"
    
    std::string toString() const {
        std::string version = std::to_string(major) + "." + 
                             std::to_string(minor) + "." + 
                             std::to_string(patch);
        if (!suffix.empty()) {
            version += "-" + suffix;
        }
        return version;
    }
    
    bool operator==(const SchemaVersion& other) const {
        return major == other.major && minor == other.minor && 
               patch == other.patch && suffix == other.suffix;
    }
    
    bool operator<(const SchemaVersion& other) const {
        if (major != other.major) return major < other.major;
        if (minor != other.minor) return minor < other.minor;
        if (patch != other.patch) return patch < other.patch;
        return suffix < other.suffix;
    }
    
    bool operator>(const SchemaVersion& other) const {
        return other < *this;
    }
    
    bool isCompatible(const SchemaVersion& other) const {
        // Same major version is compatible
        return major == other.major;
    }
};

/**
 * Migration operation result
 */
enum class MigrationResult {
    SUCCESS,
    FAILED,
    INCOMPATIBLE_VERSION,
    MIGRATION_NOT_FOUND,
    BACKUP_FAILED,
    VALIDATION_FAILED,
    ROLLBACK_FAILED,
    ALREADY_MIGRATED,
    DOWNGRADE_NOT_SUPPORTED
};

/**
 * Migration operation interface
 */
class MigrationOperation {
public:
    virtual ~MigrationOperation() = default;
    
    /**
     * Get source version this migration applies from
     */
    virtual SchemaVersion getSourceVersion() const = 0;
    
    /**
     * Get target version this migration applies to
     */
    virtual SchemaVersion getTargetVersion() const = 0;
    
    /**
     * Get migration description
     */
    virtual std::string getDescription() const = 0;
    
    /**
     * Check if migration can be applied
     */
    virtual bool canApply(StorageInterface* storage) const = 0;
    
    /**
     * Apply the migration
     */
    virtual MigrationResult apply(StorageInterface* storage) = 0;
    
    /**
     * Rollback the migration (if supported)
     */
    virtual MigrationResult rollback(StorageInterface* storage) = 0;
    
    /**
     * Validate migration was applied correctly
     */
    virtual bool validate(StorageInterface* storage) const = 0;
    
    /**
     * Get estimated migration time
     */
    virtual std::chrono::seconds getEstimatedDuration() const = 0;
    
    /**
     * Check if migration supports rollback
     */
    virtual bool supportsRollback() const = 0;
};

/**
 * Schema metadata stored in database
 */
struct SchemaMetadata {
    SchemaVersion version;
    std::string architecture;        // x86_64, arm64, etc.
    std::string endianness;         // little, big
    std::string created_by;         // Application version that created schema
    std::chrono::system_clock::time_point created_at;
    std::chrono::system_clock::time_point last_migration;
    std::vector<std::string> applied_migrations;
    std::unordered_map<std::string, std::string> custom_metadata;
    bool is_locked = false;         // Prevent concurrent migrations
    std::string lock_owner;
};

/**
 * Migration progress callback
 */
using MigrationProgressCallback = std::function<void(const std::string& operation, 
                                                   int progress_percent, 
                                                   const std::string& details)>;

/**
 * Schema manager for handling database schema versioning and migrations
 */
class SchemaManager {
public:
    explicit SchemaManager(StorageInterface* storage);
    ~SchemaManager();
    
    // === Schema Version Management ===
    
    /**
     * Initialize schema for new database
     */
    MigrationResult initializeSchema(const SchemaVersion& version);
    
    /**
     * Get current schema version
     */
    SchemaVersion getCurrentVersion() const;
    
    /**
     * Get target schema version (what we want to migrate to)
     */
    SchemaVersion getTargetVersion() const;
    
    /**
     * Set target schema version
     */
    void setTargetVersion(const SchemaVersion& version);
    
    /**
     * Check if schema exists and is valid
     */
    bool isSchemaValid() const;
    
    /**
     * Check if migration is needed
     */
    bool isMigrationNeeded() const;
    
    // === Migration Management ===
    
    /**
     * Register migration operation
     */
    void registerMigration(std::unique_ptr<MigrationOperation> migration);
    
    /**
     * Get available migrations
     */
    std::vector<MigrationOperation*> getAvailableMigrations() const;
    
    /**
     * Find migration path from current to target version
     */
    std::vector<MigrationOperation*> findMigrationPath(const SchemaVersion& from, 
                                                      const SchemaVersion& to) const;
    
    /**
     * Apply all necessary migrations to reach target version
     */
    MigrationResult migrateToTarget();
    
    /**
     * Apply specific migration
     */
    MigrationResult applyMigration(MigrationOperation* migration);
    
    /**
     * Rollback to previous version
     */
    MigrationResult rollbackToPrevious();
    
    /**
     * Rollback to specific version
     */
    MigrationResult rollbackToVersion(const SchemaVersion& version);
    
    // === Cross-Architecture Support ===
    
    /**
     * Check architecture compatibility
     */
    bool isArchitectureCompatible() const;
    
    /**
     * Get current system architecture
     */
    std::string getCurrentArchitecture() const;
    
    /**
     * Get current system endianness
     */
    std::string getCurrentEndianness() const;
    
    /**
     * Convert data for cross-architecture compatibility
     */
    std::vector<uint8_t> convertForArchitecture(const std::vector<uint8_t>& data,
                                               const std::string& source_arch,
                                               const std::string& target_arch) const;
    
    // === Backup and Recovery ===
    
    /**
     * Create backup before migration
     */
    MigrationResult createMigrationBackup(const std::string& backup_path);
    
    /**
     * Restore from migration backup
     */
    MigrationResult restoreFromBackup(const std::string& backup_path);
    
    /**
     * Enable/disable automatic backup before migrations
     */
    void setAutomaticBackup(bool enabled);
    
    /**
     * Set backup directory for automatic backups
     */
    void setBackupDirectory(const std::string& directory);
    
    // === Validation and Integrity ===
    
    /**
     * Validate schema integrity
     */
    bool validateSchemaIntegrity() const;
    
    /**
     * Validate data format consistency
     */
    bool validateDataFormat() const;
    
    /**
     * Check for schema corruption
     */
    std::vector<std::string> checkSchemaCorruption() const;
    
    /**
     * Repair schema corruption (if possible)
     */
    MigrationResult repairSchemaCorruption();
    
    // === Metadata Management ===
    
    /**
     * Get schema metadata
     */
    SchemaMetadata getMetadata() const;
    
    /**
     * Update schema metadata
     */
    MigrationResult updateMetadata(const SchemaMetadata& metadata);
    
    /**
     * Set custom metadata field
     */
    void setCustomMetadata(const std::string& key, const std::string& value);
    
    /**
     * Get custom metadata field
     */
    std::string getCustomMetadata(const std::string& key) const;
    
    // === Locking and Concurrency ===
    
    /**
     * Acquire migration lock
     */
    bool acquireMigrationLock(const std::string& owner);
    
    /**
     * Release migration lock
     */
    void releaseMigrationLock();
    
    /**
     * Check if migration is locked
     */
    bool isMigrationLocked() const;
    
    /**
     * Get migration lock owner
     */
    std::string getMigrationLockOwner() const;
    
    // === Progress and Callbacks ===
    
    /**
     * Set migration progress callback
     */
    void setProgressCallback(MigrationProgressCallback callback);
    
    /**
     * Set migration timeout
     */
    void setMigrationTimeout(std::chrono::seconds timeout);
    
    /**
     * Enable/disable dry run mode (validate without applying)
     */
    void setDryRunMode(bool enabled);
    
    // === Statistics and History ===
    
    /**
     * Get migration history
     */
    std::vector<std::string> getMigrationHistory() const;
    
    /**
     * Get migration statistics
     */
    struct MigrationStats {
        uint32_t total_migrations = 0;
        uint32_t successful_migrations = 0;
        uint32_t failed_migrations = 0;
        uint32_t rollbacks = 0;
        std::chrono::seconds total_migration_time{0};
        std::chrono::system_clock::time_point last_migration_time;
    };
    
    MigrationStats getMigrationStats() const;
    
private:
    StorageInterface* storage_;
    SchemaVersion target_version_;
    std::vector<std::unique_ptr<MigrationOperation>> migrations_;
    MigrationProgressCallback progress_callback_;
    std::chrono::seconds migration_timeout_{3600}; // 1 hour default
    bool automatic_backup_ = true;
    bool dry_run_mode_ = false;
    std::string backup_directory_;
    
    // Internal methods
    bool loadSchemaMetadata();
    bool saveSchemaMetadata(const SchemaMetadata& metadata);
    std::string generateBackupPath() const;
    void reportProgress(const std::string& operation, int percent, const std::string& details);
    bool isValidMigrationPath(const std::vector<MigrationOperation*>& path) const;
    MigrationResult executeMigrationPlan(const std::vector<MigrationOperation*>& plan);
    std::string serializeMetadata(const SchemaMetadata& metadata) const;
    SchemaMetadata deserializeMetadata(const std::string& data) const;
    
    // Architecture detection
    std::string detectArchitecture() const;
    std::string detectEndianness() const;
    
    // Data conversion utilities
    void swapEndianness(std::vector<uint8_t>& data) const;
    bool needsEndianConversion(const std::string& source_endian, const std::string& target_endian) const;
};

/**
 * Built-in migration operations
 */

/**
 * Schema initialization migration (creates initial schema)
 */
class InitialSchemaMigration : public MigrationOperation {
public:
    InitialSchemaMigration();
    
    SchemaVersion getSourceVersion() const override;
    SchemaVersion getTargetVersion() const override;
    std::string getDescription() const override;
    bool canApply(StorageInterface* storage) const override;
    MigrationResult apply(StorageInterface* storage) override;
    MigrationResult rollback(StorageInterface* storage) override;
    bool validate(StorageInterface* storage) const override;
    std::chrono::seconds getEstimatedDuration() const override;
    bool supportsRollback() const override;
};

/**
 * Index optimization migration
 */
class IndexOptimizationMigration : public MigrationOperation {
public:
    IndexOptimizationMigration(const SchemaVersion& from, const SchemaVersion& to);
    
    SchemaVersion getSourceVersion() const override;
    SchemaVersion getTargetVersion() const override;
    std::string getDescription() const override;
    bool canApply(StorageInterface* storage) const override;
    MigrationResult apply(StorageInterface* storage) override;
    MigrationResult rollback(StorageInterface* storage) override;
    bool validate(StorageInterface* storage) const override;
    std::chrono::seconds getEstimatedDuration() const override;
    bool supportsRollback() const override;
    
private:
    SchemaVersion source_version_;
    SchemaVersion target_version_;
};

/**
 * Data format migration for cross-architecture compatibility
 */
class ArchitectureMigration : public MigrationOperation {
public:
    ArchitectureMigration(const std::string& source_arch, const std::string& target_arch);
    
    SchemaVersion getSourceVersion() const override;
    SchemaVersion getTargetVersion() const override;
    std::string getDescription() const override;
    bool canApply(StorageInterface* storage) const override;
    MigrationResult apply(StorageInterface* storage) override;
    MigrationResult rollback(StorageInterface* storage) override;
    bool validate(StorageInterface* storage) const override;
    std::chrono::seconds getEstimatedDuration() const override;
    bool supportsRollback() const override;
    
private:
    std::string source_architecture_;
    std::string target_architecture_;
};

} // namespace storage
} // namespace dinero
