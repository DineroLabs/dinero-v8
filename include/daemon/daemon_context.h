#pragma once
#include <functional>
#include <memory>
#include "consensus/cpu_budget_monitor.h"
#include "storage/disk_space_monitor.h"
#include "p2p/network_limits_monitor.h"

// Forward declarations
class Subscriptions;  // WebSocket subscription manager

namespace dinero {

// Step 5: Interface forward declarations (canonical ingress APIs)
struct ITxIngress;           // Transaction submission interface
struct IBlockIngress;        // Block submission interface
struct IBlockTemplateSource; // Mining transaction source interface
// Phase 39: ChainManager DELETED (forward declaration removed)

// Logger interface for dependency injection
class ILogger;

// Unified log aggregator
class LoggerRouter;

// WebSocket server
class WebSocketServer;

// Core service wrappers
class LoggerService;
class ConfigService;
class ChainstateService;
class MempoolService;
class WalletService;
class P2PService;           // Changed from P2PManager
class RPCService;           // Changed from RPCServer
class MiningService;        // Changed from MiningCoordinator
class MetricsService;       // Changed from Metrics
// Phase 5: Network & Protocol Hardening
namespace daemon {
class PeerScoringService;     // Phase 5D: Peer reputation and ban management
class HeadersSyncService;     // Phase 5A: Headers-first sync
class CompactBlockService;    // Phase 5B: Compact block relay (BIP152)
class AddressManagerService;  // Phase 5C: Address manager and peer selection
class RBFPolicyService;       // Phase 5E: Replace-By-Fee policy
class PruneService;           // Phase 34.8: Block pruning for mobile nodes
}

// Phase 6: Performance & Scalability (November 11, 2025)
namespace consensus {
class ValidationQueue;        // Phase 6B: Parallel block validation pipeline
class ChainstateGuard;        // Phase 6B: Thread-safe UTXO access control
class ParallelBlockValidator; // Phase 6B: Multi-threaded validator wrapper
}

// Phase 2: Consensus engine interface
class IConsensusEngine;

// Stratum V1 mining server - REMOVED (separate binary: dinero-stratum)

// Phase C: Block template assembly (for mining)
class BlockAssembler;

// Transaction orphan pool
class TxOrphanPool;

// Optional services
namespace rpc {
class EventBus;
}

namespace bridge {
class FiatBridgeManager;
}

namespace p2p {
class EscrowManager;
}

// Phase 7: Lightning Network (November 11, 2025)
namespace lightning {
class LightningService;
}

// Phase N: Headers-first sync and block download
namespace consensus {
class HeaderSyncP2P;
class BlockDownloadScheduler;
class HeaderChainSelector;
class HeaderStore;
// Phase E.2.d / E.3.1: CPU budget monitoring (now included from header)
}

// Phase G.2: Block propagation
class BlockRelayManager;

// Phase G: Parallel block download scheduler
class BlockDownloadScheduler;

// Block storage (flat file storage for blocks)
class BlockStorage;

// Phase G.3: Mempool relay
class TxRelayManager;
}

// MarketplaceManager is in din namespace (not dinero::p2p)
namespace din {
class MarketplaceManager;
}

/**
 * DaemonContext - Central dependency injection container
 *
 * All major services are stored here instead of as globals/singletons.
 * This solves:
 * - Static initialization order fiasco (no more mutex crashes)
 * - Testability (can inject mocks)
 * - Clear dependency graph
 * - Deterministic lifetime management
 *
 * Services are initialized in order, started in order, and stopped
 * in reverse order by DaemonApp.
 *
 * SINGLETON ACCESS:
 * Use DaemonContext::instance() to access the global instance.
 * This is set by main() during daemon initialization.
 */
struct DaemonContext {
    /**
     * Get the global DaemonContext instance
     * @return Pointer to singleton instance, or nullptr if not initialized
     */
    static DaemonContext* instance();

    /**
     * Set the global DaemonContext instance (called once by main())
     * @param ctx Pointer to the context to install as global
     */
    static void setInstance(DaemonContext* ctx);

    // Process-level shutdown hook installed by main().
    // RPC "stop" and similar control paths should request shutdown through
    // this callback instead of reaching into process globals directly.
    std::function<void()> request_shutdown;

    // Core services (required) - all using service wrappers now
    std::shared_ptr<dinero::LoggerService> logger;

    // Logger interface for dependency injection
    // This allows new code to use ILogger* while existing code uses LoggerService
    // Typically initialized to &ProductionLogger::instance()
    dinero::ILogger* logger_interface = nullptr;

