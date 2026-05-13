/**
 * Phase 11a.1: Utreexo RPC Registration
 *
 * Centralized registration for all Utreexo RPC methods.
 */

#include "rpc/methods_utreexo.h"
#include "rpc/rpc_registry.h"
#include "common/logger.h"

extern RpcRegistry g_rpcRegistry;

void RegisterUtreexoRPC() {
    dinero::g_logger.info("  Registering Utreexo RPC methods...");

    // Phase 34.2: Core Utreexo RPC methods
    g_rpcRegistry.registerHandler("blockchain.getutreexoroots", din::rpc_getutreexoroots);
    g_rpcRegistry.registerHandler("blockchain.getutreexocommitment", din::rpc_getutreexocommitment);
    g_rpcRegistry.registerHandler("blockchain.getutxoproof", din::rpc_getutxoproof);
    g_rpcRegistry.registerHandler("blockchain.getutreexostats", din::rpc_getutreexostats);
    g_rpcRegistry.registerHandler("blockchain.getutreexocachestats", din::rpc_getutreexocachestats);
    g_rpcRegistry.registerHandler("blockchain.getutreexogossipstats", din::rpc_getutreexogossipstats);
    g_rpcRegistry.registerHandler("blockchain.rebuildutreexo", din::rpc_rebuildutreexo);

    // Phase 11a.1: Batch Utreexo RPC methods
    g_rpcRegistry.registerHandler("blockchain.getutxoproofs_batch", din::rpc_getutxoproofs_batch);
    g_rpcRegistry.registerHandler("blockchain.verifyutxoproofs_batch", din::rpc_verifyutxoproofs_batch);

    // Proof lifecycle: re-prove outpoints at current tip
    g_rpcRegistry.registerHandler("blockchain.getproofupdates", din::rpc_getproofupdates);

    dinero::g_logger.info("  Registered 10 Utreexo RPC methods (7 core + 2 batch + 1 lifecycle)");
}
