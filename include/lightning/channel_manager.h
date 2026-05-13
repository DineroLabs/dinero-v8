#pragma once

#include "lightning/lightning_types.h"
#include "lightning/lightning_db_interface.h"
#include "lightning/lightning_db_types.h"
#include "daemon/daemon_context.h"
#include "din_json.h"
#include "wallet/transaction.h"
#include <memory>
#include <string>
#include <vector>
#include <map>
#include <mutex>

// Forward declarations for oracle interfaces (::lightning namespace)
namespace lightning {
    class IChainOracle;
    class IWalletOracle;
    class IFundingService;
    class IHTLCSweepOracle;  // Phase 7B: HTLC sweep
    class ITimeOracle;  // Phase 8.5: Deterministic time
}

// Forward declarations for IPC (::dinero::ipc namespace)
namespace dinero {
namespace ipc {
    class WatchRegistrationClient;  // Phase 9.3: Watch registration
}
}

namespace dinero {

// Forward declaration
namespace wallet {
    class IWalletAPI;
}

namespace lightning {

// Forward declaration
class PeerManager;
class WatchtowerClient;
class CommitmentBuilder;
class LightningSweepManager;
class LightningEventManager;
class ChannelManagerCore;
struct BOLTMessage;

/**
 * @class ChannelManager
 * @brief Manages Lightning Network payment channels
 *
 * Phase 7.1: Core Lightning channel management system
 *
 * Responsibilities:
 * - Channel lifecycle management (open, close, force-close)
 * - Channel state persistence (RocksDB cf_channels)
 * - Balance tracking and updates
 * - HTLC coordination
 * - Commitment transaction versioning
 * - Peer connection management
 * - BOLT #2 message handling (channel protocol)
 *
 * Thread Safety: All public methods are thread-safe via internal mutex
 */
class ChannelManager {
public:
    /**
     * @brief Construct ChannelManager with daemon context
     * @param ctx Reference to DaemonContext for blockchain access
     * @param db Database instance for persistence
     * @param time_oracle Time oracle for deterministic timestamps (Phase 8.5)
     */
    ChannelManager(
        DaemonContext& ctx,
        std::shared_ptr<ILightningDB> db,
        ::lightning::ITimeOracle* time_oracle
    );
    ~ChannelManager();

    /**
     * @brief Set PeerManager reference (called by LightningService)
     * @param peer_mgr PeerManager instance for sending BOLT messages
     */
    void setPeerManager(std::shared_ptr<PeerManager> peer_mgr);

    /**
     * @brief Get PeerManager reference
     * @return Shared pointer to PeerManager (may be null if not initialized)
     */
    std::shared_ptr<PeerManager> getPeerManager() const { return m_peer_mgr; }

    /**
     * @brief Set WatchtowerClient reference (Phase 11: Revocation Flow)
     * @param watchtower_client WatchtowerClient instance for breach monitoring
     */
    void setWatchtowerClient(std::shared_ptr<WatchtowerClient> watchtower_client);

    /**
     * @brief Set CommitmentBuilder reference (Phase 13.3: Force Close)
     * @param commitment_builder CommitmentBuilder instance for building commitment txs
     */
    void setCommitmentBuilder(std::shared_ptr<CommitmentBuilder> commitment_builder);

    /**
     * @brief Set LightningSweepManager reference (Phase 13.4: CSV Timelock Sweep)
     * @param sweep_mgr LightningSweepManager instance for scheduling output sweeps
     */
    void setLightningSweepManager(std::shared_ptr<LightningSweepManager> sweep_mgr);

    /**
     * @brief Set event manager for real-time event streaming (Phase 14)
     * @param event_mgr Pointer to event manager (nullable)
     */
    void setEventManager(LightningEventManager* event_mgr);

    /**
     * @brief Set wallet API interface (Phase 3: Dependency Injection)
     * @param wallet_api Pointer to IWalletAPI implementation
     */
    void setWalletAPI(wallet::IWalletAPI* wallet_api);

    /**
     * @brief Set watch registration client (Phase 9.3: Bidirectional Oracle Communication)
     * @param watch_client Shared pointer to WatchRegistrationClient for dynamic TX watch registration
     */
    void setWatchRegistrationClient(std::shared_ptr<ipc::WatchRegistrationClient> watch_client);

