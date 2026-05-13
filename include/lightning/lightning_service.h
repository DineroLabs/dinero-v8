#pragma once

#include "lightning/lightning_types.h"
#include "lightning/lightning_db_interface.h"
#include "lightning/time_oracle.h"  // Phase 8.5: Deterministic time
#include "daemon/iservice.h"  // IService interface
#include "common/thread_pool.h"  // Thread pool for isolation
#include "din_json.h"
#include <memory>
#include <string>
#include <queue>
#include <atomic>

// Forward declaration (DaemonContext is in global namespace)
struct DaemonContext;

namespace dinero {

// Forward declaration for WalletManager
class WalletManager;

// Forward declaration for wallet API
namespace wallet {
    class IWalletAPI;
}

// Forward declaration for IPC (Phase 9.3)
namespace ipc {
    class WatchRegistrationClient;
}

namespace lightning {

// Forward declarations
class ChannelManager;
class HTLCManager;
class PaymentRouter;
class CommitmentBuilder;
class PeerManager;
class WatchtowerClient;
class LightningSweepManager;
class LightningEventManager;

/**
 * @class LightningService
 * @brief Per-Wallet Lightning Network service
 *
 * Architecture:
 * - Lightning state stored in per-wallet database (wallet_<name>.db)
 * - Node identity derived from wallet's HD seed (BIP32 m/84'/1448'/9735'/0'/0')
 * - Lightning initialized only when wallet is opened
 * - Full isolation between different wallets' Lightning state
 *
 * Lifecycle:
 * 1. Init(DaemonContext&) - minimal initialization at daemon startup
 * 2. InitForWallet(WalletManager*) - full initialization when wallet opens
 * 3. StopForWallet() - cleanup when wallet closes
 *
 * Responsibilities:
 * - Initialize all Lightning components per wallet
 * - Coordinate between ChannelManager, HTLCManager, and PaymentRouter
 * - Handle block processing events
 * - Provide high-level Lightning API
 *
 * Thread Safety: All public methods are thread-safe
 */
class LightningService : public dinero::IService {
public:
    /**
     * @brief Construct LightningService
     */
    LightningService();
    ~LightningService() override;

    // ═══════════════════════════════════════════════════════════════════════════
    // IService Interface Implementation
    // ═══════════════════════════════════════════════════════════════════════════

    std::string Name() const override { return "Lightning"; }

    /**
     * @brief Minimal daemon-level initialization
     * @param ctx Daemon context
     * @return true (Lightning initialization is now per-wallet)
     */
    bool Init(DaemonContext& ctx) override;

    /**
     * @brief Start Lightning (now a no-op, use InitForWallet instead)
     * @return true (Lightning starts per-wallet)
     */
    bool Start() override;

    /**
     * @brief Stop Lightning (now a no-op, use StopForWallet instead)
     */
    void Stop() override;

    // ═══════════════════════════════════════════════════════════════════════════
    // Per-Wallet Lightning Lifecycle
    // ═══════════════════════════════════════════════════════════════════════════

    /**
     * @brief Initialize Lightning for currently active wallet
     *
     * Called by WalletManager after wallet is opened.
     * Initializes:
     * - Node identity from wallet's HD seed
     * - SQLiteLightningDB from wallet's database
     * - All Lightning components (channels, HTLCs, routing)
     * - Peer manager and message handlers
     *
     * @param wallet_mgr Pointer to WalletManager (must have active wallet)
     * @return true if initialization successful
     */
    bool InitForWallet(dinero::WalletManager* wallet_mgr);

    /**
     * @brief Stop Lightning for current wallet
     *
     * Called by WalletManager before wallet is closed.
     * Performs graceful shutdown:
     * - Fails all pending HTLCs
     * - Persists final state to database
     * - Stops peer manager
     * - Cleans up all components
     */
    void StopForWallet();


    // ═══════════════════════════════════════════════════════════════════════════
    // Component Access
    // ═══════════════════════════════════════════════════════════════════════════

    /**
     * @brief Get ChannelManager instance
     * @return ChannelManager& Reference to channel manager
     */
    ChannelManager& getChannelManager();

    /**
     * @brief Get HTLCManager instance
     * @return HTLCManager& Reference to HTLC manager
     */
    HTLCManager& getHTLCManager();

    /**
     * @brief Get PaymentRouter instance
     * @return PaymentRouter& Reference to payment router
     */
    PaymentRouter& getPaymentRouter();

