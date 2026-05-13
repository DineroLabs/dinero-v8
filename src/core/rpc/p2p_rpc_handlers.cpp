#include "rpc/rpc_registry.h"
#include "p2p/headers_first_sync.h"
#include "daemon/daemon_context.h"
#include "daemon/services/compact_block_service.h"
#include "din_json.h"
#include "consensus/consensus.hpp"
#include <iostream>

using namespace dinero::rpc;
using namespace dinero::p2p;
using dinero::DaemonContext;

namespace {

// Headers-first sync RPC handlers
din::Json handleGetSyncStatus(const ExecutionContext& ctx, const din::Json& params) {
    if (!g_headers_sync) {
        din::Json error;
        error["code"] = -1;
        error["message"] = "Headers sync not initialized";
        throw std::runtime_error(din::dump(error));
    }
    
    din::Json result = g_headers_sync->getStatus();
    result["rpc_schema"] = "din.rpc.v1";
    return result;
}

din::Json handleStartSync(const ExecutionContext& ctx, const din::Json& params) {
    if (!g_headers_sync) {
        din::Json error;
        error["code"] = -1;
        error["message"] = "Headers sync not initialized";
        throw std::runtime_error(din::dump(error));
    }
    
    std::string peer_id = "default_peer";
    if (params.isArray() && params.size() > 0) {
        peer_id = params[0];
    } else if (params.isObject() && params.isMember("peer_id")) {
        peer_id = params["peer_id"];
    }
    
    g_headers_sync->startSync(peer_id);
    
    din::Json result;
    result["status"] = "started";
    result["peer_id"] = peer_id;
    result["rpc_schema"] = "din.rpc.v1";
    return result;
}

din::Json handleStopSync(const ExecutionContext& ctx, const din::Json& params) {
    if (!g_headers_sync) {
        din::Json error;
        error["code"] = -1;
        error["message"] = "Headers sync not initialized";
        throw std::runtime_error(din::dump(error));
    }
    
    g_headers_sync->stopSync();
    
    din::Json result;
    result["status"] = "stopped";
    result["rpc_schema"] = "din.rpc.v1";
    return result;
}

din::Json handleGetSyncMetrics(const ExecutionContext& ctx, const din::Json& params) {
    if (!g_headers_sync) {
        din::Json error;
        error["code"] = -1;
        error["message"] = "Headers sync not initialized";
        throw std::runtime_error(din::dump(error));
    }
    
    din::Json result = g_headers_sync->getMetrics();
    result["rpc_schema"] = "din.rpc.v1";
    return result;
}

// Compact blocks RPC handlers (Plan-A: Updated to use DaemonContext)
din::Json handleGetCompactBlockStats(const ExecutionContext& ctx, const din::Json& params) {
    auto* daemon_ctx = DaemonContext::instance();
    if (!daemon_ctx || !daemon_ctx->compact_blocks) {
        din::Json error;
        error["code"] = -1;
        error["message"] = "CompactBlockService not available";
        throw std::runtime_error(din::dump(error));
    }

    auto service = daemon_ctx->compact_blocks;
    din::Json result;
    result["blocks_processed"] = static_cast<uint64_t>(service->getBlocksProcessed());
    result["reconstruction_rate"] = service->getReconstructionRate();
    result["bandwidth_saved"] = static_cast<uint64_t>(service->getBandwidthSaved());
    result["rpc_schema"] = "din.rpc.v1";
    return result;
}

din::Json handleGetCompactBlockStatus(const ExecutionContext& ctx, const din::Json& params) {
    auto* daemon_ctx = DaemonContext::instance();
    if (!daemon_ctx || !daemon_ctx->compact_blocks) {
        din::Json error;
        error["code"] = -1;
        error["message"] = "CompactBlockService not available";
        throw std::runtime_error(din::dump(error));
    }

    auto service = daemon_ctx->compact_blocks;
    din::Json result;
    result["enabled"] = true;
    result["protocol"] = "BIP152";
    result["wire_format"] = "binary";
    result["short_id_length"] = 6;
    result["blocks_processed"] = static_cast<uint64_t>(service->getBlocksProcessed());
    result["reconstruction_rate"] = service->getReconstructionRate();
    result["rpc_schema"] = "din.rpc.v1";
    return result;
}

din::Json handleSetCompactBlocksEnabled(const ExecutionContext& ctx, const din::Json& params) {
    // Compact blocks are always enabled with binary wire format
    // This handler is kept for API compatibility but is a no-op
    bool enabled = true;
    if (params.isArray() && params.size() > 0) {
        enabled = params[0].asBool();
    } else if (params.isObject() && params.isMember("enabled")) {
        enabled = params["enabled"].asBool();
    }

    din::Json result;
    result["enabled"] = enabled;
    result["message"] = "Compact blocks are always enabled with binary wire format";
    result["rpc_schema"] = "din.rpc.v1";
    return result;
}

// Simulated P2P message handlers for testing
din::Json handleSimulateHeaders(const ExecutionContext& ctx, const din::Json& params) {
    if (!g_headers_sync) {
        din::Json error;
        error["code"] = -1;
        error["message"] = "Headers sync not initialized";
        throw std::runtime_error(din::dump(error));
    }
    
    // Create simulated headers response
    HeadersResponse response;
    
    int count = 10;
    if (params.isArray() && params.size() > 0) {
        count = params[0];
    } else if (params.isObject() && params.isMember("count")) {
        count = params["count"];
    }
    
    // Generate dummy headers
    Consensus consensus;  // Get consensus params
    for (int i = 0; i < count; ++i) {
        BlockHeader header;
        header.version = 1;
        header.prev_block_hash = (i == 0) ?
            "0000000000000000000000000000000000000000000000000000000000000000" :
            response.headers.back().hash;
        header.merkle_root = "1234567890abcdef1234567890abcdef1234567890abcdef1234567890abcdef";
        header.timestamp = 1640995200 + (i * 600); // 10 minutes apart
        header.difficulty = consensus.genesisBits;  // Use consensus param for test simulation
        header.nonce = 12345 + i;
        header.height = i;
        
        // Simple hash generation
        std::stringstream ss;
        ss << std::hex << std::hash<std::string>{}(
            std::to_string(header.version) + header.prev_block_hash + 
            std::to_string(header.timestamp) + std::to_string(header.nonce)
        );
        header.hash = ss.str();
        while (header.hash.length() < 64) {
            header.hash = "0" + header.hash;
        }
        
        response.headers.append(header);
    }
    
    response.more_available = (count >= 10);
    
    // Process the simulated headers
    bool success = g_headers_sync->processHeaders("simulated_peer", response);
    
    din::Json result;
    result["success"] = success;
    result["headers_processed"] = count;
    result["more_available"] = response.more_available;
    result["rpc_schema"] = "din.rpc.v1";
    return result;
}

din::Json handleSimulateCompactBlock(const ExecutionContext& ctx, const din::Json& params) {
    // Plan-A: This simulation handler is deprecated after migration to binary wire format.
    // The JSON-based simulation infrastructure has been removed.
    // For testing compact blocks, use integration tests with actual block relay.
    din::Json result;
    result["success"] = false;
    result["error"] = "Deprecated: JSON compact block simulation removed in Plan-A binary migration";
    result["message"] = "Use integration tests for compact block testing";
    result["rpc_schema"] = "din.rpc.v1";
    return result;
}

} // anonymous namespace