    // ═══════════════════════════════════════════════════════════════════════════
    // Channel Lifecycle
    // ═══════════════════════════════════════════════════════════════════════════

    /**
     * @brief Open a new Lightning channel with a peer
     *
     * Creates a funding transaction, broadcasts it, and waits for confirmations.
     * The channel enters PENDING_OPEN state until funding tx reaches
     * FUNDING_TX_CONFIRMATIONS (default: 6 blocks).
     *
     * @param peer_node_id Remote peer's node public key (33-byte hex)
     * @param local_amount_sats Our contribution to channel capacity
     * @param push_amount_sats Initial amount to push to peer (optional)
     * @param to_self_delay CSV delay for our outputs (default: 144 blocks)
     * @return ::Result<Channel> Newly created channel or error
     *
     * Example:
     *   auto result = channel_mgr.openChannel(
     *       "02abcd1234...",   // peer pubkey
     *       1000000,           // 0.01 DIN
     *       100000,            // Push 0.001 DIN to peer
     *       144                // 1-day timelock
     *   );
     *   if (result.isOk()) {
     *       std::cout << "Channel ID: " << result.unwrap().channel_id << "\n";
     *   }
     */
    ::Result<Channel> openChannel(
        const std::string& peer_node_id,
        uint64_t local_amount_sats,
        uint64_t push_amount_sats = 0,
        uint32_t to_self_delay = constants::DEFAULT_TO_SELF_DELAY
    );

    /**
     * @brief Close a channel cooperatively or forcefully
     *
     * If force=false, attempts cooperative close (preferred):
     * - Negotiate final balance with peer
     * - Create closing transaction
     * - Both parties sign and broadcast
     *
     * If force=true or cooperative fails:
     * - Broadcast latest commitment transaction
     * - Enter FORCE_CLOSING state
     * - Wait for to_self_delay before claiming outputs
     *
     * @param channel_id Channel to close (32-byte hex)
     * @param force Force unilateral close (default: false)
     * @return ::Result<void> Success or error
     */
    ::Result<void> closeChannel(const std::string& channel_id, bool force = false);

    /**
     * @brief Handle channel breach (old commitment transaction broadcast)
     *
     * Called when we detect that the peer broadcast a revoked commitment.
     * Automatically broadcasts breach remedy transaction to claim all funds
     * using the revocation secret.
     *
     * @param channel_id Channel with detected breach
     * @param commitment_number Revoked commitment number
     * @return ::Result<void> Success or error
     */
    ::Result<void> handleBreach(const std::string& channel_id, uint64_t commitment_number);

    // ═══════════════════════════════════════════════════════════════════════════
    // Channel Queries
    // ═══════════════════════════════════════════════════════════════════════════

    /**
     * @brief Get channel by ID
     * @param channel_id Channel identifier (32-byte hex)
     * @return std::optional<Channel> Channel if found, nullopt otherwise
     */
    std::optional<Channel> getChannel(const std::string& channel_id) const;

    /**
     * @brief Get channel by Short Channel ID (SCID)
     * @param scid Short Channel ID (BOLT #7 format)
     * @return std::optional<Channel> Channel if found, nullopt otherwise
     */
    std::optional<Channel> getChannelBySCID(uint64_t scid) const;

    /**
     * @brief List all channels
     * @param state_filter Optional state filter (e.g., only OPEN channels)
     * @return std::vector<Channel> All matching channels
     */
    std::vector<Channel> listChannels(
        std::optional<ChannelState> state_filter = std::nullopt
    ) const;

    /**
     * @brief Get channel statistics
     * @return Json object with channel counts, total capacity, etc.
     */
    din::Json getStats() const;

    // ═══════════════════════════════════════════════════════════════════════════
    // Channel Updates (called by CommitmentBuilder and HTLCManager)
    // ═══════════════════════════════════════════════════════════════════════════

    /**
     * @brief Update channel balance after commitment transaction
     * @param channel_id Channel to update
     * @param local_balance_msats New local balance (milliuna)
     * @param remote_balance_msats New remote balance (milliuna)
     * @param commitment_number New commitment transaction version
     * @return ::Result<void> Success or error
     */
    ::Result<void> updateBalance(
        const std::string& channel_id,
        uint64_t local_balance_msats,
        uint64_t remote_balance_msats,
        uint64_t commitment_number
    );