    /**
     * @brief Get CommitmentBuilder instance
     * @return CommitmentBuilder& Reference to commitment builder
     */
    CommitmentBuilder& getCommitmentBuilder();

    /**
     * @brief Get Lightning database instance
     * @return std::shared_ptr<ILightningDB> Database instance
     */
    std::shared_ptr<ILightningDB> getDatabase() const { return m_db; }

    /**
     * @brief Get node public key (Lightning node ID)
     * @return const std::vector<uint8_t>& 33-byte compressed public key
     */
    const std::vector<uint8_t>& getNodePubkey() const { return m_node_pubkey; }

    /**
     * @brief Get node private key (for onion decryption and signing)
     * @return const std::vector<uint8_t>& 32-byte private key
     */
    const std::vector<uint8_t>& getNodePrivkey() const { return m_node_privkey; }

    /**
     * @brief Get Lightning event manager
     * @return LightningEventManager* Event manager instance (may be nullptr)
     */
    LightningEventManager* getEventManager() const { return m_event_mgr.get(); }

    // ═══════════════════════════════════════════════════════════════════════════
    // High-Level Lightning API
    // ═══════════════════════════════════════════════════════════════════════════

    /**
     * @brief Send Lightning payment to destination
     *
     * High-level payment API that:
     * - Finds optimal route
     * - Creates HTLCs
     * - Waits for settlement
     * - Returns preimage on success
     *
     * @param destination_node_id Target node pubkey (33-byte hex)
     * @param amount_msats Payment amount in milliuna
     * @param max_fee_msats Maximum routing fee (0 = no limit)
     * @param timeout_ms Payment timeout (default: 60000 = 1 minute)
     * @return Result<std::vector<uint8_t>> Preimage if successful, or error
     */
    Result<std::vector<uint8_t>> sendPayment(
        const std::string& destination_node_id,
        uint64_t amount_msats,
        uint64_t max_fee_msats = 0,
        uint64_t timeout_ms = 60000
    );

    /**
     * @brief Create Lightning invoice
     *
     * Generates a BOLT #11 invoice for receiving payments.
     *
     * @param amount_msats Invoice amount
     * @param description Human-readable description
     * @param expiry_seconds Invoice expiry time (default: 3600 = 1 hour)
     * @return Result<LightningInvoice> Invoice or error
     */
    Result<LightningInvoice> createInvoice(
        uint64_t amount_msats,
        const std::string& description,
        uint32_t expiry_seconds = 3600
    );

    /**
     * @brief Pay Lightning invoice
     *
     * Decodes invoice and sends payment.
     *
     * @param bolt11_invoice BOLT #11 invoice string
     * @param timeout_ms Payment timeout
     * @return Result<std::vector<uint8_t>> Preimage if successful, or error
     */
    Result<std::vector<uint8_t>> payInvoice(
        const std::string& bolt11_invoice,
        uint64_t timeout_ms = 60000
    );

    // ═══════════════════════════════════════════════════════════════════════════
    // Block Processing (called by DaemonContext)
    // ═══════════════════════════════════════════════════════════════════════════

    /**
     * @brief Enqueue block event for async processing (SAFETY PATTERN #2)
     *
     * CRITICAL: This method NEVER blocks the daemon's block processing thread.
     * Block events are queued and processed asynchronously by Lightning's
     * dedicated thread pool, ensuring LN cannot slow down consensus.
     *
     * Called by DaemonContext when a new block is accepted.
     *
     * @param block_height New block height
     * @param block_hash New block hash
     */
    void enqueueBlockEvent(uint64_t block_height, const std::string& block_hash);

    /**
     * @brief Process new block (internal, runs on Lightning thread pool)
     *
     * Updates Lightning state:
     * - Confirms funding transactions
     * - Detects channel breaches
     * - Times out expired HTLCs
     * - Monitors force-close confirmations
     *
     * @param block_height New block height
     * @param block_hash New block hash
     * @return Result<void> Success or error
     */
    Result<void> onNewBlock(uint64_t block_height, const std::string& block_hash);

    // ═══════════════════════════════════════════════════════════════════════════
    // Statistics
    // ═══════════════════════════════════════════════════════════════════════════

    /**
     * @brief Get Lightning service statistics
     * @return din::Json Comprehensive stats (channels, HTLCs, payments, routing)
     */
    din::Json getStats() const;

    // ═══════════════════════════════════════════════════════════════════════════
    // Health Monitoring (SAFETY PATTERN #5)
    // ═══════════════════════════════════════════════════════════════════════════

