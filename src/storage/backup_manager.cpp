#include "storage/backup_manager.h"
#include "storage/storage_metrics.h"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <thread>
#include <chrono>
#include <openssl/sha.h>
#include "compat/jsoncpp_compat.h"

namespace dinero {
namespace storage {

BackupManager::BackupManager(StorageInterface* storage) 
    : storage_(storage) {
    if (!storage_) {
        throw std::invalid_argument("Storage interface cannot be null");
    }
}

BackupManager::~BackupManager() = default;

BackupResult BackupManager::createBackup(const std::string& backup_path, const BackupOptions& options) {
    if (!storage_) {
        return BackupResult::FAILED;
    }
    
    try {
        reportProgress("Starting backup creation to: " + backup_path);
        
        // Check available space
        uint64_t estimated_size = estimateBackupSize();
        uint64_t available_space = getAvailableSpace(backup_path);
        
        if (available_space < estimated_size * 1.2) { // 20% buffer
            reportProgress("Insufficient disk space for backup");
            return BackupResult::INSUFFICIENT_SPACE;
        }
        
        // Create backup directory structure
        if (!createDirectoryStructure(backup_path)) {
            return BackupResult::PERMISSION_DENIED;
        }
        
        // Check if backup already exists
        if (std::filesystem::exists(backup_path + "/backup_metadata.json") && !options.backup_id.empty()) {
            return BackupResult::ALREADY_EXISTS;
        }
        
        BackupResult result = BackupResult::FAILED;
        std::string backend_name = storage_->name();
        
        auto start_time = std::chrono::high_resolution_clock::now();
        
        // Dispatch to backend-specific implementation
        if (backend_name == "rocksdb") {
            result = createRocksDBBackup(backup_path, options);
        } else if (backend_name == "leveldb") {
            result = createLevelDBBackup(backup_path, options);
        } else if (backend_name == "sqlite") {
            result = createSQLiteBackup(backup_path, options);
        } else {
            reportProgress("Unsupported backend for backup: " + backend_name);
            return BackupResult::FAILED;
        }
        
        auto end_time = std::chrono::high_resolution_clock::now();
        auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count();
        
        if (result == BackupResult::SUCCESS) {
            // Create and save backup metadata
            BackupInfo info = createBackupMetadata(backup_path, options);
            info.backup_size_bytes = calculateDirectorySize(backup_path);
            
            if (options.verify_backup) {
                reportProgress("Verifying backup integrity");
                BackupResult verify_result = verifyBackup(backup_path);
                if (verify_result != BackupResult::SUCCESS) {
                    reportProgress("Backup verification failed");
                    return verify_result;
                }
                info.is_verified = true;
            }
            
            info.is_complete = true;
            if (!saveBackupMetadata(info)) {
                reportProgress("Failed to save backup metadata");
                return BackupResult::FAILED;
            }
            
            reportProgress("Backup completed successfully in " + std::to_string(duration_ms) + "ms");
            
            // Record metrics
            if (g_storage_metrics) {
                g_storage_metrics->recordBackup(duration_ms * 1000, true, info.backup_size_bytes);
            }
        } else {
            reportProgress("Backup failed");
            
            // Clean up partial backup
            try {
                std::filesystem::remove_all(backup_path);
            } catch (...) {
                // Ignore cleanup errors
            }
            
            if (g_storage_metrics) {
                g_storage_metrics->recordBackup(duration_ms * 1000, false, 0);
                g_storage_metrics->recordError("backup", "backup_failed");
            }
        }
        
        return result;
        
    } catch (const std::exception& e) {
        reportProgress("Backup failed with exception: " + std::string(e.what()));
        
        if (g_storage_metrics) {
            g_storage_metrics->recordError("backup", "exception");
        }
        
        return BackupResult::FAILED;
    }
}

BackupResult BackupManager::createIncrementalBackup(const std::string& backup_path, 
                                                   const std::string& base_backup_path,
                                                   const BackupOptions& options) {
    // Incremental backups are primarily supported by RocksDB
    std::string backend_name = storage_->name();
    
    if (backend_name != "rocksdb") {
        reportProgress("Incremental backups not supported for backend: " + backend_name);
        return BackupResult::FAILED;
    }
    
    // Verify base backup exists and is valid
    BackupResult verify_result = verifyBackup(base_backup_path);
    if (verify_result != BackupResult::SUCCESS) {
        reportProgress("Base backup verification failed");
        return verify_result;
    }
    
    reportProgress("Creating incremental backup based on: " + base_backup_path);
    
    // For now, fall back to full backup
    // TODO: Implement true incremental backup using SQLite backup engine
    return createBackup(backup_path, options);
}

BackupResult BackupManager::verifyBackup(const std::string& backup_path) {
    try {
        reportProgress("Verifying backup at: " + backup_path);
        
        // Check if backup directory exists
        if (!std::filesystem::exists(backup_path)) {
            return BackupResult::NOT_FOUND;
        }
        
        // Load backup metadata
        BackupInfo info = loadBackupMetadata(backup_path);
        if (info.backup_id.empty()) {
            reportProgress("Backup metadata not found or invalid");
            return BackupResult::INCOMPLETE;
        }
        
        // Verify file list
        std::vector<std::string> current_files = getFileList(backup_path);
        if (current_files.size() != info.file_list.size()) {
            reportProgress("File count mismatch in backup");
            return BackupResult::CORRUPTION_DETECTED;
        }
        
        // Verify checksum if available
        if (!info.checksum.empty()) {
            std::string current_checksum = calculateChecksum(backup_path);
            if (current_checksum != info.checksum) {
                reportProgress("Checksum verification failed");
                return BackupResult::VERIFICATION_FAILED;
            }
        }
        
        // Backend-specific verification
        std::string backend_name = info.backend_type;
        if (backend_name == "sqlite") {
            // Verify SQLite database integrity
            // TODO: Add SQLite-specific verification (PRAGMA integrity_check)
        } else if (backend_name == "rocksdb") {
            // Legacy RocksDB support
            // TODO: Add RocksDB-specific verification
        } else if (backend_name == "leveldb") {
            // Legacy LevelDB support
            // TODO: Add LevelDB-specific verification
        }
        
        reportProgress("Backup verification completed successfully");
        return BackupResult::SUCCESS;
        
    } catch (const std::exception& e) {
        reportProgress("Backup verification failed: " + std::string(e.what()));
        return BackupResult::FAILED;
    }
}

BackupResult BackupManager::restoreFromBackup(const std::string& backup_path,
                                             const std::string& target_path,
                                             const RestoreOptions& options) {
    try {
        reportProgress("Starting restore from: " + backup_path + " to: " + target_path);
        
        // Verify backup before restore
        if (options.verify_before_restore) {
            BackupResult verify_result = verifyBackup(backup_path);
            if (verify_result != BackupResult::SUCCESS) {
                reportProgress("Backup verification failed before restore");
                return verify_result;
            }
        }
        
        // Load backup metadata
        BackupInfo info = loadBackupMetadata(backup_path);
        if (info.backup_id.empty()) {
            return BackupResult::INCOMPLETE;
        }
        
        // Backup existing data if requested
        if (options.backup_existing && std::filesystem::exists(target_path)) {
            std::string backup_existing_path = target_path + options.backup_existing_suffix;
            reportProgress("Backing up existing data to: " + backup_existing_path);
            
            if (!copyDirectory(target_path, backup_existing_path)) {
                reportProgress("Failed to backup existing data");
                if (!options.force_overwrite) {
                    return BackupResult::FAILED;
                }
            }
        }
        
        // Create target directory
        if (!createDirectoryStructure(target_path)) {
            return BackupResult::PERMISSION_DENIED;
        }
        
        BackupResult result = BackupResult::FAILED;
        auto start_time = std::chrono::high_resolution_clock::now();
        
        // Dispatch to backend-specific restore
        if (info.backend_type == "rocksdb") {
            result = restoreRocksDBBackup(backup_path, target_path, options);
        } else if (info.backend_type == "leveldb") {
            result = restoreLevelDBBackup(backup_path, target_path, options);
        } else if (info.backend_type == "sqlite") {
            result = restoreSQLiteBackup(backup_path, target_path, options);
        } else {
            reportProgress("Unsupported backend for restore: " + info.backend_type);
            return BackupResult::FAILED;
        }
        
        auto end_time = std::chrono::high_resolution_clock::now();
        auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count();
        
        if (result == BackupResult::SUCCESS) {
            // Validate chain consistency if requested
            if (options.validate_chain_consistency) {
                reportProgress("Validating chain consistency");
                if (!validateChainConsistency(target_path)) {
                    reportProgress("Chain consistency validation failed");
                    return BackupResult::CORRUPTION_DETECTED;
                }
            }
            
            reportProgress("Restore completed successfully in " + std::to_string(duration_ms) + "ms");
            
            if (g_storage_metrics) {
                g_storage_metrics->recordRestore(duration_ms * 1000, true, info.backup_size_bytes);
            }
        } else {
            reportProgress("Restore failed");
            
            if (g_storage_metrics) {
                g_storage_metrics->recordRestore(duration_ms * 1000, false, 0);
                g_storage_metrics->recordError("restore", "restore_failed");
            }
        }
        
        return result;
        
    } catch (const std::exception& e) {
        reportProgress("Restore failed with exception: " + std::string(e.what()));
        
        if (g_storage_metrics) {
            g_storage_metrics->recordError("restore", "exception");
        }
        
        return BackupResult::FAILED;
    }
}

std::vector<BackupInfo> BackupManager::listBackups(const std::string& backup_root_path) {
    std::vector<BackupInfo> backups;
    
    try {
        if (!std::filesystem::exists(backup_root_path)) {
            return backups;
        }
        
        for (const auto& entry : std::filesystem::directory_iterator(backup_root_path)) {
            if (entry.is_directory()) {
                BackupInfo info = loadBackupMetadata(entry.path().string());
                if (!info.backup_id.empty()) {
                    backups.append(info);
                }
            }
        }
        
        // Sort by creation time (newest first)
        std::sort(backups.begin(), backups.end(), 
                 [](const BackupInfo& a, const BackupInfo& b) {
                     return a.created_at > b.created_at;
                 });
        
    } catch (const std::exception& e) {
        reportProgress("Failed to list backups: " + std::string(e.what()));
    }
    
    return backups;
}

BackupInfo BackupManager::getBackupInfo(const std::string& backup_path) {
    return loadBackupMetadata(backup_path);
}

int BackupManager::cleanupOldBackups(const std::string& backup_root_path, 
                                    int max_backups, 
                                    int max_age_days) {
    int cleaned_count = 0;
    
    try {
        std::vector<BackupInfo> backups = listBackups(backup_root_path);
        
        auto now = std::chrono::system_clock::now();
        auto max_age = std::chrono::hours(24 * max_age_days);
        
        // Remove backups older than max_age_days
        for (const auto& backup : backups) {
            auto age = now - backup.created_at;
            if (age > max_age) {
                reportProgress("Removing old backup: " + backup.backup_id);
                std::filesystem::remove_all(backup.backup_path);
                cleaned_count++;
            }
        }
        
        // Remove excess backups beyond max_backups
        if (static_cast<int>(backups.size()) > max_backups) {
            for (size_t i = max_backups; i < backups.size(); i++) {
                reportProgress("Removing excess backup: " + backups[i].backup_id);
                std::filesystem::remove_all(backups[i].backup_path);
                cleaned_count++;
            }
        }
        
    } catch (const std::exception& e) {
        reportProgress("Cleanup failed: " + std::string(e.what()));
    }
    
    return cleaned_count;
}

uint64_t BackupManager::estimateBackupSize() {
    try {
        auto stats = storage_->getStats();
        return stats.total_size_bytes;
    } catch (...) {
        return 0;
    }
}

uint64_t BackupManager::getAvailableSpace(const std::string& backup_path) {
    try {
        std::filesystem::path path(backup_path);
        auto space_info = std::filesystem::space(path.parent_path());
        return space_info.available;
    } catch (...) {
        return 0;
    }
}

void BackupManager::setCompressionLevel(int level) {
    compression_level_ = std::clamp(level, 0, 9);
}

void BackupManager::setVerificationLevel(int level) {
    verification_level_ = std::clamp(level, 0, 3);
}

void BackupManager::setProgressReporting(bool enabled) {
    progress_reporting_ = enabled;
}

// Backend-specific implementations

BackupResult BackupManager::createRocksDBBackup(const std::string& backup_path, const BackupOptions& options) {
    try {
        reportProgress("Creating RocksDB backup using checkpoint");
        
        // Use RocksDB checkpoint API for consistent backup
        StorageResult result = storage_->backup(backup_path);
        
        if (result == StorageResult::SUCCESS) {
            reportProgress("RocksDB checkpoint created successfully");
            return BackupResult::SUCCESS;
        } else {
            reportProgress("RocksDB checkpoint failed");
            return BackupResult::FAILED;
        }
        
    } catch (const std::exception& e) {
        reportProgress("RocksDB backup failed: " + std::string(e.what()));
        return BackupResult::FAILED;
    }
}

BackupResult BackupManager::createLevelDBBackup(const std::string& backup_path, const BackupOptions& options) {
    try {
        reportProgress("Creating LevelDB backup using file copy");
        
        // LevelDB doesn't have checkpoint API, use file-level copy
        // First, get the source directory from storage
        auto stats = storage_->getStats();
        std::string source_dir = stats.data_directory;
        
        if (source_dir.empty()) {
            reportProgress("Cannot determine LevelDB data directory");
            return BackupResult::FAILED;
        }
        
        // Copy all LevelDB files
        if (!copyDirectory(source_dir, backup_path)) {
            reportProgress("Failed to copy LevelDB files");
            return BackupResult::FAILED;
        }
        
        reportProgress("LevelDB file copy completed successfully");
        return BackupResult::SUCCESS;
        
    } catch (const std::exception& e) {
        reportProgress("LevelDB backup failed: " + std::string(e.what()));
        return BackupResult::FAILED;
    }
}

BackupResult BackupManager::createSQLiteBackup(const std::string& backup_path, const BackupOptions& options) {
    try {
        reportProgress("Creating SQLite backup");
        
        // Use storage interface backup method
        StorageResult result = storage_->backup(backup_path);
        
        if (result == StorageResult::SUCCESS) {
            reportProgress("SQLite backup completed successfully");
            return BackupResult::SUCCESS;
        } else {
            reportProgress("SQLite backup failed");
            return BackupResult::FAILED;
        }
        
    } catch (const std::exception& e) {
        reportProgress("SQLite backup failed: " + std::string(e.what()));
        return BackupResult::FAILED;
    }
}

BackupResult BackupManager::restoreRocksDBBackup(const std::string& backup_path, const std::string& target_path, const RestoreOptions& options) {
    try {
        reportProgress("Restoring RocksDB backup");
        
        // Copy checkpoint files to target directory
        if (!copyDirectory(backup_path, target_path)) {
            reportProgress("Failed to copy RocksDB backup files");
            return BackupResult::FAILED;
        }
        
        reportProgress("RocksDB restore completed successfully");
        return BackupResult::SUCCESS;
        
    } catch (const std::exception& e) {
        reportProgress("RocksDB restore failed: " + std::string(e.what()));
        return BackupResult::FAILED;
    }
}

BackupResult BackupManager::restoreLevelDBBackup(const std::string& backup_path, const std::string& target_path, const RestoreOptions& options) {
    try {
        reportProgress("Restoring LevelDB backup");
        
        // Copy backup files to target directory
        if (!copyDirectory(backup_path, target_path)) {
            reportProgress("Failed to copy LevelDB backup files");
            return BackupResult::FAILED;
        }
        
        reportProgress("LevelDB restore completed successfully");
        return BackupResult::SUCCESS;
        
    } catch (const std::exception& e) {
        reportProgress("LevelDB restore failed: " + std::string(e.what()));
        return BackupResult::FAILED;
    }
}

BackupResult BackupManager::restoreSQLiteBackup(const std::string& backup_path, const std::string& target_path, const RestoreOptions& options) {
    try {
        reportProgress("Restoring SQLite backup");
        
        // Use storage interface restore method
        StorageResult result = storage_->restore(backup_path);
        
        if (result == StorageResult::SUCCESS) {
            reportProgress("SQLite restore completed successfully");
            return BackupResult::SUCCESS;
        } else {
            reportProgress("SQLite restore failed");
            return BackupResult::FAILED;
        }
        
    } catch (const std::exception& e) {
        reportProgress("SQLite restore failed: " + std::string(e.what()));
        return BackupResult::FAILED;
    }
}

// Utility methods

BackupInfo BackupManager::createBackupMetadata(const std::string& backup_path, const BackupOptions& options) {
    BackupInfo info;
    
    info.backup_id = options.backup_id.empty() ? 
        "backup_" + std::to_string(std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count()) : 
        options.backup_id;
    
    info.backup_path = backup_path;
    info.backend_type = storage_->name();
    info.created_at = std::chrono::system_clock::now();
    info.block_height = getCurrentBlockHeight();
    info.chain_tip_hash = getCurrentChainTip();
    
    auto stats = storage_->getStats();
    info.source_directory = stats.data_directory;
    
    return info;
}

bool BackupManager::saveBackupMetadata(const BackupInfo& info) {
    try {
        Json::Value j;
        j["backup_id"] = info.backup_id;
        j["backup_path"] = info.backup_path;
        j["backend_type"] = info.backend_type;
        j["source_directory"] = info.source_directory;
        j["created_at"] = std::chrono::duration_cast<std::chrono::seconds>(
            info.created_at.time_since_epoch()).count();
        j["backup_size_bytes"] = info.backup_size_bytes;
        j["block_height"] = info.block_height;
        j["chain_tip_hash"] = info.chain_tip_hash;
        j["is_complete"] = info.is_complete;
        j["is_verified"] = info.is_verified;
        j["checksum"] = info.checksum;
        j["file_list"] = info.file_list;
        
        std::string metadata_path = info.backup_path + "/backup_metadata.json";
        std::ofstream file(metadata_path);
        file << jJson::StyledWriter().write();
        
        return file.good();
        
    } catch (...) {
        return false;
    }
}

BackupInfo BackupManager::loadBackupMetadata(const std::string& backup_path) {
    BackupInfo info;
    
    try {
        std::string metadata_path = backup_path + "/backup_metadata.json";
        if (!std::filesystem::exists(metadata_path)) {
            return info; // Empty info indicates not found
        }
        
        std::ifstream file(metadata_path);
        Json::Value j;
        file >> j;
        
        info.backup_id = j.isMember("backup_id") ? "backup_id" : "";
        info.backup_path = j.isMember("backup_path") ? "backup_path" : backup_path;
        info.backend_type = j.isMember("backend_type") ? "backend_type" : "";
        info.source_directory = j.isMember("source_directory") ? "source_directory" : "";
        
        if (j.isMember("created_at")) {
            auto timestamp = j["created_at"].asInt64();
            info.created_at = std::chrono::system_clock::from_time_t(timestamp);
        }
        
        info.backup_size_bytes = j.isMember("backup_size_bytes") ? "backup_size_bytes" : 0ULL;
        info.block_height = j.isMember("block_height") ? "block_height" : 0ULL;
        info.chain_tip_hash = j.isMember("chain_tip_hash") ? "chain_tip_hash" : "";
        info.is_complete = j.isMember("is_complete") ? "is_complete" : false;
        info.is_verified = j.isMember("is_verified") ? "is_verified" : false;
        info.checksum = j.isMember("checksum") ? "checksum" : "";
        
        if (j.isMember("file_list")) {
            info.file_list = j["file_list"].get<std::vector<std::string>>();
        }
        
    } catch (...) {
        // Return empty info on any error
        info = BackupInfo{};
    }
    
    return info;
}

std::string BackupManager::calculateChecksum(const std::string& directory) {
    // Simple SHA256 checksum of all files in directory
    // TODO: Implement proper checksum calculation
    return "checksum_placeholder";
}

bool BackupManager::verifyChecksum(const std::string& directory, const std::string& expected_checksum) {
    std::string actual_checksum = calculateChecksum(directory);
    return actual_checksum == expected_checksum;
}

uint64_t BackupManager::calculateDirectorySize(const std::string& directory) {
    uint64_t size = 0;
    
    try {
        for (const auto& entry : std::filesystem::recursive_directory_iterator(directory)) {
            if (entry.is_regular_file()) {
                size += entry.file_size();
            }
        }
    } catch (...) {
        // Ignore errors
    }
    
    return size;
}

std::vector<std::string> BackupManager::getFileList(const std::string& directory) {
    std::vector<std::string> files;
    
    try {
        for (const auto& entry : std::filesystem::recursive_directory_iterator(directory)) {
            if (entry.is_regular_file()) {
                files.append(entry.path().string());
            }
        }
        
        std::sort(files.begin(), files.end());
    } catch (...) {
        // Ignore errors
    }
    
    return files;
}

bool BackupManager::copyDirectory(const std::string& source, const std::string& destination) {
    try {
        std::filesystem::copy(source, destination, 
                            std::filesystem::copy_options::recursive |
                            std::filesystem::copy_options::overwrite_existing);
        return true;
    } catch (...) {
        return false;
    }
}

bool BackupManager::createDirectoryStructure(const std::string& path) {
    try {
        std::filesystem::create_directories(path);
        return true;
    } catch (...) {
        return false;
    }
}

void BackupManager::reportProgress(const std::string& message) {
    if (progress_reporting_) {
        // TODO: Use proper logging system
        std::cout << "[BackupManager] " << message << std::endl;
    }
}

bool BackupManager::validateChainConsistency(const std::string& data_path) {
    // TODO: Implement chain consistency validation
    // This would verify that the restored data forms a valid blockchain
    return true;
}

uint64_t BackupManager::getCurrentBlockHeight() {
    try {
        auto stats = storage_->getStats();
        return stats.block_count;
    } catch (...) {
        return 0;
    }
}

std::string BackupManager::getCurrentChainTip() {
    try {
        // TODO: Get actual chain tip hash from storage
        return "chain_tip_placeholder";
    } catch (...) {
        return "";
    }
}

// Factory implementation

std::unique_ptr<BackupManager> BackupManagerFactory::create(StorageInterface* storage) {
    return std::make_unique<BackupManager>(storage);
}

std::unique_ptr<BackupManager> BackupManagerFactory::create(StorageInterface* storage, 
                                                          int compression_level,
                                                          int verification_level) {
    auto manager = std::make_unique<BackupManager>(storage);
    manager->setCompressionLevel(compression_level);
    manager->setVerificationLevel(verification_level);
    return manager;
}

} // namespace storage
} // namespace dinero