    /**
     * @brief Add HTLC to channel
     * @param channel_id Channel to add HTLC to
     * @param htlc HTLC to add
     * @return ::Result<void> Success or error
     */
    ::Result<void> addHTLC(const std::string& channel_id, const HTLC& htlc);

    /**
     * @brief Settle HTLC with preimage
     * @param channel_id Channel containing HTLC
     * @param htlc_id HTLC to settle
     * @param preimage Payment preimage (32 bytes)
     * @return ::Result<void> Success or error
     */
    ::Result<void> settleHTLC(
        const std::string& channel_id,
        const std::string& htlc_id,
        const std::vector<uint8_t>& preimage
    );

    /**
     * @brief Fail HTLC (payment failed or timed out)
     * @param channel_id Channel containing HTLC
     * @param htlc_id HTLC to fail
     * @param reason Failure reason string
     * @return ::Result<void> Success or error
     */
    ::Result<void> failHTLC(
        const std::string& channel_id,
        const std::string& htlc_id,
        const std::string& reason
    );

    // ═══════════════════════════════════════════════════════════════════════════
    // Persistence
    // ═══════════════════════════════════════════════════════════════════════════

    /**
     * @brief Load channels from database on startup
     * @return ::Result<void> Success or error
     */
    ::Result<void> loadChannels();

    /**
     * @brief Save channel to database
     * @param channel Channel to persist
     * @return ::Result<void> Success or error
     */
    ::Result<void> saveChannel(const Channel& channel);

    /**
     * @brief Delete channel from database
     * @param channel_id Channel to delete
     * @return ::Result<void> Success or error
     */
    ::Result<void> deleteChannel(const std::string& channel_id);

    // ═══════════════════════════════════════════════════════════════════════════
    // Block Processing (called by DaemonContext on new blocks)
    // ═══════════════════════════════════════════════════════════════════════════

    /**
     * @brief Process new block for channel updates
     *
     * Called on each new block to:
     * - Confirm funding transactions (PENDING_OPEN → OPEN)
     * - Detect channel breaches (revoked commitments)
     * - Timeout expired HTLCs
     * - Monitor force-close confirmations
     *
     * @param block_height New block height
     * @param block_hash New block hash
     * @return ::Result<void> Success or error
     */
    ::Result<void> onNewBlock(uint64_t block_height, const std::string& block_hash);

    // ═══════════════════════════════════════════════════════════════════════════
    // BOLT #2 Message Handlers (Phase 7.3: Channel Opening Protocol)
    // ═══════════════════════════════════════════════════════════════════════════

    /**
     * @brief Handle OPEN_CHANNEL message from peer
     *
     * Called when a peer wants to open a channel with us (we are the acceptor).
     * Validates parameters and sends ACCEPT_CHANNEL response.
     *
     * @param peer_node_id Peer's node ID
     * @param message BOLT OPEN_CHANNEL message
     */
    void handleOpenChannel(const std::string& peer_node_id, const BOLTMessage& message);

    /**
     * @brief Handle ACCEPT_CHANNEL message from peer
     *
     * Called when peer accepts our channel open request (we are the initiator).
     * Builds and signs funding transaction, sends FUNDING_CREATED.
     *
     * @param peer_node_id Peer's node ID
     * @param message BOLT ACCEPT_CHANNEL message
     */
    void handleAcceptChannel(const std::string& peer_node_id, const BOLTMessage& message);

    /**
     * @brief Handle FUNDING_CREATED message from peer
     *
     * Called when peer has created the funding transaction (we are the acceptor).
     * Verifies and signs, sends FUNDING_SIGNED response.
     *
     * @param peer_node_id Peer's node ID
     * @param message BOLT FUNDING_CREATED message
     */
    void handleFundingCreated(const std::string& peer_node_id, const BOLTMessage& message);

    /**
     * @brief Handle FUNDING_SIGNED message from peer
     *
     * Called when peer has signed the funding transaction (we are the initiator).
     * Broadcasts funding tx to the blockchain.
     *
     * @param peer_node_id Peer's node ID
     * @param message BOLT FUNDING_SIGNED message
     */
    void handleFundingSigned(const std::string& peer_node_id, const BOLTMessage& message);

    /**
     * @brief Handle CHANNEL_READY message from peer
     *
     * Called when peer signals that funding tx is confirmed.
     * Marks channel as fully open and ready for payments.
     *
     * @param peer_node_id Peer's node ID
     * @param message BOLT CHANNEL_READY message
     */
    void handleChannelReady(const std::string& peer_node_id, const BOLTMessage& message);

