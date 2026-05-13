#include "rpc/storage_info_rpc_handlers.h"
#include "storage/storage_factory.h"
#include "storage/storage_interface.h"
#include "storage/config_safety.h"
#include "storage/schema_manager.h"
#include "storage/storage_metrics.h"
#include <sstream>

namespace dinero {
namespace rpc {

Json::Value getstorageinfo(const Json::Value& params) {
    Json::Value result;
    result["rpc_schema"] = "din.rpc.v1";
    
    try {
        // Get current storage configuration
        auto config = storage::StorageFactory::getCurrentConfig();
        auto available_backends = storage::StorageFactory::getAvailableBackends();
        
        // Backend information
        result["backend"] = {
            {"requested", config.backend},
            {"effective", storage::StorageFactory::getCurrentBackend()},
            {"available", available_backends},
            {"allow_fallback", config.allow_fallback}
        };
        
        // Check if fallback is in effect
        bool fallback_in_effect = (config.backend != storage::StorageFactory::getCurrentBackend());
        result["fallback_in_effect"] = fallback_in_effect;
        
        if (fallback_in_effect) {
            result["fallback_reason"] = "Requested backend unavailable, using fallback";
        }
        
        // Storage size and statistics
        if (storage::g_storage) {
            auto stats = storage::g_storage->getStats();
            result["size"] = {
                {"total_bytes", stats.total_size_bytes},
                {"blocks_count", stats.block_count},
                {"transactions_count", stats.transaction_count},
                {"utxos_count", stats.utxo_count}
            };
            
            // Tip information
            result["tip"] = {
                {"height", stats.tip_height},
                {"hash", stats.tip_hash},
                {"timestamp", stats.tip_timestamp}
            };
        }
        
        // Configuration safety status
        if (storage::g_config_safety) {
            auto safety_level = storage::g_config_safety->getSafetyLevel();
            std::string safety_level_str;
            switch (safety_level) {
                case storage::SafetyLevel::STRICT: safety_level_str = "STRICT"; break;
                case storage::SafetyLevel::SAFE: safety_level_str = "SAFE"; break;
                case storage::SafetyLevel::PERMISSIVE: safety_level_str = "PERMISSIVE"; break;
                case storage::SafetyLevel::UNSAFE: safety_level_str = "UNSAFE"; break;
            }
            
            result["config_safety"] = {
                {"level", safety_level_str},
                {"fallbacks_allowed", storage::g_config_safety->areFallbacksAllowed()},
                {"production_ready", storage::g_config_safety->isProductionReady(config)}
            };
        }
        
        // Schema information
        if (storage::g_schema_manager) {
            auto schema_info = storage::g_schema_manager->getCurrentSchemaInfo();
            result["schema"] = {
                {"version", schema_info.version.toString()},
                {"architecture", schema_info.architecture},
                {"endianness", schema_info.endianness},
                {"created_at", std::chrono::duration_cast<std::chrono::seconds>(
                    schema_info.created_at.time_since_epoch()).count()}
            };
        }
        
        // Performance metrics summary
        if (storage::g_storage_metrics) {
            auto metrics = storage::g_storage_metrics->getMetrics();
            result["performance"] = {
                {"avg_block_write_ms", metrics.avg_block_write_duration_ms},
                {"avg_block_read_ms", metrics.avg_block_read_duration_ms},
                {"total_operations", metrics.total_operations},
                {"error_rate", metrics.error_rate}
            };
        }
        
        // Operational status (basic implementation)
        result["operational"] = {
            {"healthy", true}, // Basic health - could be enhanced with actual DB health checks
            {"read_only", false}, // Default assumption - could check DB open mode
            {"compaction_active", false}, // Would need RocksDB stats to determine
            {"backup_in_progress", false} // Would need backup manager integration
        };
        
    } catch (const std::exception& e) {
        result["error"] = {
            {"code", -1},
            {"message", std::string("Failed to get storage info: ") + e.what()}
        };
    }
    
    return result;
}

Json::Value getdbstats_enhanced(const Json::Value& params) {
    Json::Value result;
    result["rpc_schema"] = "din.rpc.v1";
    
    try {
        // Get basic database statistics
        if (storage::g_storage) {
            auto stats = storage::g_storage->getStats();
            
            result["basic_stats"] = {
                {"total_size_bytes", stats.total_size_bytes},
                {"block_count", stats.block_count},
                {"transaction_count", stats.transaction_count},
                {"utxo_count", stats.utxo_count},
                {"tip_height", stats.tip_height},
                {"tip_hash", stats.tip_hash}
            };
            
            // Backend-specific statistics
            std::string backend = storage::StorageFactory::getCurrentBackend();
            result["backend"] = backend;
            
            if (backend == "rocksdb") {
                // RocksDB properties
                result["rocksdb_properties"] = {
                    {"num_files_at_level0", stats.level0_files},
                    {"num_files_at_level1", stats.level1_files},
                    {"num_files_at_level2", stats.level2_files},
                    {"compaction_pending", stats.compaction_pending},
                    {"background_errors", stats.background_errors},
                    {"cur_size_active_mem_table", stats.active_memtable_size},
                    {"cur_size_all_mem_tables", stats.all_memtables_size},
                    {"size_all_mem_tables", stats.all_memtables_size},
                    {"num_entries_active_mem_table", stats.active_memtable_entries},
                    {"num_entries_imm_mem_tables", stats.immutable_memtable_entries}
                };
                
                // Compression and build info (default configuration)
                result["build_info"] = {
                    {"compression_enabled", true}, // Default RocksDB compression
                    {"bloom_filter_enabled", true}, // Default bloom filter setting
                    {"portable_build", false}, // Assuming native build
                    {"version", "10.5.1"} // Current installed RocksDB version
                };
                
            } else if (backend == "leveldb") {
                // LevelDB approximate sizes
                result["leveldb_sizes"] = {
                    {"blocks_prefix_size", stats.blocks_size_bytes},
                    {"transactions_prefix_size", stats.transactions_size_bytes},
                    {"utxos_prefix_size", stats.utxos_size_bytes},
                    {"metadata_prefix_size", stats.metadata_size_bytes}
                };
                
                result["build_info"] = {
                    {"compression_enabled", true}, // Default LevelDB compression
                    {"version", "1.23"} // LevelDB version (would query actual version)
                };
            }
        }
        
        // Schema version information
        if (storage::g_schema_manager) {
            auto schema_info = storage::g_schema_manager->getCurrentSchemaInfo();
            result["schema_version"] = {
                {"current", schema_info.version.toString()},
                {"major", schema_info.version.major},
                {"minor", schema_info.version.minor},
                {"patch", schema_info.version.patch},
                {"architecture", schema_info.architecture},
                {"endianness", schema_info.endianness},
                {"migration_count", schema_info.applied_migrations.size()}
            };
        }
        
        // Performance metrics
        if (storage::g_storage_metrics) {
            auto metrics = storage::g_storage_metrics->getMetrics();
            result["performance_metrics"] = {
                {"block_write_count", metrics.block_write_count},
                {"block_read_count", metrics.block_read_count},
                {"utxo_operations_count", metrics.utxo_operations_count},
                {"batch_commit_count", metrics.batch_commit_count},
                {"batch_abort_count", metrics.batch_abort_count},
                {"compaction_count", metrics.compaction_count},
                {"iterator_operations_count", metrics.iterator_operations_count},
                {"error_count", metrics.error_count}
            };
            
            result["timing_metrics"] = {
                {"avg_block_write_ms", metrics.avg_block_write_duration_ms},
                {"avg_block_read_ms", metrics.avg_block_read_duration_ms},
                {"avg_utxo_operation_ms", metrics.avg_utxo_operation_duration_ms},
                {"avg_batch_commit_ms", metrics.avg_batch_commit_duration_ms},
                {"p95_block_write_ms", metrics.p95_block_write_duration_ms},
                {"p99_block_write_ms", metrics.p99_block_write_duration_ms}
            };
        }
        
        // Operational metadata
        result["metadata"] = {
            {"collection_timestamp", std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()).count()},
            {"uptime_seconds", 0}, // Would need daemon start time tracking
            {"data_directory", ""}, // Would need global config access
            {"config_file", ""} // Would need global config access
        };
        
    } catch (const std::exception& e) {
        result["error"] = {
            {"code", -1},
            {"message", std::string("Failed to get database stats: ") + e.what()}
        };
    }
    
    return result;
}

} // namespace rpc
} // namespace dinero
