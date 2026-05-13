#pragma once

#include "rpc_registry.h"
#include "din_json.h"

namespace dinero {
namespace rpc {

/**
 * Register all storage-related RPC handlers
 */
void registerStorageRpcHandlers(RpcRegistry& registry);

/**
 * Get database statistics and health information
 * 
 * Returns detailed statistics about the storage backends including:
 * - RocksDB properties (if enabled)
 * - LevelDB approximate sizes (if enabled) 
 * - General storage metrics
 * - Resource usage
 * - Performance statistics
 */
din::Json handleGetDbStats(const ExecutionContext& ctx, const din::Json& params);

/**
 * Get storage health status
 * 
 * Returns overall health status and any active alerts
 */
din::Json handleGetStorageHealth(const ExecutionContext& ctx, const din::Json& params);

/**
 * Get detailed storage metrics
 * 
 * Returns comprehensive metrics from the storage metrics collector
 */
din::Json handleGetStorageMetrics(const ExecutionContext& ctx, const din::Json& params);

/**
 * Trigger manual compaction
 * 
 * Admin RPC to trigger storage compaction
 */
din::Json handleCompactStorage(const ExecutionContext& ctx, const din::Json& params);

/**
 * Create storage checkpoint/backup
 * 
 * Admin RPC to create a storage checkpoint
 */
din::Json handleCheckpointStorage(const ExecutionContext& ctx, const din::Json& params);

/**
 * Reset storage metrics counters
 * 
 * Admin RPC to reset metrics for fresh monitoring period
 */
din::Json handleResetStorageMetrics(const ExecutionContext& ctx, const din::Json& params);

} // namespace rpc
} // namespace dinero