    /**
     * @brief Handle REVOKE_AND_ACK message from peer (Phase 11: Revocation Flow)
     *
     * Called when peer revokes their old commitment transaction.
     * Extracts per_commitment_secret and stores it in WatchtowerClient
     * for potential breach remedy.
     *
     * @param peer_node_id Peer's node ID
     * @param message BOLT REVOKE_AND_ACK message
     */
    void handleRevokeAndAck(const std::string& peer_node_id, const BOLTMessage& message);

    /**
     * @brief Handle SHUTDOWN message from peer (Phase 13.2: Cooperative Close)
     *
     * Called when peer initiates cooperative channel close.
     * Validates scriptpubkey and prepares for fee negotiation.
     *
     * @param peer_node_id Peer's node ID
     * @param message BOLT SHUTDOWN message
     */
    void handleShutdown(const std::string& peer_node_id, const BOLTMessage& message);

    /**
     * @brief Handle CLOSING_SIGNED message from peer (Phase 13.2: Fee Negotiation)
     *
     * Called during cooperative close fee negotiation.
     * Detects agreement and builds final closing transaction.
     *
     * @param peer_node_id Peer's node ID
     * @param message BOLT CLOSING_SIGNED message
     */
    void handleClosingSigned(const std::string& peer_node_id, const BOLTMessage& message);

private:
    // ═══════════════════════════════════════════════════════════════════════════
    // Internal State
    // ═══════════════════════════════════════════════════════════════════════════

    DaemonContext& m_daemon_ctx;                           // Blockchain access
    std::shared_ptr<ILightningDB> m_db;                     // Persistence layer
    std::shared_ptr<PeerManager> m_peer_mgr;               // Peer messaging
    std::shared_ptr<WatchtowerClient> m_watchtower;        // Breach monitoring (Phase 11)
    std::shared_ptr<CommitmentBuilder> m_commitment_builder; // Commitment tx builder (Phase 13.3)
    std::shared_ptr<LightningSweepManager> m_sweep_mgr;    // CSV timelock sweep manager (Phase 13.4)
    LightningEventManager* m_event_mgr;                    // Event manager for Phase 14
    wallet::IWalletAPI* m_wallet_api;                      // Wallet API interface (Phase 3)
    std::shared_ptr<ipc::WatchRegistrationClient> m_watch_client; // Phase 9.3: Watch registration client

    // ═══════════════════════════════════════════════════════════════════════════
    // L2 State Machine (Pure Lightning Logic)
    // ═══════════════════════════════════════════════════════════════════════════

    std::shared_ptr<ChannelManagerCore> m_core;            // Pure L2 state machine
    std::shared_ptr<::lightning::IChainOracle> m_chain_oracle;   // Blockchain queries
    std::shared_ptr<::lightning::IWalletOracle> m_wallet_oracle; // Wallet queries
    std::shared_ptr<::lightning::IFundingService> m_funding_service; // Funding TX creation
    std::shared_ptr<::lightning::IHTLCSweepOracle> m_sweep_oracle;  // Phase 7B: HTLC sweep service
    ::lightning::ITimeOracle* m_time_oracle;               // Phase 8.5: Deterministic time (NOT owned)

    // Channel storage (in-memory cache, backed by RocksDB)
    mutable std::mutex m_channels_mutex;
    std::map<std::string, Channel> m_channels;             // channel_id → Channel
    std::map<uint64_t, std::string> m_scid_to_channel;    // SCID → channel_id mapping

    // Pending funding transactions (awaiting FUNDING_SIGNED from peer)
    mutable std::mutex m_pending_funding_txs_mutex;
    std::map<std::string, Transaction> m_pending_funding_txs;  // channel_id → funding_tx

    // Node identity (our Lightning node pubkey)
    std::vector<uint8_t> m_node_pubkey;                    // 33-byte compressed pubkey
    std::vector<uint8_t> m_node_privkey;                   // 32-byte private key

    // ═══════════════════════════════════════════════════════════════════════════
    // BOLT Message Helpers
    // ═══════════════════════════════════════════════════════════════════════════

    /**
     * @brief Serialize OPEN_CHANNEL message payload
     */
    std::vector<uint8_t> serializeOpenChannel(const Channel& channel);