void registerP2pRpcHandlers() {
    extern RpcRegistry* g_rpcRegistry;
    if (!g_rpcRegistry) {
        std::cout << "[RPC] Warning: RPC registry not available for P2P handlers" << std::endl;
        return;
    }
    
    // Headers-first sync methods
    RpcMethodMeta sync_status_meta;
    sync_status_meta.category = "p2p";
    sync_status_meta.description = "Get headers-first sync status";
    sync_status_meta.result_meta.description = "Current sync state and progress";
    g_rpcRegistry->registerHandler("p2p.getsyncstatus", handleGetSyncStatus, sync_status_meta);
    
    RpcMethodMeta start_sync_meta;
    start_sync_meta.category = "p2p";
    start_sync_meta.description = "Start headers-first sync with a peer";
    RpcParamMeta peer_param;
    peer_param.name = "peer_id";
    peer_param.type = "string";
    peer_param.description = "Peer identifier to sync with";
    peer_param.required = false;
    start_sync_meta.params.append(peer_param);
    g_rpcRegistry->registerHandler("p2p.startsync", handleStartSync, start_sync_meta);
    
    RpcMethodMeta stop_sync_meta;
    stop_sync_meta.category = "p2p";
    stop_sync_meta.description = "Stop current headers-first sync";
    g_rpcRegistry->registerHandler("p2p.stopsync", handleStopSync, stop_sync_meta);
    
    RpcMethodMeta sync_metrics_meta;
    sync_metrics_meta.category = "p2p";
    sync_metrics_meta.description = "Get headers-first sync metrics";
    g_rpcRegistry->registerHandler("p2p.getsyncmetrics", handleGetSyncMetrics, sync_metrics_meta);
    
    // Compact blocks methods
    RpcMethodMeta compact_stats_meta;
    compact_stats_meta.category = "p2p";
    compact_stats_meta.description = "Get compact block statistics";
    g_rpcRegistry->registerHandler("p2p.getcompactblockstats", handleGetCompactBlockStats, compact_stats_meta);
    
    RpcMethodMeta compact_status_meta;
    compact_status_meta.category = "p2p";
    compact_status_meta.description = "Get compact block manager status";
    g_rpcRegistry->registerHandler("p2p.getcompactblockstatus", handleGetCompactBlockStatus, compact_status_meta);
    
    RpcMethodMeta set_compact_enabled_meta;
    set_compact_enabled_meta.category = "p2p";
    set_compact_enabled_meta.description = "Enable or disable compact blocks";
    RpcParamMeta enabled_param;
    enabled_param.name = "enabled";
    enabled_param.type = "boolean";
    enabled_param.description = "Whether to enable compact blocks";
    enabled_param.required = false;
    set_compact_enabled_meta.params.append(enabled_param);
    g_rpcRegistry->registerHandler("p2p.setcompactblocksenabled", handleSetCompactBlocksEnabled, set_compact_enabled_meta);
    
    // Testing/simulation methods
    RpcMethodMeta simulate_headers_meta;
    simulate_headers_meta.category = "testing";
    simulate_headers_meta.description = "Simulate receiving headers for testing";
    RpcParamMeta count_param;
    count_param.name = "count";
    count_param.type = "integer";
    count_param.description = "Number of headers to simulate";
    count_param.required = false;
    simulate_headers_meta.params.append(count_param);
    g_rpcRegistry->registerHandler("test.simulateheaders", handleSimulateHeaders, simulate_headers_meta);
    
    RpcMethodMeta simulate_compact_meta;
    simulate_compact_meta.category = "testing";
    simulate_compact_meta.description = "Simulate receiving compact block for testing";
    g_rpcRegistry->registerHandler("test.simulatecompactblock", handleSimulateCompactBlock, simulate_compact_meta);
    
    std::cout << "[RPC] Registered P2P RPC handlers" << std::endl;
}