    // Per-service logger routing (Step B: Separate log files per service)
    // Each service gets its own dedicated logger for fine-grained control
    // Example: wallet.log, p2p.log, mining.log, mempool.log
    dinero::ILogger* wallet_logger = nullptr;
    dinero::ILogger* p2p_logger = nullptr;
    dinero::ILogger* mining_logger = nullptr;
    dinero::ILogger* mempool_logger = nullptr;

    // Unified log aggregator (Optional: Real-time log streaming and aggregation)
    dinero::LoggerRouter* logger_router = nullptr;

    std::shared_ptr<dinero::ConfigService> config;

    // Phase 39: ChainManager DELETED
    // ChainDB is now owned by ChainstateService (access via chainstate->GetChainDB())

    std::shared_ptr<dinero::ChainstateService> chainstate;
    std::shared_ptr<dinero::MempoolService> mempool;
    std::shared_ptr<dinero::WalletService> wallet;

    // ========================================================================
    // Step 5: Canonical Ingress Interfaces (non-owning pointers)
    // ========================================================================
    // These are the ONLY ways external components should submit transactions
    // and blocks. Raw pointers because lifetime is daemon-static (no ownership
    // semantics needed - services outlive all consumers).
    //
    // Consumers: RPC, gRPC, P2P handlers, wallet, mining
    // Implementors: MempoolService (ITxIngress), BlockIngressService (IBlockIngress)
    //
    // LIFETIME INVARIANTS:
    //   - These pointers are valid for the entire daemon lifetime
    //   - Implementations are owned by concrete services (shared_ptr in services_)
    //   - Services are constructed at startup and destroyed at shutdown
    //   - Wiring happens once in DaemonApp::InitializeServices()
    //
    // IMPORTANT:
    //   If services ever become dynamically reloadable,
    //   these pointers must be upgraded to managed ownership.
    // ========================================================================
    dinero::ITxIngress* tx_ingress = nullptr;           // Transaction submission
    dinero::IBlockIngress* block_ingress = nullptr;     // Block submission
    dinero::IBlockTemplateSource* block_template_source = nullptr;  // Mining tx selection
    dinero::TxOrphanPool* orphan_pool = nullptr;                    // TX orphan pool (non-owning)
    std::shared_ptr<dinero::P2PService> p2p;              // Updated to service wrapper
    std::shared_ptr<dinero::RPCService> rpc;              // Updated to service wrapper
    std::shared_ptr<dinero::MiningService> mining;  // Updated to service wrapper
    std::shared_ptr<dinero::MetricsService> metrics;      // Updated to service wrapper

    // 🛡️ Phase 5: Network & Protocol Hardening (November 11, 2025)
    // Phase 5D: DoS protection - Replaces g_peer_scoring global
    std::shared_ptr<dinero::daemon::PeerScoringService> peer_scoring;

    // Phase 5A: Headers-first sync - Replaces g_headers_sync global
    std::shared_ptr<dinero::daemon::HeadersSyncService> headers_sync;

    // Phase 5B: Compact block relay (BIP152) - Replaces g_compact_blocks global
    std::shared_ptr<dinero::daemon::CompactBlockService> compact_blocks;

    // Phase 5C: Address manager and peer selection - Replaces g_addrman global
    std::shared_ptr<dinero::daemon::AddressManagerService> address_manager;

    // Phase 5E: RBF (Replace-By-Fee) policy
    std::shared_ptr<dinero::daemon::RBFPolicyService> rbf_policy;

    // 🚀 Phase 6B: Parallel Validation & Pipelining (November 11, 2025)
    // Replaces: g_chainstate_guard, g_validation_queue globals

    // Thread-safe UTXO access control (readers-writer lock)
    std::shared_ptr<dinero::consensus::ChainstateGuard> chainstate_guard;

    // Parallel block validation pipeline (3-5× faster IBD)
    std::shared_ptr<dinero::consensus::ValidationQueue> validation_queue;

    // Multi-threaded block validator (drop-in replacement for BlockValidator)
    std::shared_ptr<dinero::consensus::ParallelBlockValidator> parallel_validator;

    // 🛡️ Phase E.2.d / E.3.1: CPU Budget Monitoring (Production Hardening)
    // Tracks validation CPU usage and enforces timeouts to prevent DoS
    std::unique_ptr<dinero::consensus::CPUBudgetMonitor> cpu_monitor;