    /**
     * @brief Serialize ACCEPT_CHANNEL message payload
     */
    std::vector<uint8_t> serializeAcceptChannel(const Channel& channel);

    /**
     * @brief Serialize FUNDING_CREATED message payload
     */
    std::vector<uint8_t> serializeFundingCreated(
        const std::string& temporary_channel_id,
        const std::string& funding_txid,
        uint16_t funding_output_index,
        const std::vector<uint8_t>& signature
    );

    /**
     * @brief Serialize FUNDING_SIGNED message payload
     */
    std::vector<uint8_t> serializeFundingSigned(
        const std::string& channel_id,
        const std::vector<uint8_t>& signature
    );

    /**
     * @brief Serialize CHANNEL_READY message payload
     */
    std::vector<uint8_t> serializeChannelReady(
        const std::string& channel_id,
        const std::vector<uint8_t>& next_per_commitment_point
    );

    /**
     * @brief Parse OPEN_CHANNEL message payload
     */
    ::Result<Channel> parseOpenChannel(const std::vector<uint8_t>& payload);

    /**
     * @brief Parse ACCEPT_CHANNEL message payload
     */
    ::Result<Channel> parseAcceptChannel(const std::vector<uint8_t>& payload);

    /**
     * @brief Parse FUNDING_CREATED message payload
     */
    struct FundingCreatedData {
        std::string temporary_channel_id;
        std::string funding_txid;
        uint16_t funding_output_index;
        std::vector<uint8_t> signature;
    };
    ::Result<FundingCreatedData> parseFundingCreated(const std::vector<uint8_t>& payload);

    /**
     * @brief Parse FUNDING_SIGNED message payload
     */
    struct FundingSignedData {
        std::string channel_id;
        std::vector<uint8_t> signature;
    };
    ::Result<FundingSignedData> parseFundingSigned(const std::vector<uint8_t>& payload);

    /**
     * @brief Parse CHANNEL_READY message payload
     */
    struct ChannelReadyData {
        std::string channel_id;
        std::vector<uint8_t> next_per_commitment_point;
    };
    ::Result<ChannelReadyData> parseChannelReady(const std::vector<uint8_t>& payload);

    // Statistics
    struct Stats {
        uint64_t total_channels = 0;
        uint64_t open_channels = 0;
        uint64_t pending_channels = 0;
        uint64_t total_capacity_sats = 0;
        uint64_t total_local_balance_msats = 0;
        uint64_t total_remote_balance_msats = 0;
        uint64_t total_htlcs = 0;
    };
    mutable Stats m_stats;

    // ═══════════════════════════════════════════════════════════════════════════
    // Internal Helpers
    // ═══════════════════════════════════════════════════════════════════════════

    /**
     * @brief Generate unique 32-byte channel ID
     * @param funding_txid Funding transaction hash
     * @param funding_vout Funding transaction output index
     * @return std::string Channel ID (32-byte hex)
     */
    std::string generateChannelId(const std::string& funding_txid, uint32_t funding_vout) const;

    /**
     * @brief Create funding transaction for new channel
     * @param local_amount_sats Our contribution
     * @param local_pubkey Our funding pubkey (32 bytes x-only)
     * @param remote_pubkey Peer's funding pubkey (32 bytes x-only)
     * @return ::Result<std::pair<std::string, uint32_t>> (txid, vout) or error
     */
    ::Result<std::pair<std::string, uint32_t>> createFundingTransaction(
        uint64_t local_amount_sats,
        const std::vector<uint8_t>& local_pubkey,
        const std::vector<uint8_t>& remote_pubkey
    );

    /**
     * @brief Verify channel invariants
     * @param channel Channel to validate
     * @return ::Result<void> Success or error with reason
     */
    ::Result<void> validateChannel(const Channel& channel) const;

    /**
     * @brief Update internal statistics
     */
    void updateStats();

    /**
     * @brief Convert Channel to ChannelRecord for database storage
     * @param channel Channel to convert
     * @return ChannelRecord Typed record for MessagePack serialization
     */
    ChannelRecord channelToChannelRecord(const Channel& channel) const;

    /**
     * @brief Convert ChannelRecord back to Channel
     * @param record ChannelRecord from database
     * @return Channel Runtime channel object
     */
    Channel channelRecordToChannel(const ChannelRecord& record) const;
};

} // namespace lightning
} // namespace dinero
