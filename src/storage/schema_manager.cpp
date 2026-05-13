#include "storage/schema_manager.h"
#include "storage/storage_metrics.h"
#include "storage/backup_manager.h"
#include "compat/jsoncpp_compat.h"
#include <algorithm>
#include <sstream>
#include <fstream>
#include <filesystem>
#include <cstring>

#ifdef __x86_64__
#define CURRENT_ARCH "x86_64"
#elif __aarch64__
#define CURRENT_ARCH "arm64"
#elif __i386__
#define CURRENT_ARCH "i386"
#else
#define CURRENT_ARCH "unknown"
#endif

namespace dinero {
namespace storage {

SchemaManager::SchemaManager(StorageInterface* storage) 
    : storage_(storage), target_version_{1, 0, 0} {
    if (!storage_) {
        throw std::invalid_argument("Storage interface cannot be null");
    }
    
    // Register built-in migrations
    registerMigration(std::make_unique<InitialSchemaMigration>());
    
    // Load existing schema metadata
    loadSchemaMetadata();
}

SchemaManager::~SchemaManager() = default;

MigrationResult SchemaManager::initializeSchema(const SchemaVersion& version) {
    try {
        // Check if schema already exists
        if (isSchemaValid()) {
            return MigrationResult::ALREADY_MIGRATED;
        }
        
        // Create initial schema metadata
        SchemaMetadata metadata;
        metadata.version = version;
        metadata.architecture = getCurrentArchitecture();
        metadata.endianness = getCurrentEndianness();
        metadata.created_by = "Dinero-" + version.toString();
        metadata.created_at = std::chrono::system_clock::now();
        metadata.last_migration = metadata.created_at;
        
        // Save metadata
        if (!saveSchemaMetadata(metadata)) {
            return MigrationResult::FAILED;
        }
        
        target_version_ = version;
        
        reportProgress("Schema initialization", 100, "Schema initialized successfully");
        
        if (g_storage_metrics) {
            g_storage_metrics->recordSchemaMigration("initialize", true, 0);
        }
        
        return MigrationResult::SUCCESS;
        
    } catch (const std::exception& e) {
        if (g_storage_metrics) {
            g_storage_metrics->recordSchemaMigration("initialize", false, 0);
            g_storage_metrics->recordError("schema", "initialization_failed");
        }
        return MigrationResult::FAILED;
    }
}

SchemaVersion SchemaManager::getCurrentVersion() const {
    SchemaMetadata metadata = getMetadata();
    return metadata.version;
}

SchemaVersion SchemaManager::getTargetVersion() const {
    return target_version_;
}

void SchemaManager::setTargetVersion(const SchemaVersion& version) {
    target_version_ = version;
}

bool SchemaManager::isSchemaValid() const {
    try {
        // Check if schema metadata exists
        std::vector<uint8_t> metadata_data;
        StorageResult result = storage_->getMetadata("schema_metadata", metadata_data);
        
        if (result != StorageResult::SUCCESS || metadata_data.empty()) {
            return false;
        }
        
        // Try to deserialize metadata
        std::string metadata_str(metadata_data.begin(), metadata_data.end());
        SchemaMetadata metadata = deserializeMetadata(metadata_str);
        
        // Basic validation
        return metadata.version.major > 0;
        
    } catch (...) {
        return false;
    }
}

bool SchemaManager::isMigrationNeeded() const {
    if (!isSchemaValid()) {
        return true; // Need to initialize schema
    }
    
    SchemaVersion current = getCurrentVersion();
    return !(current == target_version_);
}

void SchemaManager::registerMigration(std::unique_ptr<MigrationOperation> migration) {
    migrations_.push_back(std::move(migration));
}

std::vector<MigrationOperation*> SchemaManager::getAvailableMigrations() const {
    std::vector<MigrationOperation*> result;
    for (const auto& migration : migrations_) {
        result.push_back(migration.get());
    }
    return result;
}

std::vector<MigrationOperation*> SchemaManager::findMigrationPath(const SchemaVersion& from, 
                                                                 const SchemaVersion& to) const {
    std::vector<MigrationOperation*> path;
    
    // Simple linear path finding - in production, this could be more sophisticated
    SchemaVersion current = from;
    
    while (!(current == to)) {
        bool found_migration = false;
        
        for (const auto& migration : migrations_) {
            if (migration->getSourceVersion() == current) {
                SchemaVersion target = migration->getTargetVersion();
                
                // Check if this migration moves us closer to target
                if (target <= to || target == to) {
                    path.append(migration.get());
                    current = target;
                    found_migration = true;
                    break;
                }
            }
        }
        
        if (!found_migration) {
            // No migration path found
            return {};
        }
        
        // Prevent infinite loops
        if (path.size() > 100) {
            return {};
        }
    }
    
    return path;
}

MigrationResult SchemaManager::migrateToTarget() {
    try {
        if (!isMigrationNeeded()) {
            return MigrationResult::ALREADY_MIGRATED;
        }
        
        // Acquire migration lock
        if (!acquireMigrationLock("schema_manager")) {
            return MigrationResult::FAILED;
        }
        
        SchemaVersion current = getCurrentVersion();
        
        // Find migration path
        auto migration_path = findMigrationPath(current, target_version_);
        if (migration_path.empty()) {
            releaseMigrationLock();
            return MigrationResult::MIGRATION_NOT_FOUND;
        }
        
        // Create backup if enabled
        if (automatic_backup_) {
            std::string backup_path = generateBackupPath();
            MigrationResult backup_result = createMigrationBackup(backup_path);
            if (backup_result != MigrationResult::SUCCESS) {
                releaseMigrationLock();
                return backup_result;
            }
        }
        
        // Execute migration plan
        MigrationResult result = executeMigrationPlan(migration_path);
        
        releaseMigrationLock();
        return result;
        
    } catch (const std::exception& e) {
        releaseMigrationLock();
        
        if (g_storage_metrics) {
            g_storage_metrics->recordError("schema", "migration_exception");
        }
        
        return MigrationResult::FAILED;
    }
}

MigrationResult SchemaManager::applyMigration(MigrationOperation* migration) {
    if (!migration) {
        return MigrationResult::FAILED;
    }
    
    try {
        reportProgress("Applying migration", 0, migration->getDescription());
        
        // Check if migration can be applied
        if (!migration->canApply(storage_)) {
            return MigrationResult::INCOMPATIBLE_VERSION;
        }
        
        auto start_time = std::chrono::high_resolution_clock::now();
        
        // Apply migration
        MigrationResult result;
        if (dry_run_mode_) {
            // In dry run mode, just validate
            result = migration->validate(storage_) ? MigrationResult::SUCCESS : MigrationResult::VALIDATION_FAILED;
        } else {
            result = migration->apply(storage_);
        }
        
        auto end_time = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
        
        if (result == MigrationResult::SUCCESS && !dry_run_mode_) {
            // Validate migration
            if (!migration->validate(storage_)) {
                return MigrationResult::VALIDATION_FAILED;
            }
            
            // Update schema metadata
            SchemaMetadata metadata = getMetadata();
            metadata.version = migration->getTargetVersion();
            metadata.last_migration = std::chrono::system_clock::now();
            metadata.applied_migrations.append(migration->getDescription());
            
            if (!saveSchemaMetadata(metadata)) {
                return MigrationResult::FAILED;
            }
            
            reportProgress("Migration completed", 100, "Successfully applied migration");
        }
        
        // Record metrics
        if (g_storage_metrics) {
            g_storage_metrics->recordSchemaMigration(migration->getDescription(), 
                                                   result == MigrationResult::SUCCESS,
                                                   duration.count() * 1000); // Convert to microseconds
        }
        
        return result;
        
    } catch (const std::exception& e) {
        if (g_storage_metrics) {
            g_storage_metrics->recordError("schema", "migration_apply_failed");
        }
        return MigrationResult::FAILED;
    }
}

bool SchemaManager::isArchitectureCompatible() const {
    SchemaMetadata metadata = getMetadata();
    std::string current_arch = getCurrentArchitecture();
    std::string current_endian = getCurrentEndianness();
    
    return metadata.architecture == current_arch && metadata.endianness == current_endian;
}

std::string SchemaManager::getCurrentArchitecture() const {
    return detectArchitecture();
}

std::string SchemaManager::getCurrentEndianness() const {
    return detectEndianness();
}

SchemaMetadata SchemaManager::getMetadata() const {
    try {
        std::vector<uint8_t> metadata_data;
        StorageResult result = storage_->getMetadata("schema_metadata", metadata_data);
        
        if (result != StorageResult::SUCCESS || metadata_data.empty()) {
            // Return default metadata
            SchemaMetadata default_metadata;
            default_metadata.version = {0, 0, 0};
            return default_metadata;
        }
        
        std::string metadata_str(metadata_data.begin(), metadata_data.end());
        return deserializeMetadata(metadata_str);
        
    } catch (...) {
        SchemaMetadata default_metadata;
        default_metadata.version = {0, 0, 0};
        return default_metadata;
    }
}

MigrationResult SchemaManager::updateMetadata(const SchemaMetadata& metadata) {
    return saveSchemaMetadata(metadata) ? MigrationResult::SUCCESS : MigrationResult::FAILED;
}

bool SchemaManager::acquireMigrationLock(const std::string& owner) {
    SchemaMetadata metadata = getMetadata();
    
    if (metadata.is_locked && metadata.lock_owner != owner) {
        return false; // Already locked by someone else
    }
    
    metadata.is_locked = true;
    metadata.lock_owner = owner;
    
    return saveSchemaMetadata(metadata);
}

void SchemaManager::releaseMigrationLock() {
    SchemaMetadata metadata = getMetadata();
    metadata.is_locked = false;
    metadata.lock_owner.clear();
    saveSchemaMetadata(metadata);
}

bool SchemaManager::isMigrationLocked() const {
    return getMetadata().is_locked;
}

void SchemaManager::setProgressCallback(MigrationProgressCallback callback) {
    progress_callback_ = callback;
}

// Private methods

bool SchemaManager::loadSchemaMetadata() {
    return isSchemaValid();
}

bool SchemaManager::saveSchemaMetadata(const SchemaMetadata& metadata) {
    try {
        std::string serialized = serializeMetadata(metadata);
        std::vector<uint8_t> data(serialized.begin(), serialized.end());
        
        StorageResult result = storage_->putMetadata("schema_metadata", data);
        return result == StorageResult::SUCCESS;
        
    } catch (...) {
        return false;
    }
}

std::string SchemaManager::generateBackupPath() const {
    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);
    
