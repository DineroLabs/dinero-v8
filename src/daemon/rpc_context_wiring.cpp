// RPC Context Wiring - Week 2 Migration
// Wires DaemonContext to RPC server and registers context-aware handlers

#include "daemon/daemon_context.h"
#include "daemon/http_rpc_server.h"
#include "daemon/services/config_service.h"  // For ConfigService::DataDir()
#include "daemon/services/wallet_service.h"
#include "rpc/rpc_registry.h"
#include "rpc/rpc_init.h"                    // For RegisterAllRPCMethods() (Phase E.3.1 CPU stats)
#include "rpc/methods_mining.h"             // For din::rpc::registerStratumMethodsContext()
#include "rpc/methods_pool.h"               // Pool accounting RPC (feature-gated)
#include "rpc/rpc_dynamic_p2p_handlers.h"   // Task 6: dynamic_p2p.observe handler
#include "rpc/rpc_relay_hints_handlers.h"   // Phase 2b: relay_hints.list handler
#include "rpc/rpc_seeder_handlers.h"        // Dashboard seeder lifecycle handlers
#include "vault/vault_runtime.h"            // Track C: Liquidity Vault runtime owner
// DISABLED: Payroll feature (experimental)
// #include "rpc/payroll_rpc.h"     // For WirePayrollRpcContext()
// #include "database/payroll_db.h" // For PayrollDB
#include "common/logger.h"
#include <iostream>

// Forward declarations of context-aware registration functions
void registerBlockchainMethodsContext();
void registerWalletMethodsContext();
// TODO Phase D: Re-enable when file added to build
// void register_context_wallet_covenant_methods();  // Phase C.4: Covenant construction RPC methods
void registerMiningMethodsContext();  // Phase F.5: MiningManager v2 mining RPC
void registerMiningRPCv14();  // v0.14.0.3: BlockAssembler-based mining RPC
void registerMempoolMethodsContext();
void registerNetworkMethodsContext();
void registerContractMethodsContext();
void registerEconomicsMethodsContext();
void registerPaymentMethodsContext();
void registerSyncMethodsContext();
void registerMarketMethodsContext();
void registerBridgeMethodsContext();
void registerDiscoveryMethodsContext();
void registerAuthMethodsContext();
void registerMultiassetMethodsContext();
#ifdef DINERO_HAVE_HARDWARE_WALLETS
void registerHardwareWalletMethodsContext();
#endif
void registerDescriptorMethodsContext();  // Phase 3C: Descriptor persistence RPC
void registerTelemetryMethodsContext();
void registerConsensusMethodsContext();
void registerWebsocketMethodsContext();
void registerSilentPaymentsMethodsContext();
void WireDiagnosticsRpcContext();  // Node diagnostics (node.info, rpc.methods)
void WireLoggingRpcContext();  // Logging control (logging.setlevel, logging.getlevel)
void WireLogsRpcContext();  // Log aggregation (logs.recent, logs.services, logs.tail)
void registerV7PqWalletMethods();  // Phase 4c.3.1: V7 post-quantum wallet RPCs (getnewp2mraddress, listp2mraddresses, signp2mr)
void registerShieldedWalletMethods();  // V7 shielded pool RPCs (wallet.shield, wallet.unshield, wallet.shieldedbalance)

// DISABLED: Payroll feature (experimental)
// // Forward declaration for payroll RPC wiring
// namespace dinero {
// namespace rpc {
//     void WirePayrollRpcContext(std::shared_ptr<dinero::payroll::PayrollDB> db);  // Private Payroll over Lightning
// }
// }

// Forward declaration for din::rpc namespace
namespace din {
namespace rpc {
    void registerMiningExtrasMethodsVNext();
    void registerEconomicsMethodsVNext();  // Economics RPC methods (VNext DSL: rpc.getcontext, etc.)
    void registerStratumMethodsContext();  // Stratum RPC methods (context-only)
    void registerGPUMiningRPCHandlers();   // GPU mining RPC methods (mining.gpustatus, mining.allowgpu, mining.gpuinfo)
}
}

