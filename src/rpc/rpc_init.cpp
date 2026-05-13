#include "rpc/rpc_init.h"
#include "daemon/daemon_context.h"
#include "common/logger.h"
#include "rpc/cpu_stats_rpc.h"  // Phase E.3.1: CPU stats RPC
#include "rpc/mining_control_rpc.h"  // Phase Y: CPU miner control
#include "rpc/methods_utreexo.h"  // Phase 11a.1: Utreexo RPC methods
#include "rpc/methods_vault.h"  // Track C: Liquidity Vault RPC
#include "rpc/ct_fee_rpc.h"  // Phase 3: CT fee configuration RPC

// Phase 26.3: Mining template RPC registration
extern void registerMiningTemplateRPC();

// v0.14.0.3: Mining RPC v14 (BlockAssembler-based)
extern void registerMiningRPCv14();

// Phase 29B: Master RPC registration function
// This is called EXACTLY ONCE during daemon initialization
// to register all RPC methods in a centralized, controlled manner.
// Uses the global g_rpcRegistry declared in rpc_registry.h
//
// This matches Bitcoin Core's RegisterAllCoreRPCCommands() architecture.

void RegisterAllRPCMethods(DaemonContext& ctx) {
    dinero::g_logger.info("Phase 29B: Registering all RPC methods (centralized)...");

    // Register each subsystem's RPC methods exactly once
    RegisterBlockchainRPC(ctx);
    RegisterMiningRPC(ctx);
    RegisterWalletRPC(ctx);
    RegisterP2PRPC(ctx);
    RegisterNetworkRPC(ctx);
    RegisterDiagnosticsRPC(ctx);  // Phase E.3.1: CPU stats & resource monitoring
    RegisterDpiRPC(ctx);          // DPI: Dinero Payment Intent protocol

    dinero::g_logger.info("Phase 29B: RPC registration complete");
}

// Subsystem-specific registration functions
// Each function registers RPC methods for ONE subsystem only.
// These call g_rpcRegistry.registerHandler() directly.
//
// Step 4 migration note: move registrations from WireRpcContext() into these functions.

void RegisterBlockchainRPC(DaemonContext& ctx) {
    dinero::g_logger.info("  Registering blockchain RPC methods...");

    // Phase 11a.1: Utreexo RPC methods (core + batch)
    RegisterUtreexoRPC();

    // Phase 34.3: Utreexo Bridge API (utreexo.getproof, utreexo.getroots, utreexo.getstate)
    // Registers aliases for light clients (CSN) to fetch proofs from full nodes
    RegisterUtreexoBridgeRPC();

    // Track C: Liquidity Vault RPC (vault.account.spendable, vault.withdraw,
    // vault.withdrawal.status, vault.metrics, vault.observe). The vault
    // service itself is initialised separately in daemon main once the
    // chainstate + signing backend are wired up.
    RegisterVaultRPC();

    // Step 4 migration: move remaining blockchain RPC registration here from WireRpcContext().
    // Methods: getblockchaininfo, getblock, getblockhash, getbestblockhash,
    //          invalidateblock, reconsiderblock, etc.
}

void RegisterMiningRPC(DaemonContext& ctx) {
    dinero::g_logger.info("  Registering mining RPC methods...");

    // Phase Y: CPU miner control (start/stop/getstatus/setthreads/setaddress)
    extern RpcRegistry g_rpcRegistry;
    register_mining_control_rpc_methods(g_rpcRegistry);

    // Phase 3: CT fee configuration (ct.setminfee, ct.setweightmultiplier, etc.)
    register_ct_fee_rpc_methods(g_rpcRegistry);

    // Phase 26.3: Block template methods (getblocktemplate, submitblock)
    // registerMiningTemplateRPC() is still registered in WireRpcContext() in this build.
    // registerMiningTemplateRPC();
    // v0.14.0.3: BlockAssembler-based mining RPC (replaces Phase 26 for production)
    // registerMiningRPCv14() is still registered in WireRpcContext() in this build.
    // registerMiningRPCv14();
    // Step 4 migration: move remaining mining RPC registration here.
    // Methods: getmininginfo, etc.
}

void RegisterWalletRPC(DaemonContext& ctx) {
    dinero::g_logger.info("  Registering wallet RPC methods...");
    // Step 4 migration: move wallet RPC registration here from WireRpcContext().
    // Methods: wallet.createhd, wallet.getnewaddress, wallet.getbalance,
    //          wallet.sendtoaddress, etc.
}

void RegisterP2PRPC(DaemonContext& ctx) {
    dinero::g_logger.info("  Registering P2P RPC methods...");
    // Step 4 migration: move P2P RPC registration here from WireRpcContext().
    // Methods: addnode, getpeerinfo, etc.
    // NOTE: validateblock/invalidateblock moved to blockchain (not P2P)
}

void RegisterNetworkRPC(DaemonContext& ctx) {
    dinero::g_logger.info("  Registering network RPC methods...");
    // Step 4 migration: move network RPC registration here from WireRpcContext().
    // Methods: getnetworkinfo, ping, etc.
}

void RegisterDiagnosticsRPC(DaemonContext& ctx) {
    dinero::g_logger.info("  Registering diagnostics RPC methods...");
    // Phase E.3.1: CPU stats & resource monitoring (read-only observability)
    extern RpcRegistry g_rpcRegistry;
    register_cpu_stats_rpc_methods(g_rpcRegistry);

    // Daemon readiness contract (getdaemonstatus)
    extern void register_daemon_status_rpc_methods(RpcRegistry&);
    register_daemon_status_rpc_methods(g_rpcRegistry);
}