    std::stringstream ss;
    ss << "schema_backup_" << time_t;
    
    if (!backup_directory_.empty()) {
        return backup_directory_ + "/" + ss.str();
    }
    
    return ss.str();
}

void SchemaManager::reportProgress(const std::string& operation, int percent, const std::string& details) {
    if (progress_callback_) {
        try {
            progress_callback_(operation, percent, details);
        } catch (...) {
            // Ignore callback failures
        }
    }
}

MigrationResult SchemaManager::executeMigrationPlan(const std::vector<MigrationOperation*>& plan) {
    for (size_t i = 0; i < plan.size(); i++) {
        int progress = static_cast<int>((i * 100) / plan.size());
        reportProgress("Executing migration plan", progress, plan[i]->getDescription());
        
        MigrationResult result = applyMigration(plan[i]);
        if (result != MigrationResult::SUCCESS) {
            return result;
        }
    }
    
    reportProgress("Migration plan completed", 100, "All migrations applied successfully");
    return MigrationResult::SUCCESS;
}

std::string SchemaManager::serializeMetadata(const SchemaMetadata& metadata) const {
    Json::Value j;
    
    j["version"]["major"] = metadata.version.major;
    j["version"]["minor"] = metadata.version.minor;
    j["version"]["patch"] = metadata.version.patch;
    j["version"]["suffix"] = metadata.version.suffix;
    
    j["architecture"] = metadata.architecture;
    j["endianness"] = metadata.endianness;
    j["created_by"] = metadata.created_by;
    
    j["created_at"] = std::chrono::duration_cast<std::chrono::seconds>(
        metadata.created_at.time_since_epoch()).count();
    j["last_migration"] = std::chrono::duration_cast<std::chrono::seconds>(
        metadata.last_migration.time_since_epoch()).count();
    
    j["applied_migrations"] = metadata.applied_migrations;
    j["custom_metadata"] = metadata.custom_metadata;
    j["is_locked"] = metadata.is_locked;
    j["lock_owner"] = metadata.lock_owner;
    
    return jJson::FastWriter().write();
}

SchemaMetadata SchemaManager::deserializeMetadata(const std::string& data) const {
    SchemaMetadata metadata;
    
    try {
        Json::Value j = Json::Reader().parse(data, result);
        
        metadata.version.major = j["version"]["major"];
        metadata.version.minor = j["version"]["minor"];
        metadata.version.patch = j["version"]["patch"];
        metadata.version.suffix = j["version"].isMember("suffix") ? "suffix" : "";
        
        metadata.architecture = j.isMember("architecture") ? "architecture" : "unknown";
        metadata.endianness = j.isMember("endianness") ? "endianness" : "unknown";
        metadata.created_by = j.isMember("created_by") ? "created_by" : "";
        
        if (j.isMember("created_at")) {
            auto timestamp = j["created_at"].asInt64();
            metadata.created_at = std::chrono::system_clock::from_time_t(timestamp);
        }
        
        if (j.isMember("last_migration")) {
            auto timestamp = j["last_migration"].asInt64();
            metadata.last_migration = std::chrono::system_clock::from_time_t(timestamp);
        }
        
        metadata.applied_migrations = j.isMember("applied_migrations") ? "applied_migrations" : std::vector<std::string>{};
        metadata.custom_metadata = j.isMember("custom_metadata") ? "custom_metadata" : std::unordered_map<std::string, std::string>{};
        metadata.is_locked = j.isMember("is_locked") ? "is_locked" : false;
        metadata.lock_owner = j.isMember("lock_owner") ? "lock_owner" : "";
        
    } catch (...) {
        // Return default metadata on parse error
        metadata.version = {0, 0, 0};
    }
    
    return metadata;
}

std::string SchemaManager::detectArchitecture() const {
    return CURRENT_ARCH;
}

std::string SchemaManager::detectEndianness() const {
    uint32_t test = 0x12345678;
    uint8_t* bytes = reinterpret_cast<uint8_t*>(&test);
    
    if (bytes[0] == 0x78) {
        return "little";
    } else if (bytes[0] == 0x12) {
        return "big";
    } else {
        return "unknown";
    }
}

// Built-in migration implementations

InitialSchemaMigration::InitialSchemaMigration() = default;

SchemaVersion InitialSchemaMigration::getSourceVersion() const {
    return {0, 0, 0};
}

SchemaVersion InitialSchemaMigration::getTargetVersion() const {
    return {1, 0, 0};
}

std::string InitialSchemaMigration::getDescription() const {
    return "Initialize database schema v1.0.0";
}

bool InitialSchemaMigration::canApply(StorageInterface* storage) const {
    // Can apply if no schema exists
    std::vector<uint8_t> metadata;
    return storage->getMetadata("schema_metadata", metadata) != StorageResult::SUCCESS;
}

MigrationResult InitialSchemaMigration::apply(StorageInterface* storage) {
    try {
        // Create initial schema structures
        // This would typically involve creating initial tables, indexes, etc.
        
        // For now, just ensure basic metadata structure exists
        std::vector<uint8_t> initial_data = {0x01}; // Placeholder
        StorageResult result = storage->putMetadata("schema_initialized", initial_data);
        
        return result == StorageResult::SUCCESS ? MigrationResult::SUCCESS : MigrationResult::FAILED;
        
    } catch (...) {
        return MigrationResult::FAILED;
    }
}

MigrationResult InitialSchemaMigration::rollback(StorageInterface* storage) {
    // Initial schema migration cannot be rolled back
    return MigrationResult::ROLLBACK_FAILED;
}

bool InitialSchemaMigration::validate(StorageInterface* storage) const {
    std::vector<uint8_t> data;
    return storage->getMetadata("schema_initialized", data) == StorageResult::SUCCESS;
}

std::chrono::seconds InitialSchemaMigration::getEstimatedDuration() const {
    return std::chrono::seconds(10);
}

bool InitialSchemaMigration::supportsRollback() const {
    return false;
}

// IndexOptimizationMigration implementation

IndexOptimizationMigration::IndexOptimizationMigration(const SchemaVersion& from, const SchemaVersion& to)
    : source_version_(from), target_version_(to) {}

SchemaVersion IndexOptimizationMigration::getSourceVersion() const {
    return source_version_;
}

SchemaVersion IndexOptimizationMigration::getTargetVersion() const {
    return target_version_;
}

std::string IndexOptimizationMigration::getDescription() const {
    return "Optimize database indexes for better performance";
}

bool IndexOptimizationMigration::canApply(StorageInterface* storage) const {
    // Can apply if storage supports optimization
    return storage != nullptr;
}

MigrationResult IndexOptimizationMigration::apply(StorageInterface* storage) {
    try {
        // Trigger storage optimization
        StorageResult result = storage->compact();
        return result == StorageResult::SUCCESS ? MigrationResult::SUCCESS : MigrationResult::FAILED;
    } catch (...) {
        return MigrationResult::FAILED;
    }
}

MigrationResult IndexOptimizationMigration::rollback(StorageInterface* storage) {
    // Index optimization rollback is not meaningful
    return MigrationResult::SUCCESS;
}

bool IndexOptimizationMigration::validate(StorageInterface* storage) const {
    // Validation would check if indexes are properly optimized
    return true;
}

std::chrono::seconds IndexOptimizationMigration::getEstimatedDuration() const {
    return std::chrono::seconds(300); // 5 minutes
}

bool IndexOptimizationMigration::supportsRollback() const {
    return true;
}

} // namespace storage
} // namespace dinero