    // 🛡️ Phase E.2.b: Disk Space Monitoring (Production Hardening)
    // Tracks disk space usage and enforces storage limits
    std::unique_ptr<dinero::storage::DiskSpaceMonitor> disk_monitor;

    // 🧹 Phase 34.8: Pruning Service (Mobile-Friendly Storage)
    // Manages block pruning for space-constrained nodes
    std::shared_ptr<dinero::daemon::PruneService> prune;

    // 🛡️ Phase E.2.c: Network Limits Monitoring (Production Hardening)
    // Aggregates connection, rate limiting, and peer scoring health
    std::unique_ptr<dinero::p2p::NetworkLimitsMonitor> network_monitor;

    // Phase 2: Consensus engine (modular consensus layer)
    std::shared_ptr<dinero::IConsensusEngine> consensus;

    // Phase C: Block template assembly (for mining)
    // BlockAssembler belongs to consensus layer (creates valid block templates)
    // Injected into MiningManager (mining consumes templates, doesn't create them)
    std::unique_ptr<dinero::BlockAssembler> block_assembler;

    // ⚡ Phase 7: Lightning Network (November 11, 2025)
    // Lightning service manages channels, HTLCs, routing, and payments
    std::shared_ptr<dinero::lightning::LightningService> lightning;

    // 🔗 Phase N: Headers-First Blockchain Synchronization
    // Phase N.3: Header sync P2P integration
    std::shared_ptr<dinero::consensus::HeaderChainSelector> header_chain;
    std::shared_ptr<dinero::consensus::HeaderStore> header_store;
    std::shared_ptr<dinero::consensus::HeaderSyncP2P> header_sync;

    // Phase N.4: Block download scheduler
    std::shared_ptr<dinero::consensus::BlockDownloadScheduler> block_download;

    // Phase G: Parallel block download scheduler (10-20× IBD speedup)
    std::shared_ptr<dinero::BlockDownloadScheduler> parallel_block_download;

    // Phase N.4: Block storage (flat file storage for downloaded blocks)
    std::shared_ptr<dinero::BlockStorage> block_storage;

    // Phase G.2: Block propagation (minimal relay - no headers-first)
    std::shared_ptr<dinero::BlockRelayManager> block_relay;

    // Phase G.3: Mempool relay (minimal transaction propagation)
    std::shared_ptr<dinero::TxRelayManager> tx_relay;

    // Optional services (may be nullptr)
    std::shared_ptr<dinero::rpc::EventBus> event_bus;
    std::shared_ptr<dinero::bridge::FiatBridgeManager> fiat_bridge;
    std::shared_ptr<din::MarketplaceManager> marketplace;  // Note: din namespace
    std::shared_ptr<dinero::p2p::EscrowManager> escrow;

    // Stratum V1 mining server - REMOVED (separate binary: dinero-stratum)

    // WebSocket server (for streaming RPC operations)
    // Raw pointer since WebSocketServer is managed outside DaemonContext lifecycle
    dinero::WebSocketServer* websocket_server = nullptr;

    // Phase 3F: WebSocket subscription manager (replaces g_subscriptions global)
    // Raw pointer since Subscriptions is managed outside DaemonContext lifecycle
    Subscriptions* websocket_subscriptions = nullptr;

    // ═══════════════════════════════════════════════════════════════════
    // Phase F.2: Mining Restart State (E.3 contract enforcement)
    // ═══════════════════════════════════════════════════════════════════
    //
    // Restart semantics (loaded at daemon startup, persisted at shutdown):
    // - is_fresh_start: true immediately after daemon restart
    // - mining_was_active_before: persisted flag (was mining active before shutdown?)
    //
    // Contract: CONFIG persists, STATE does not
    // - Mining address persists (config)
    // - Mining enabled state does NOT persist (always false after restart)
    // - mining_was_active_before persists (for policy check in E.3)
    // ═══════════════════════════════════════════════════════════════════

    bool is_fresh_start = false;              // True immediately after daemon restart
    bool mining_was_active_before = false;    // Loaded from persistence at startup

    // ═══════════════════════════════════════════════════════════════════
    // Phase 5: Socket Wallet Server Configuration
    // ═══════════════════════════════════════════════════════════════════
    // Configurable via:
    //   - CLI flag: --wallet-socket-port=<port>
    //   - Env var: DINERO_WALLET_SOCKET_PORT
    // Default: 50051
    uint16_t wallet_socket_port = 50051;
};
