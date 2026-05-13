#include "rpc/storage_rpc_handlers.h"
#include "storage/storage_factory.h"
#include "storage/storage_metrics.h"
#include "storage/storage_guardrails.h"
#include "storage/backpressure_manager.h"
#include <sstream>
#include <iomanip>

namespace dinero {
namespace rpc {

void registerStorageRpcHandlers(RpcRegistry& registry) {
    // Database statistics RPC
    RpcMethodMeta getdbstats_meta;
    getdbstats_meta.name = "getdbstats";
    getdbstats_meta.ns = "storage";
    getdbstats_meta.description = "Get detailed database statistics and properties";
    getdbstats_meta.help = "Returns comprehensive statistics about storage backends including RocksDB properties, LevelDB sizes, metrics, and resource usage";
    getdbstats_meta.result = {"object", "Database statistics object"};
    
    registry.registerHandler("getdbstats", handleGetDbStats, getdbstats_meta, "storage");
    
    // Storage health RPC
    RpcMethodMeta health_meta;
    health_meta.name = "getstoragehealth";
    health_meta.ns = "storage";
    health_meta.description = "Get storage system health status";
    health_meta.help = "Returns overall health status and any active storage alerts";
    health_meta.result = {"object", "Storage health status object"};
    
    registry.registerHandler("getstoragehealth", handleGetStorageHealth, health_meta, "storage");
    
    // Storage metrics RPC
    RpcMethodMeta metrics_meta;
    metrics_meta.name = "getstoragemetrics";
    metrics_meta.ns = "storage";
    metrics_meta.description = "Get detailed storage performance metrics";
    metrics_meta.help = "Returns comprehensive performance metrics from storage operations";
    
    RpcParamMeta format_param;
    format_param.name = "format";
    format_param.type = "string";
    format_param.desc = "Output format: 'json' (default) or 'prometheus'";
    format_param.required = false;
    metrics_meta.params.append(format_param);
    
    metrics_meta.result = {"object", "Storage metrics object"};
    
    registry.registerHandler("getstoragemetrics", handleGetStorageMetrics, metrics_meta, "storage");
    
    // Admin RPCs
    RpcMethodMeta compact_meta;
    compact_meta.name = "compactstorage";
    compact_meta.ns = "storage";
    compact_meta.description = "Trigger manual storage compaction";
    compact_meta.help = "Manually trigger storage compaction to optimize space and performance";
    compact_meta.result = {"object", "Compaction result"};
    
    registry.registerHandler("compactstorage", handleCompactStorage, compact_meta, "storage");
    
    RpcMethodMeta checkpoint_meta;
    checkpoint_meta.name = "checkpointstorage";
    checkpoint_meta.ns = "storage";
    checkpoint_meta.description = "Create storage checkpoint/backup";
    checkpoint_meta.help = "Create a consistent checkpoint of the storage for backup purposes";
    
    RpcParamMeta path_param;
    path_param.name = "path";
    path_param.type = "string";
    path_param.desc = "Backup directory path";
    path_param.required = true;
    checkpoint_meta.params.append(path_param);
    
    checkpoint_meta.result = {"object", "Checkpoint result"};
    
    registry.registerHandler("checkpointstorage", handleCheckpointStorage, checkpoint_meta, "storage");
    
    RpcMethodMeta reset_meta;
    reset_meta.name = "resetstoragemetrics";
    reset_meta.ns = "storage";
    reset_meta.description = "Reset storage metrics counters";
    reset_meta.help = "Reset all storage metrics counters for a fresh monitoring period";
    reset_meta.result = {"object", "Reset result"};
    
    registry.registerHandler("resetstoragemetrics", handleResetStorageMetrics, reset_meta, "storage");
}

din::Json handleGetDbStats(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;
    
    try {
        auto* storage = GetGlobalStorage();
        if (!storage) {
            result["error"] = "Storage not initialized";
            return result;
        }
        
        // Get basic storage stats
        auto storage_stats = storage->getStats();
        
        result["backend"] = storage_stats.backend_name;
        result["data_directory"] = storage_stats.data_directory;
        result["is_healthy"] = storage_stats.is_healthy;
        result["uptime_seconds"] = storage_stats.uptime_seconds;
        
        // Database size information
        din::Json size_info;
        size_info["total_size_bytes"] = storage_stats.total_size_bytes;
        size_info["data_size_bytes"] = storage_stats.data_size_bytes;
        size_info["index_size_bytes"] = storage_stats.index_size_bytes;
        size_info["log_size_bytes"] = storage_stats.log_size_bytes;
        result["size"] = size_info;
        
        // Operation counts
        din::Json operations;
        operations["total_reads"] = storage_stats.total_reads;
        operations["total_writes"] = storage_stats.total_writes;
        operations["total_deletes"] = storage_stats.total_deletes;
        operations["failed_operations"] = storage_stats.failed_operations;
        result["operations"] = operations;
        
        // Performance metrics
        din::Json performance;
        performance["avg_read_latency_us"] = storage_stats.avg_read_latency_us;
        performance["avg_write_latency_us"] = storage_stats.avg_write_latency_us;
        performance["p95_read_latency_us"] = storage_stats.p95_read_latency_us;
        performance["p95_write_latency_us"] = storage_stats.p95_write_latency_us;
        result["performance"] = performance;
        
        // Backend-specific properties
        din::Json backend_props;
        
        if (storage_stats.backend_name == "rocksdb") {
            // RocksDB specific properties
            backend_props["num_immutable_mem_tables"] = storage_stats.rocksdb_properties["rocksdb.num-immutable-mem-table"];
            backend_props["mem_table_flush_pending"] = storage_stats.rocksdb_properties["rocksdb.mem-table-flush-pending"];
            backend_props["compaction_pending"] = storage_stats.rocksdb_properties["rocksdb.compaction-pending"];
            backend_props["background_errors"] = storage_stats.rocksdb_properties["rocksdb.background-errors"];
            backend_props["cur_size_active_mem_table"] = storage_stats.rocksdb_properties["rocksdb.cur-size-active-mem-table"];
            backend_props["cur_size_all_mem_tables"] = storage_stats.rocksdb_properties["rocksdb.cur-size-all-mem-tables"];
            backend_props["num_entries_active_mem_table"] = storage_stats.rocksdb_properties["rocksdb.num-entries-active-mem-table"];
            backend_props["num_entries_imm_mem_tables"] = storage_stats.rocksdb_properties["rocksdb.num-entries-imm-mem-tables"];
            backend_props["estimate_num_keys"] = storage_stats.rocksdb_properties["rocksdb.estimate-num-keys"];
            backend_props["estimate_table_readers_mem"] = storage_stats.rocksdb_properties["rocksdb.estimate-table-readers-mem"];
            backend_props["num_snapshots"] = storage_stats.rocksdb_properties["rocksdb.num-snapshots"];
            backend_props["num_live_versions"] = storage_stats.rocksdb_properties["rocksdb.num-live-versions"];
            backend_props["estimate_live_data_size"] = storage_stats.rocksdb_properties["rocksdb.estimate-live-data-size"];
            backend_props["total_sst_files_size"] = storage_stats.rocksdb_properties["rocksdb.total-sst-files-size"];
            backend_props["block_cache_usage"] = storage_stats.rocksdb_properties["rocksdb.block-cache-usage"];
            backend_props["block_cache_pinned_usage"] = storage_stats.rocksdb_properties["rocksdb.block-cache-pinned-usage"];
            
            // Compaction stats
            din::Json compaction_stats;
            compaction_stats["level0_files"] = storage_stats.rocksdb_properties["rocksdb.num-files-at-level0"];
            compaction_stats["level1_files"] = storage_stats.rocksdb_properties["rocksdb.num-files-at-level1"];
            compaction_stats["level2_files"] = storage_stats.rocksdb_properties["rocksdb.num-files-at-level2"];
            compaction_stats["level3_files"] = storage_stats.rocksdb_properties["rocksdb.num-files-at-level3"];
            compaction_stats["level4_files"] = storage_stats.rocksdb_properties["rocksdb.num-files-at-level4"];
            compaction_stats["level5_files"] = storage_stats.rocksdb_properties["rocksdb.num-files-at-level5"];
            compaction_stats["level6_files"] = storage_stats.rocksdb_properties["rocksdb.num-files-at-level6"];
            backend_props["compaction_stats"] = compaction_stats;
            
        } else if (storage_stats.backend_name == "leveldb") {
            // LevelDB specific approximate sizes
            backend_props["approximate_sizes"] = din::Json::object();
            
            // Get approximate sizes for different key ranges
            std::vector<std::pair<std::string, std::string>> ranges = {
                {"block:", "block;"},           // Block data
                {"tx:", "tx;"},                 // Transaction data  
                {"utxo:", "utxo;"},            // UTXO data
                {"chain:", "chain;"},          // Chain state
                {"height:", "height;"},        // Height index
                {"meta:", "meta;"}             // Metadata
            };
            
            for (const auto& range : ranges) {
                uint64_t size = storage->getApproximateSize(range.first, range.second);
                backend_props["approximate_sizes"][range.first.substr(0, range.first.length()-1)] = size;
            }
            
            // LevelDB stats (if available)
            backend_props["leveldb_stats"] = storage_stats.leveldb_stats;
        }
        
        result["backend_properties"] = backend_props;
        
        // Resource usage
        din::Json resources;
        resources["memory_usage_bytes"] = storage_stats.memory_usage_bytes;
        resources["file_descriptors"] = storage_stats.open_file_descriptors;
        resources["compaction_debt_bytes"] = storage_stats.compaction_debt_bytes;
        result["resources"] = resources;
        
        // Storage metrics integration
        if (g_storage_metrics) {
            auto metrics_summary = g_storage_metrics->getSummary();
            
            din::Json metrics_info;
            metrics_info["total_operations"] = metrics_summary.total_operations;
            metrics_info["successful_operations"] = metrics_summary.successful_operations;
            metrics_info["failed_operations"] = metrics_summary.failed_operations;
            metrics_info["success_rate_percent"] = metrics_summary.success_rate;
            metrics_info["avg_latency_microseconds"] = metrics_summary.avg_latency_micros;
            metrics_info["p95_latency_microseconds"] = metrics_summary.p95_latency_micros;
            metrics_info["p99_latency_microseconds"] = metrics_summary.p99_latency_micros;
            metrics_info["total_bytes_processed"] = metrics_summary.total_bytes_processed;
            
            result["metrics"] = metrics_info;
        }
        
        result["timestamp"] = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        
    } catch (const std::exception& e) {
        result["error"] = std::string("Failed to get database statistics: ") + e.what();
    }
    
    return result;
}

din::Json handleGetStorageHealth(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;
    
    try {
        auto* storage = GetGlobalStorage();
        if (!storage) {
            result["healthy"] = false;
            result["status"] = "Storage not initialized";
            return result;
        }
        
        auto storage_stats = storage->getStats();
        result["healthy"] = storage_stats.is_healthy;
        result["backend"] = storage_stats.backend_name;
        result["uptime_seconds"] = storage_stats.uptime_seconds;
        
        // Check for critical issues
        din::Json issues = din::Json::array();
        
        if (storage_stats.failed_operations > 0) {
            din::Json issue;
            issue["type"] = "failed_operations";
            issue["count"] = storage_stats.failed_operations;
            issue["severity"] = "warning";
            issues.append(issue);
        }
        
        if (storage_stats.compaction_debt_bytes > 1024 * 1024 * 1024) { // > 1GB
            din::Json issue;
            issue["type"] = "high_compaction_debt";
            issue["debt_bytes"] = storage_stats.compaction_debt_bytes;
            issue["severity"] = "critical";
            issues.append(issue);
        }
        
        if (storage_stats.memory_usage_bytes > 2ULL * 1024 * 1024 * 1024) { // > 2GB
            din::Json issue;
            issue["type"] = "high_memory_usage";
            issue["memory_bytes"] = storage_stats.memory_usage_bytes;
            issue["severity"] = "warning";
            issues.append(issue);
        }
        
        result["issues"] = issues;
        result["issue_count"] = issues.size();
        
        // Performance indicators
        din::Json performance;
        performance["avg_read_latency_us"] = storage_stats.avg_read_latency_us;
        performance["avg_write_latency_us"] = storage_stats.avg_write_latency_us;
        performance["read_performance"] = storage_stats.avg_read_latency_us < 1000 ? "good" : 
                                        storage_stats.avg_read_latency_us < 10000 ? "fair" : "poor";
        performance["write_performance"] = storage_stats.avg_write_latency_us < 5000 ? "good" :
                                         storage_stats.avg_write_latency_us < 50000 ? "fair" : "poor";
        result["performance"] = performance;
        
        result["timestamp"] = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        
    } catch (const std::exception& e) {
        result["healthy"] = false;
        result["error"] = std::string("Failed to get storage health: ") + e.what();
    }
    
    return result;
}

din::Json handleGetStorageMetrics(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;
    
    try {
        if (!g_storage_metrics) {
            result["error"] = "Storage metrics not initialized";
            return result;
        }
        
        std::string format = "json";
        if (params.isMember("format") && params["format"].isString()) {
            format = params["format"].asString();
        }
        
        if (format == "prometheus") {
            result["format"] = "prometheus";
            result["data"] = g_storage_metrics->exportPrometheus();
        } else {
            // Parse JSON export
            std::string json_export = g_storage_metrics->exportJSON();
            result = din::Json::parse(json_export);
            result["format"] = "json";
        }
        
        result["timestamp"] = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        
    } catch (const std::exception& e) {
        result["error"] = std::string("Failed to get storage metrics: ") + e.what();
    }
    
    return result;
}

din::Json handleCompactStorage(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;
    
    try {
        auto* storage = GetGlobalStorage();
        if (!storage) {
            result["success"] = false;
            result["error"] = "Storage not initialized";
            return result;
        }
        
        auto start_time = std::chrono::high_resolution_clock::now();
        
        auto compact_result = storage->compact();
        
        auto end_time = std::chrono::high_resolution_clock::now();
        auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count();
        
        result["success"] = (compact_result == StorageResult::SUCCESS);
        result["duration_ms"] = duration_ms;
        
        if (compact_result == StorageResult::SUCCESS) {
            result["message"] = "Storage compaction completed successfully";
            
            // Record compaction metrics
            if (g_storage_metrics) {
                g_storage_metrics->recordCompaction(duration_ms * 1000, true, 0); // Convert to microseconds
            }
        } else {
            result["error"] = "Compaction failed";
            result["result_code"] = static_cast<int>(compact_result);
            
            if (g_storage_metrics) {
                g_storage_metrics->recordCompaction(duration_ms * 1000, false, 0);
                g_storage_metrics->recordError("compaction", "manual_compaction_failed");
            }
        }
        
        result["timestamp"] = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        
    } catch (const std::exception& e) {
        result["success"] = false;
        result["error"] = std::string("Compaction failed with exception: ") + e.what();
        
        if (g_storage_metrics) {
            g_storage_metrics->recordError("compaction", "exception");
        }
    }
    
    return result;
}

din::Json handleCheckpointStorage(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;
    
    try {
        if (!params.isMember("path") || !params["path"].isString()) {
            result["success"] = false;
            result["error"] = "Missing required parameter: path";
            return result;
        }
        
        std::string backup_path = params["path"].asString();
        
        auto* storage = GetGlobalStorage();
        if (!storage) {
            result["success"] = false;
            result["error"] = "Storage not initialized";
            return result;
        }
        
        auto start_time = std::chrono::high_resolution_clock::now();
        
        auto backup_result = storage->backup(backup_path);
        
        auto end_time = std::chrono::high_resolution_clock::now();
        auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count();
        
        result["success"] = (backup_result == StorageResult::SUCCESS);
        result["backup_path"] = backup_path;
        result["duration_ms"] = duration_ms;
        
        if (backup_result == StorageResult::SUCCESS) {
            result["message"] = "Storage checkpoint created successfully";
            
            // Get backup size if possible
            try {
                uint64_t backup_size = 0;
                for (const auto& entry : std::filesystem::recursive_directory_iterator(backup_path)) {
                    if (entry.is_regular_file()) {
                        backup_size += entry.file_size();
                    }
                }
                result["backup_size_bytes"] = backup_size;
            } catch (...) {
                // Ignore size calculation errors
            }
        } else {
            result["error"] = "Checkpoint creation failed";
            result["result_code"] = static_cast<int>(backup_result);
        }
        
        result["timestamp"] = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        
    } catch (const std::exception& e) {
        result["success"] = false;
        result["error"] = std::string("Checkpoint failed with exception: ") + e.what();
    }
    
    return result;
}

din::Json handleResetStorageMetrics(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;
    
    try {
        if (!g_storage_metrics) {
            result["success"] = false;
            result["error"] = "Storage metrics not initialized";
            return result;
        }
        
        // Get summary before reset for reporting
        auto summary_before = g_storage_metrics->getSummary();
        
        g_storage_metrics->reset();
        
        result["success"] = true;
        result["message"] = "Storage metrics counters reset successfully";
        
        // Report what was reset
        din::Json reset_info;
        reset_info["operations_cleared"] = summary_before.total_operations;
        reset_info["bytes_cleared"] = summary_before.total_bytes_processed;
        reset_info["errors_cleared"] = summary_before.error_counts.size();
        result["reset_info"] = reset_info;
        
        result["timestamp"] = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        
    } catch (const std::exception& e) {
        result["success"] = false;
        result["error"] = std::string("Reset failed with exception: ") + e.what();
    }
    
    return result;
}

} // namespace rpc
} // namespace dinero
