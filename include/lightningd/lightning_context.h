#pragma once

#include <memory>
#include <string>
#include <vector>
#include <cstdint>

// Forward declarations - gRPC clients
namespace dinero {
namespace grpc_client {
    class BlockchainClient;
    class MempoolClient;
    class EventSubscriber;
}
namespace lightning {
    class WalletClient;  // Phase 2: Wallet service client
}
}

// Forward declarations - Lightning components
namespace dinero {
namespace lightning {
    class ChannelManager;
    class HTLCManager;
    class PaymentRouter;
    class CommitmentBuilder;
    class WatchtowerClient;
    class LightningSweepManager;
    class PeerManager;
    class GossipManager;
    class ILightningDB;
    class LightningEventManager;
}
}

// Forward declarations - Configuration
namespace dinero {
    class ConfigService;
    class ILogger;
}

namespace lightningd {

/**
 * LightningContext - Dependency injection container for lightningd
 *
 * This replaces DaemonContext for the standalone Lightning daemon.
 * Instead of direct ChainDB/Mempool access, Lightning components
 * use gRPC clients to communicate with dinerod.
 *
 * Key Differences from DaemonContext:
 * - Uses BlockchainClient instead of ChainDB
 * - Uses MempoolClient instead of Mempool
 * - Uses EventSubscriber for block events
 * - No consensus, mining, or P2P components
 * - Focused solely on Lightning Network operations
 *
 * Lifecycle:
 * 1. Construct (empty)
 * 2. Initialize gRPC clients
 * 3. Initialize Lightning components
 * 4. Start all components
 * 5. Stop in reverse order
 */
struct LightningContext {
    //=========================================================================
    // gRPC Clients (replace direct blockchain access)
    //=========================================================================

    // Blockchain queries (replaces ChainDB)
    std::unique_ptr<dinero::grpc_client::BlockchainClient> blockchain;

    // Mempool operations (replaces Mempool)
    std::unique_ptr<dinero::grpc_client::MempoolClient> mempool;

    // Wallet operations (replaces WalletManager/HDWallet) - Phase 2
    std::unique_ptr<dinero::lightning::WalletClient> wallet;

    // Event streaming (replaces block event callbacks)
    // TODO Phase 3 Week 3-4: Add EventSubscriber when implemented
    // std::unique_ptr<dinero::grpc_client::EventSubscriber> events;

    //=========================================================================
    // Service Adapters (make gRPC clients look like dinerod services)
    //=========================================================================

    // Adapters that wrap gRPC clients to provide the same API as ChainstateService/MempoolService
    // This allows Lightning components to use the same code paths they use in dinerod
    std::unique_ptr<class ChainstateAdapter> chainstate;  // Wraps blockchain gRPC client
    std::unique_ptr<class MempoolAdapter> mempool_adapter;  // Wraps mempool gRPC client

    //=========================================================================
    // Lightning Components (same as dinerod)
    //=========================================================================
    // TODO Phase 3 Week 3-4: Add Lightning components when moving from dinerod
    // These are commented out for now to avoid incomplete type errors during compilation

    // // Channel lifecycle management
    // std::unique_ptr<dinero::lightning::ChannelManager> channel_mgr;

    // // HTLC processing and settlement
    // std::unique_ptr<dinero::lightning::HTLCManager> htlc_mgr;

    // // Payment pathfinding and routing
    // std::unique_ptr<dinero::lightning::PaymentRouter> payment_router;

    // // Commitment transaction construction
    // std::unique_ptr<dinero::lightning::CommitmentBuilder> commitment_builder;

    // // Breach detection and justice transactions
    // std::unique_ptr<dinero::lightning::WatchtowerClient> watchtower;

    // // CSV sweep after force-close
    // std::unique_ptr<dinero::lightning::LightningSweepManager> sweep_mgr;

    // // Lightning P2P peer management (port 9735)
    // std::unique_ptr<dinero::lightning::PeerManager> peer_mgr;

    // // Network graph and gossip protocol
    // std::unique_ptr<dinero::lightning::GossipManager> gossip_mgr;

    // // Event streaming to clients
    // std::unique_ptr<dinero::lightning::LightningEventManager> event_mgr;

    //=========================================================================
    // Storage
    //=========================================================================
    // TODO Phase 3 Week 3-4: Add Lightning database when implemented

    // // Lightning state database (SQLite)
    // std::unique_ptr<dinero::lightning::ILightningDB> db;

    //=========================================================================
    // Configuration and Logging
    //=========================================================================

    // Configuration (shared with or separate from dinerod)
    std::shared_ptr<dinero::ConfigService> config;

    // Logger interface
    dinero::ILogger* logger = nullptr;

    //=========================================================================
    // Configuration Values
    //=========================================================================

    // gRPC connection to dinerod
    std::string dinerod_grpc_address = "127.0.0.1:50051";

    // Lightning P2P port
    int lightning_port = 9735;

    // Data directory for Lightning state
    std::string data_dir = "~/.dinero/lightning";

    // Node public key (derived from wallet seed)
    std::vector<uint8_t> node_pubkey;

    //=========================================================================
    // Singleton Access (optional, for legacy compatibility)
    //=========================================================================

    /**
     * Get the global LightningContext instance
     * @return Pointer to singleton instance, or nullptr if not initialized
     */
    static LightningContext* instance();

    /**
     * Set the global LightningContext instance (called once by main())
     * @param ctx Pointer to the context to install as global
     */
    static void setInstance(LightningContext* ctx);

    /**
     * Destructor - defined in .cpp to allow incomplete types
     */
    ~LightningContext();

private:
    static LightningContext* s_instance;
};

} // namespace lightningd