// Forward declaration for dinero::rpc namespace
namespace dinero {
namespace rpc {
    void register_peer_scoring_methods();  // Phase 5D: Peer scoring & DoS protection
    void register_headers_sync_methods();  // Phase 5A: Headers-first sync
    void register_compact_blocks_methods();  // Phase 5B: Compact block relay
    void register_address_manager_methods();  // Phase 5C: Address manager
    void register_rbf_policy_methods();  // Phase 5E: RBF policy
    void registerWalletSyncMethods();      // Wallet sync RPC (getsyncstatus, getreorginfo, getslowreason)
    void registerOpenRpcMethods();         // OpenRPC schema/introspection RPC
}
}

// Forward declaration for Lightning Network methods (Phase 7)
void register_lightning_methods();  // Phase 7: Lightning Network (ln.*)

// Forward declaration for daemon_id persistence
extern din::Json rpc_context_getcontext(const ExecutionContext& ctx, const din::Json& params);

namespace dinero {

/**
 * Persist daemon_id to <datadir>/.daemon_id
 *
 * Optional Layer 2.5: Enables monitoring/automation tools to read daemon_id
 * without making RPC calls.
 */
void PersistDaemonId(DaemonContext& ctx) {
    try {
        // Build ExecutionContext for RPC call
        ExecutionContext exec_ctx;
        exec_ctx.daemon = &ctx;

        // Call rpc.getcontext to get daemon_id
        din::Json empty_params;
        din::Json result = rpc_context_getcontext(exec_ctx, empty_params);

        // Extract daemon_id
        if (!result.isMember("daemon_id")) {
            dinero::g_logger.warning("[RPC Context] Failed to persist daemon_id: missing daemon_id field");
            return;
        }

        std::string daemon_id = result["daemon_id"].asString();

        // Get datadir from config
        if (!ctx.config) {
            dinero::g_logger.warning("[RPC Context] Failed to persist daemon_id: config service not available");
            return;
        }

        auto config = std::dynamic_pointer_cast<ConfigService>(ctx.config);
        if (!config) {
            dinero::g_logger.warning("[RPC Context] Failed to persist daemon_id: config service cast failed");
            return;
        }

        std::string datadir = config->DataDir();
        std::string daemon_id_path = datadir + "/.daemon_id";

        // Write daemon_id to file
        std::ofstream file(daemon_id_path);
        if (!file.is_open()) {
            dinero::g_logger.warning("[RPC Context] Failed to open daemon_id file: " + daemon_id_path);
            return;
        }

        file << daemon_id << std::endl;
        file.close();

        dinero::g_logger.info("[RPC Context] ✅ Daemon ID persisted: " + daemon_id_path);
        dinero::g_logger.info("[RPC Context] Daemon ID: " + daemon_id);

    } catch (const std::exception& e) {
        dinero::g_logger.warning("[RPC Context] Failed to persist daemon_id: " + std::string(e.what()));
    }
}

/**
 * Wire DaemonContext to the RPC system
 *
 * This function connects the service layer (DaemonContext) to the RPC layer
 * so that context-aware RPC handlers can access services without globals.
 *
 * Call this AFTER all services have been initialized and started.
 */
bool WireRpcContext(DaemonContext& ctx, HttpRpcServer* http_server) {
    if (!http_server) {
        dinero::g_logger.error("[RPC Context] HttpRpcServer is null, cannot wire context");
        return false;
    }

    dinero::g_logger.info("[RPC Context] Wiring DaemonContext to RPC server...");

    // Step 1: Inject DaemonContext into HttpRpcServer
    // This makes ctx available to ExecutionContext in RPC handlers
    http_server->set_daemon_context(&ctx);
    dinero::g_logger.info("[RPC Context] DaemonContext injected into HttpRpcServer");

    // Step 2: Register context-aware handlers
    // These will OVERWRITE legacy handlers with context-aware versions
    dinero::g_logger.info("[RPC Context] Registering context-aware RPC handlers...");

    try {
        // Blockchain namespace (Week 2)
        registerBlockchainMethodsContext();
        dinero::g_logger.info("[RPC Context] ✅ Blockchain context-aware handlers registered");

        // Wallet namespace (Week 2)
        registerWalletMethodsContext();
        dinero::g_logger.info("[RPC Context] ✅ Wallet context-aware handlers registered");

        // V7 post-quantum wallet namespace (Phase 4c.3.1)
        registerV7PqWalletMethods();
        dinero::g_logger.info("[RPC Context] ✅ V7 PQ wallet handlers registered (getnewp2mraddress, listp2mraddresses, signp2mr)");

        // Wallet sync (getsyncstatus, getreorginfo, getslowreason)
        dinero::rpc::registerWalletSyncMethods();
        dinero::g_logger.info("[RPC Context] ✅ Wallet sync handlers registered (reorg, sync status)");

        // v7 shielded pool.
        registerShieldedWalletMethods();
        dinero::g_logger.info("[RPC Context] ✅ V7 shielded pool handlers registered (wallet.shield, wallet.unshield)");

        // TODO Phase D: Re-enable covenant methods when methods_wallet_covenant.cpp is added to build
        // // Wallet covenant (Phase C.4: Covenant construction)
        // register_context_wallet_covenant_methods();
        // dinero::g_logger.info("[RPC Context] ✅ Wallet covenant handlers registered (CTV/CSFS construction)");

        // Phase F.5: Mining methods now use MiningManager v2 and jsoncpp API
        // Mining extras (getblocktemplate, generatetoaddress) - REGISTER FIRST so real implementation wins
        din::rpc::registerMiningExtrasMethodsVNext();
        dinero::g_logger.info("[RPC Context] ✅ Mining extras handlers registered");

        // Mining namespace (Week 2 - Day 1 parallel) - stub with IfAbsent won't overwrite
        registerMiningMethodsContext();
        dinero::g_logger.info("[RPC Context] ✅ Mining context-aware handlers registered");

        // v0.14.0.3: BlockAssembler-based mining RPC
        registerMiningRPCv14();
        dinero::g_logger.info("[RPC Context] ✅ Mining RPC v14 handlers registered (BlockAssembler)");

        // Stratum mining server methods (context-only, no MiningState)
        din::rpc::registerStratumMethodsContext();
        dinero::g_logger.info("[RPC Context] ✅ Stratum handlers registered");

        // GPU mining methods (mining.gpustatus, mining.allowgpu, mining.gpuinfo)
        din::rpc::registerGPUMiningRPCHandlers();
        dinero::g_logger.info("[RPC Context] ✅ GPU mining handlers registered");

        // Mempool namespace (Week 2 - Day 2)
        registerMempoolMethodsContext();
        dinero::g_logger.info("[RPC Context] ✅ Mempool context-aware handlers registered");

        // Pool accounting namespace (feature-gated; see pool.status)
        din::rpc::registerPoolMethods();
        dinero::g_logger.info("[RPC Context] ✅ Pool accounting handlers registered");

        // Network namespace (Week 2 - Day 1 parallel continued)
        registerNetworkMethodsContext();
        dinero::g_logger.info("[RPC Context] ✅ Network context-aware handlers registered");

        // Phase 5: Network & Protocol Hardening (November 11, 2025)

        // Peer Scoring namespace (Phase 5D)
        dinero::rpc::register_peer_scoring_methods();
        dinero::g_logger.info("[RPC Context] ✅ Peer scoring handlers registered (DoS protection)");

        // Headers-first sync namespace (Phase 5A)
        dinero::rpc::register_headers_sync_methods();
        dinero::g_logger.info("[RPC Context] ✅ Headers-first sync handlers registered (sync.*)");

        // Compact block relay namespace (Phase 5B)
        dinero::rpc::register_compact_blocks_methods();
        dinero::g_logger.info("[RPC Context] ✅ Compact block relay handlers registered (compactblocks.*)");

        // Address manager namespace (Phase 5C)
        dinero::rpc::register_address_manager_methods();
        dinero::g_logger.info("[RPC Context] ✅ Address manager handlers registered (addrman.*)");

        // RBF policy namespace (Phase 5E)
        dinero::rpc::register_rbf_policy_methods();
        dinero::g_logger.info("[RPC Context] ✅ RBF policy handlers registered (rbf.*)");

        // ⚡ Lightning Network namespace (Phase 7: November 11, 2025)
        register_lightning_methods();
        dinero::g_logger.info("[RPC Context] ✅ Lightning Network handlers registered (ln.*)");

        // Contract namespace (Week 2 - Day 2)
        // INTENTIONALLY DISABLED: Contract handlers exist in methods_contract_context.cpp
        // but are disabled pending full integration testing.
        // To enable: uncomment registerContractMethodsContext() below.
        // registerContractMethodsContext();
        dinero::g_logger.info("[RPC Context] ⚠️  Contract handlers DISABLED (intentional)");

        // Economics namespace (Week 2 - Day 2)
        registerEconomicsMethodsContext();
        dinero::g_logger.info("[RPC Context] ✅ Economics context-aware handlers registered");

        // Economics VNext (RPC_METHOD DSL with full metadata)
        din::rpc::registerEconomicsMethodsVNext();
        dinero::g_logger.info("[RPC Context] ✅ Economics VNext handlers registered (DSL)");

        // Sync namespace (Week 2 - Quick wins)
        registerSyncMethodsContext();
        dinero::g_logger.info("[RPC Context] ✅ Sync context-aware handlers registered");

        // Payment namespace (Week 2 - Day 3)
        registerPaymentMethodsContext();
        dinero::g_logger.info("[RPC Context] ✅ Payment context-aware handlers registered");

        // Market namespace (Week 2 - Day 2)
        registerMarketMethodsContext();
        dinero::g_logger.info("[RPC Context] ✅ Market context-aware handlers registered");

        // Bridge namespace (Week 2 - Day 3) - DISABLED: Incomplete implementation
        //         // registerBridgeMethodsContext();
        dinero::g_logger.info("[RPC Context] ⚠️  Bridge handlers DISABLED (experimental)");

        // Discovery namespace (Week 2 - Day 3)
        registerDiscoveryMethodsContext();
        dinero::g_logger.info("[RPC Context] ✅ Discovery context-aware handlers registered");

        // OpenRPC schema/introspection namespace
        dinero::rpc::registerOpenRpcMethods();
        dinero::g_logger.info("[RPC Context] ✅ OpenRPC handlers registered");

        // Auth namespace (Week 2 - Day 3)
        registerAuthMethodsContext();
        dinero::g_logger.info("[RPC Context] ✅ Auth context-aware handlers registered");

        // Multiasset namespace (Week 2 - Day 3) - DISABLED: Incomplete implementation
        // registerMultiassetMethodsContext();
        dinero::g_logger.info("[RPC Context] ✅ Multiasset context-aware handlers registered");

        // Hardware Wallet namespace (Week 2 - Final push)
#ifdef DINERO_HAVE_HARDWARE_WALLETS
        registerHardwareWalletMethodsContext();
        dinero::g_logger.info("[RPC Context] ✅ Hardware wallet context-aware handlers registered");
#else
        dinero::g_logger.info("[RPC Context] ⏭ Hardware wallet handlers skipped (built without ENABLE_HARDWARE_WALLETS)");
#endif

        // Descriptor namespace (Phase 3C: Descriptor persistence)
        registerDescriptorMethodsContext();
        dinero::g_logger.info("[RPC Context] ✅ Descriptor context-aware handlers registered");

        // Telemetry namespace (Week 2 - Final push)
        registerTelemetryMethodsContext();

        // Consensus namespace (Week 2 - Final push)
        registerConsensusMethodsContext();

        // Websocket namespace (Week 2 - Final push)
        registerWebsocketMethodsContext();

        // Silent Payments namespace (Week 2 - Final push)
        registerSilentPaymentsMethodsContext();

        // Diagnostics namespace (node.info, rpc.methods) - November 8, 2025
        WireDiagnosticsRpcContext();
        dinero::g_logger.info("[RPC Context] ✅ Diagnostics context-aware handlers registered");

        // Task 6: Dynamic P2P observe — live DPP mode/governor/peer-quality snapshot.
        // ctx.p2p is already the fully-wired P2PService; pass as raw ptr (handler owns nothing).
        // Returns {enabled:false, mode:"off", ...} gracefully when DPP is off or p2p is null.
        http_server->register_method("dynamic_p2p.observe",
            [&ctx](const Json::Value& /*params*/) -> Json::Value {
                return dinero::rpc::HandleDynamicP2PObserve(ctx.p2p.get());
            });
        dinero::g_logger.info("[RPC Context] ✅ dynamic_p2p.observe handler registered");

        // Phase 2b: relay_hints.list — contents of the relay-hint cache as a
        // structured list. Dashboard polls this every 5s to render the
        // DiscoverySection freshness bar + stoplight glyph.
        // Returns empty targets array when cache is empty; never errors.
        http_server->register_method("relay_hints.list",
            [&ctx](const Json::Value& /*params*/) -> Json::Value {
                return dinero::rpc::HandleRelayHintsList(ctx.p2p.get());
            });
        dinero::g_logger.info("[RPC Context] ✅ relay_hints.list handler registered");

        // Phase 3: relayhints.dial — dashboard/operator action to submit a
        // RELAY_CONNECT through a cached hint. The handler refuses arbitrary
        // relay endpoints; relay_endpoint must match relay_hints.list output.
        http_server->register_method("relayhints.dial",
            [&ctx](const Json::Value& params) -> Json::Value {
                return dinero::rpc::HandleRelayHintsDial(ctx.p2p.get(), params);
            });
        dinero::g_logger.info("[RPC Context] ✅ relayhints.dial handler registered");

        // Dashboard seeder lifecycle. These RPCs are intentionally explicit:
        // the daemon never starts dinero-seeder by default; Qt must pass the
        // bundled binary path after the user opts in and presses Start Seeder.
        http_server->register_method("seeder.status",
            [&ctx](const Json::Value& /*params*/) -> Json::Value {
                return dinero::rpc::HandleSeederStatus(
                    dynamic_cast<dinero::ConfigService*>(ctx.config.get()));
            });
        http_server->register_method("seeder.start",
            [&ctx](const Json::Value& params) -> Json::Value {
                return dinero::rpc::HandleSeederStart(
                    dynamic_cast<dinero::ConfigService*>(ctx.config.get()),
                    params);
            });
        http_server->register_method("seeder.stop",
            [&ctx](const Json::Value& /*params*/) -> Json::Value {
                return dinero::rpc::HandleSeederStop(
                    dynamic_cast<dinero::ConfigService*>(ctx.config.get()));
            });
        dinero::g_logger.info("[RPC Context] ✅ seeder lifecycle handlers registered");

        // Phase E.3.1: CPU stats & resource monitoring (node.getcpustats, node.getresourcepressure, node.getdiskstats)
        RegisterAllRPCMethods(ctx);
        dinero::g_logger.info("[RPC Context] ✅ CPU stats & resource monitoring handlers registered (Phase E.3.1)");

        // Track C: Liquidity Vault runtime. Disabled by default —
        // operator flips `enabled = true` in this block once they're
        // ready to start observing deposits. Stage 0 shadow rollouts
        // set `shadow_mode = true` so deposit_observed entries flow
        // but no credits open. Chain-query closures point at
        // chainstate-backed lookups; the include-by-default policy
        // for tx_included_at avoids false reorg reports under
        // transient block-fetch failures.
        {
            dinero::vault::VaultRuntimeConfig vault_cfg;
            // Operator gates. Defaults stay safe (disabled). Flip with
            //   -vault=1            in dinero.conf or on the cli to start
            //                       the runtime.
            //   -vault.shadow=0     to leave shadow mode and open real
            //                       credits (only after Stage 0 / 1).
            //   -vault.ledgerpath=  override the JSON-line persistence
            //                       location (default <datadir>/vault/
            //                       ledger.jsonl).
            // Vault on by default. Operators who don't want it pass
            // `-vault=0` (e.g. headless seed nodes that have no
            // custodial role). Shadow mode is now off by default
            // (v2.1.29): the deposit flow opens real credits as soon
            // as a deposit reaches k_credit confirmations.
            vault_cfg.enabled = ctx.config->GetBool("vault", true);
            vault_cfg.shadow_mode = ctx.config->GetBool("vault.shadow", false);
            vault_cfg.persistence_path = ctx.config->GetString(
                "vault.ledgerpath", ctx.config->DataDir() + "/vault/ledger.jsonl");
            // Track C, C.8: operator address ↔ default account, plus
            // K-confirmation policy. Auto-observer no-ops if the
            // address is empty. Defaults are conservative for first
            // real-funds runs (k_credit=10, k_settle=20).
            vault_cfg.operator_address = ctx.config->GetString("vault.address", "");
            vault_cfg.default_account = ctx.config->GetString("vault.account", "default");
            vault_cfg.k_observe = static_cast<uint64_t>(ctx.config->GetInt("vault.k_observe", 1));
            vault_cfg.k_credit = static_cast<uint64_t>(ctx.config->GetInt("vault.k_credit", 10));
            vault_cfg.k_settle = static_cast<uint64_t>(ctx.config->GetInt("vault.k_settle", 20));
            // Real chainstate-backed closures (Track C, C.6).
            // ChainDB::getBlockHashByHeight gives the canonical
            // active-chain hash; ChainDB::getBlock + tx walk gives
            // tx inclusion at a specific block hash. Both are
            // conservative on failure (zero-array → UNKNOWN for the
            // hash query; true → RE_MINED_SAME_TXID for the inclusion
            // query) so transient lookup hiccups never trigger a
            // false compensating-debit cascade.
            vault_cfg.block_hash_at_height = dinero::vault::MakeChainstateBlockHashClosure(ctx);
            vault_cfg.tx_included_at = dinero::vault::MakeChainstateTxIncludedClosure(ctx);
            dinero::vault::InitializeVaultRuntime(std::move(vault_cfg));
            dinero::g_logger.info("[RPC Context] ✅ Liquidity Vault runtime initialised (Track C)");
            if (ctx.wallet) {
                if (auto wallet_service =
                        std::dynamic_pointer_cast<dinero::WalletService>(ctx.wallet)) {
                    wallet_service->EnsureRuntimeWalletBindings();
                }
            }
        }

        // Logging control namespace (logging.setlevel, logging.getlevel) - Step C: Dynamic runtime control
        WireLoggingRpcContext();
        dinero::g_logger.info("[RPC Context] ✅ Logging control context-aware handlers registered");

        // Log aggregation namespace (logs.recent, logs.services, logs.tail) - Unified log aggregator
        WireLogsRpcContext();
        dinero::g_logger.info("[RPC Context] ✅ Log aggregation context-aware handlers registered");

        // DISABLED: Payroll feature (experimental Lightning-based payroll)
        // // Private Payroll over Lightning namespace
        // // Initialize PayrollDB with datadir from ConfigService
        // std::string payroll_db_path = ctx.config->DataDir() + "/payroll.db";
        // auto payroll_db = std::make_shared<dinero::payroll::PayrollDB>(payroll_db_path);
        // if (payroll_db->Open()) {
        //     dinero::rpc::WirePayrollRpcContext(payroll_db);
        //     dinero::g_logger.info("[RPC Context] ✅ Payroll context-aware handlers registered");
        // } else {
        //     dinero::g_logger.warning("[RPC Context] ⚠️  Failed to open PayrollDB, skipping payroll RPC registration");
        // }

    } catch (const std::exception& e) {
        dinero::g_logger.error("[RPC Context] Failed to register context-aware handlers: " +
                              std::string(e.what()));
        return false;
    }

    dinero::g_logger.info("[RPC Context] ✅ Context wiring complete");
    dinero::g_logger.info("[RPC Context] RPC handlers can now access services via context");

    return true;
}

} // namespace dinero
