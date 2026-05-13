#pragma once

#include "din_json.h"
#include "rpc/rpc_registry.h"

// Forward declarations for Utreexo RPC methods
namespace din {

// Phase 34.2: Core Utreexo RPC methods
Json rpc_getutreexoroots(const ExecutionContext& ctx, const Json& params);
Json rpc_getutreexocommitment(const ExecutionContext& ctx, const Json& params);
Json rpc_getutxoproof(const ExecutionContext& ctx, const Json& params);
Json rpc_getutreexostats(const ExecutionContext& ctx, const Json& params);
Json rpc_getutreexocachestats(const ExecutionContext& ctx, const Json& params);
Json rpc_getutreexogossipstats(const ExecutionContext& ctx, const Json& params);
Json rpc_rebuildutreexo(const ExecutionContext& ctx, const Json& params);

// Phase 11a.1: Batch Utreexo RPC methods
Json rpc_getutxoproofs_batch(const ExecutionContext& ctx, const Json& params);
Json rpc_verifyutxoproofs_batch(const ExecutionContext& ctx, const Json& params);

// Proof lifecycle: re-prove outpoints at current tip
Json rpc_getproofupdates(const ExecutionContext& ctx, const Json& params);

} // namespace din

// Registration functions
void RegisterUtreexoRPC();
void RegisterUtreexoBridgeRPC();  // Phase 34.3: Bridge API for light clients
