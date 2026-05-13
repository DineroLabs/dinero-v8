#ifndef DINEROCOIN_WALLET_REFERENCE_BLOCKCHAIN_SYNC_H
#define DINEROCOIN_WALLET_REFERENCE_BLOCKCHAIN_SYNC_H

#include <string>
#include <memory>
#include <cstdint>
#include "common/rpc_client.h"

namespace dinero {
namespace wallet {
namespace reference {

// Forward declarations
class ReferenceWallet;
class UTXOManager;
class Database;

/**
 * Blockchain Sync Configuration
 */
struct BlockchainSyncConfig {
    std::string rpc_url = "http://127.0.0.1:8332";
    std::string rpc_user = "dinero";
    std::string rpc_password = "";
    int rpc_timeout = 30;
    uint32_t start_height = 0;          // Height to start scanning from
    uint32_t batch_size = 100;          // Number of blocks to fetch per batch
    bool verbose = false;
};

/**
 * Blockchain Sync Statistics
 */
struct BlockchainSyncStats {
    uint32_t start_height;              // Height where sync started
    uint32_t current_height;            // Current blockchain height
    uint32_t scanned_height;            // Last scanned block height
    uint32_t blocks_scanned;            // Total blocks scanned
    uint32_t transactions_found;        // Transactions relevant to wallet
    uint32_t utxos_added;               // UTXOs added
    uint32_t utxos_spent;               // UTXOs marked as spent
    int64_t sync_start_time;            // Unix timestamp when sync started
    int64_t sync_end_time;              // Unix timestamp when sync completed (0 if ongoing)
    bool is_syncing;                    // Currently syncing?
    std::string last_error;             // Last error message (empty if no error)
};

/**
 * Minimal Blockchain Sync
 *
 * Read-only, address-scoped blockchain synchronization for reference wallet.
 *
 * Guarantees:
 * 1. Deterministic: Same blockchain state = same UTXO set
 * 2. Read-only: Never broadcasts transactions
 * 3. Address-scoped: Only scans for wallet's single address
 * 4. Simple: Forward-scan only, no complex reorg handling initially
 */
class BlockchainSync {
public:
    /**
     * Create blockchain sync instance
     * @param wallet Pointer to wallet (non-owning)
     * @param config Sync configuration
     */
    explicit BlockchainSync(
        ReferenceWallet* wallet,
        const BlockchainSyncConfig& config = BlockchainSyncConfig{}
    );

    ~BlockchainSync();

    // Disable copy/move
    BlockchainSync(const BlockchainSync&) = delete;
    BlockchainSync& operator=(const BlockchainSync&) = delete;

    /**
     * Initialize connection to blockchain node
     * @return true if connection successful
     */
    bool Initialize();

    /**
     * Sync blockchain from start_height to current tip
     * @param start_height Height to start from (0 = from beginning)
     * @param max_blocks Maximum blocks to scan (0 = scan to tip)
     * @return true if sync completed successfully
     */
    bool Sync(uint32_t start_height = 0, uint32_t max_blocks = 0);

    /**
     * Sync specific block by height
     * @param height Block height to sync
     * @return true if block processed successfully
     */
    bool SyncBlock(uint32_t height);

    /**
     * Sync specific block by hash
     * @param block_hash Block hash (hex)
     * @return true if block processed successfully
     */
    bool SyncBlock(const std::string& block_hash);

    /**
     * Get current blockchain height from node
     * @return Current height (0 if error)
     */
    uint32_t GetCurrentHeight() const;

    /**
     * Get sync statistics
     * @return Sync stats structure
     */
    BlockchainSyncStats GetStats() const;

    /**
     * Check if node is connected and reachable
     * @return true if connected
     */
    bool IsConnected() const;

    /**
     * Stop ongoing sync operation
     */
    void Stop();

    /**
     * Get last error message
     * @return Error string (empty if no error)
     */
    std::string GetLastError() const;

private:
    // Process a single block
    bool ProcessBlock(uint32_t height);

    // Process a single block by hash
    bool ProcessBlockByHash(const std::string& block_hash, uint32_t height);

    // Parse block and extract relevant transactions
    void ParseBlock(
        const Json::Value& block,
        uint32_t height
    );

    // Parse transaction and extract relevant UTXOs
    void ParseTransaction(
        const Json::Value& tx,
        uint32_t height,
        bool is_coinbase
    );

    // Check if output is relevant to wallet
    bool IsRelevantOutput(const Json::Value& vout) const;

    // Extract address from output
    std::string ExtractAddress(const Json::Value& vout) const;

    // Update sync statistics
    void UpdateStats();

    // Member variables
    ReferenceWallet* wallet_;               // Non-owning pointer
    BlockchainSyncConfig config_;
    std::unique_ptr<Dinero::Common::RPCClient> rpc_client_;

    // Sync state
    BlockchainSyncStats stats_;
    bool stop_requested_;
    mutable std::string last_error_;
};

} // namespace reference
} // namespace wallet
} // namespace dinero

#endif // DINEROCOIN_WALLET_REFERENCE_BLOCKCHAIN_SYNC_H