    /**
     * @brief Check if Lightning service is healthy
     *
     * Health criteria:
     * - Thread pool is responsive
     * - Heartbeat is recent (within threshold)
     * - No critical resource exhaustion
     * - Components initialized properly
     *
     * @return bool true if healthy, false otherwise
     */
    bool isHealthy() const;

    /**
     * @brief Get health status details
     * @return din::Json Detailed health information
     */
    din::Json getHealthStatus() const;

private:
    // ═══════════════════════════════════════════════════════════════════════════
    // Internal State
    // ═══════════════════════════════════════════════════════════════════════════

    DaemonContext* m_daemon_ctx;                           // Blockchain context (stored in Init())
    dinero::WalletManager* m_wallet_mgr;                   // Wallet manager (stored in InitForWallet())
    std::shared_ptr<ILightningDB> m_db;                    // Per-wallet persistence layer
    std::unique_ptr<wallet::IWalletAPI> m_wallet_client;   // Wallet API client (Phase 3)
    std::shared_ptr<ipc::WatchRegistrationClient> m_watch_client;  // Phase 9.3: Watch registration client
    std::unique_ptr<::lightning::ITimeOracle> m_time_oracle; // Phase 8.5: Deterministic time source (Phase M.4: Global namespace)

    // Lightning components
    std::unique_ptr<ChannelManager> m_channel_mgr;
    std::unique_ptr<HTLCManager> m_htlc_mgr;
    std::unique_ptr<PaymentRouter> m_payment_router;
    std::unique_ptr<CommitmentBuilder> m_commitment_builder;
    std::unique_ptr<PeerManager> m_peer_mgr;
    std::unique_ptr<WatchtowerClient> m_watchtower;            // Phase 10: Breach detection & penalty enforcement
    std::unique_ptr<LightningSweepManager> m_sweep_mgr;        // Phase 13.4: CSV timelock output sweep
    std::unique_ptr<LightningEventManager> m_event_mgr;        // Phase 14: Live event stream for WebSocket
    std::shared_ptr<void> m_ws_subscription;  // Phase 14.4: WebSocket Live Push subscription (type-erased SubscriptionHandle)

    // Thread isolation (SAFETY PATTERN #1)
    std::unique_ptr<ThreadPool> m_lightning_pool;          // Dedicated worker threads for LN operations

    // Event queue isolation (SAFETY PATTERN #2)
    struct BlockEvent {
        uint64_t height;
        std::string hash;
    };
    mutable std::mutex m_block_queue_mutex;
    std::queue<BlockEvent> m_block_events;                 // Async block event queue
    std::atomic<size_t> m_queued_blocks{0};                // Event count for monitoring

    // Service state
    bool m_initialized;
    mutable std::mutex m_mutex;

    // Node identity (derived from wallet's HD seed)
    std::vector<uint8_t> m_node_pubkey;                    // 33-byte compressed pubkey
    std::vector<uint8_t> m_node_privkey;                   // 32-byte private key

    // Health monitoring (SAFETY PATTERN #5)
    std::atomic<uint64_t> m_last_heartbeat{0};             // Unix timestamp of last successful operation
    std::atomic<uint64_t> m_last_block_processed{0};       // Unix timestamp of last block processed
    std::atomic<uint64_t> m_total_events_processed{0};     // Total events processed (lifetime counter)
    std::atomic<uint64_t> m_total_exceptions_caught{0};    // Total exceptions caught (health indicator)

    // ═══════════════════════════════════════════════════════════════════════════
    // Internal Helpers
    // ═══════════════════════════════════════════════════════════════════════════

    /**
     * @brief Initialize node identity from wallet's HD seed
     *
     * Derives Lightning node keypair using BIP32 path:
     * m/84'/1448'/9735'/0'/0'
     *
     * @return Result<void> Success or error
     */
    Result<void> initializeNodeIdentity();

    /**
     * @brief Generate random preimage
     * @return std::vector<uint8_t> 32-byte preimage
     */
    std::vector<uint8_t> generatePreimage() const;

    /**
     * @brief Process queued block events asynchronously (SAFETY PATTERN #2)
     *
     * Drains the block event queue and schedules each event for processing
     * on the Lightning thread pool. This ensures:
     * - daemon never blocks waiting for LN
     * - LN processes events at its own pace
     * - LN overload cannot slow down consensus
     *
     * Called periodically by event loop or manually for testing.
     */
    void processEvents();
};

} // namespace lightning
} // namespace dinero
