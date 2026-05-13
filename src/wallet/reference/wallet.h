#ifndef DINEROCOIN_WALLET_REFERENCE_WALLET_H
#define DINEROCOIN_WALLET_REFERENCE_WALLET_H

#include <string>
#include <vector>
#include <memory>
#include <cstdint>

namespace dinero {
namespace wallet {
namespace reference {

// Forward declarations
class Database;
class UTXOManager;
class TransactionBuilder;

/**
 * UTXO structure - represents an unspent transaction output
 */
struct UTXO {
    std::string txid;          // Transaction ID (hex)
    uint32_t vout;             // Output index
    uint64_t amount;           // Amount in una
    std::string script_pubkey; // Script pubkey (hex)
    uint32_t height;           // Block height when confirmed
    bool is_coinbase;          // Is this a coinbase output?

    // Deterministic sorting: first by txid, then by vout
    bool operator<(const UTXO& other) const {
        if (txid != other.txid) return txid < other.txid;
        return vout < other.vout;
    }
};

/**
 * Transaction structure
 */
struct Transaction {
    std::string txid;
    std::string hex;           // Raw transaction hex
    uint64_t amount_sent;      // Amount sent (excluding change)
    uint64_t fee;              // Transaction fee
    std::string to_address;    // Recipient address
    uint32_t height;           // Block height (0 if unconfirmed)
    int64_t timestamp;         // Unix timestamp
};

/**
 * Balance breakdown
 */
struct Balance {
    uint64_t confirmed;        // Confirmed balance (>= 1 confirmation)
    uint64_t unconfirmed;      // Unconfirmed balance (0 confirmations)
    uint64_t immature;         // Immature coinbase (< 100 confirmations)
    uint64_t total;            // Total = confirmed + unconfirmed + immature
};

/**
 * Reference Wallet
 *
 * Guarantees:
 * 1. Single address per wallet (never changes)
 * 2. Deterministic UTXO selection (sorted by txid:vout)
 * 3. Byte-identical transactions (same inputs + fee = same tx)
 * 4. No RBF, no replace-by-fee
 * 5. Explicit fees only (no estimation)
 */
class ReferenceWallet {
public:
    /**
     * Create a new wallet from mnemonic
     * @param wallet_name Wallet identifier
     * @param mnemonic BIP39 mnemonic (12/24 words)
     * @param passphrase Optional BIP39 passphrase
     * @param data_dir Directory for wallet.db
     * @return Unique pointer to wallet instance
     */
    static std::unique_ptr<ReferenceWallet> CreateFromMnemonic(
        const std::string& wallet_name,
        const std::string& mnemonic,
        const std::string& passphrase = "",
        const std::string& data_dir = "."
    );

    /**
     * Generate new random mnemonic
     * @param word_count 12 or 24 words
     * @return BIP39 mnemonic phrase
     */
    static std::string GenerateMnemonic(int word_count = 24);

    /**
     * Load existing wallet
     * @param wallet_name Wallet identifier
     * @param data_dir Directory containing wallet.db
     * @return Unique pointer to wallet instance
     */
    static std::unique_ptr<ReferenceWallet> Load(
        const std::string& wallet_name,
        const std::string& data_dir = "."
    );

    ~ReferenceWallet();

    // Disable copy/move (wallet state is tied to database)
    ReferenceWallet(const ReferenceWallet&) = delete;
    ReferenceWallet& operator=(const ReferenceWallet&) = delete;

    /**
     * Get wallet's single address
     * @return Bech32 address (din1q...)
     */
    std::string GetAddress() const;

    /**
     * Get balance breakdown
     * @param min_confirmations Minimum confirmations for confirmed balance
     * @return Balance structure
     */
    Balance GetBalance(uint32_t min_confirmations = 1) const;

    /**
     * List all unspent outputs (sorted deterministically)
     * @param min_confirmations Minimum confirmations
     * @return Sorted vector of UTXOs
     */
    std::vector<UTXO> ListUnspent(uint32_t min_confirmations = 1) const;

    /**
     * Send transaction
     * @param to_address Recipient address
     * @param amount Amount to send (una)
     * @param fee Explicit fee (una)
     * @return Transaction structure with txid and hex
     * @throws std::runtime_error if insufficient funds or invalid params
     */
    Transaction SendToAddress(
        const std::string& to_address,
        uint64_t amount,
        uint64_t fee
    );

    /**
     * Get transaction details
     * @param txid Transaction ID
     * @return Transaction structure
     * @throws std::runtime_error if txid not found
     */
    Transaction GetTransaction(const std::string& txid) const;

    /**
     * List wallet transactions
     * @param limit Maximum number of transactions (0 = all)
     * @param offset Skip first N transactions
     * @return Vector of transactions (newest first)
     */
    std::vector<Transaction> ListTransactions(
        uint32_t limit = 100,
        uint32_t offset = 0
    ) const;

    /**
     * Process new block
     * Called by the blockchain sync engine when new blocks arrive
     * @param block_height Block height
     * @param block_hash Block hash
     * @param transactions Transactions in block (hex)
     */
    void ProcessBlock(
        uint32_t block_height,
        const std::string& block_hash,
        const std::vector<std::string>& transactions
    );

    /**
     * Process new mempool transaction
     * @param tx_hex Transaction hex
     */
    void ProcessMempoolTransaction(const std::string& tx_hex);

    /**
     * Get wallet metadata
     */
    struct WalletInfo {
        std::string name;
        std::string address;
        int64_t creation_time;
        uint32_t last_block_height;
        std::string last_block_hash;
    };

    WalletInfo GetWalletInfo() const;

    /**
     * Export private key (WIF format)
     * WARNING: Only for backup/recovery
     * @return Private key in WIF format
     */
    std::string DumpPrivateKey() const;

    /**
     * Validate address format
     * @param address Address to validate
     * @return true if valid bech32 Dinero address
     */
    static bool ValidateAddress(const std::string& address);

    // ===== Manual UTXO Injection (for testing) =====

    /**
     * Add UTXO manually (for testing without blockchain sync)
     * @param utxo UTXO to add
     */
    void AddUTXO(const UTXO& utxo);

    /**
     * Remove UTXO manually (mark as spent)
     * @param txid Transaction ID
     * @param vout Output index
     * @param spent_in_txid Transaction that spent this UTXO
     * @param spent_at_height Block height where spent
     */
    void RemoveUTXO(
        const std::string& txid,
        uint32_t vout,
        const std::string& spent_in_txid,
        uint32_t spent_at_height
    );

    /**
     * Select UTXOs for transaction (for testing)
     * @param amount Amount needed (excluding fee)
     * @param fee Transaction fee
     * @param current_height Current blockchain height
     * @return Selected UTXOs
     * @throws std::runtime_error if insufficient funds
     */
    std::vector<UTXO> SelectUTXOsForTransaction(
        uint64_t amount,
        uint64_t fee,
        uint32_t current_height
    ) const;

    /**
     * Set current blockchain height (for testing)
     * @param height Current blockchain height
     */
    void SetCurrentHeight(uint32_t height);

    // ===== Blockchain Synchronization =====

    /**
     * Synchronize wallet with blockchain
     * @param rpc_url RPC endpoint URL (e.g., "http://127.0.0.1:8332")
     * @param rpc_user RPC username
     * @param rpc_password RPC password
     * @param start_height Height to start scanning from (0 = from beginning)
     * @param max_blocks Maximum blocks to scan (0 = scan to tip)
     * @return true if sync completed successfully
     */
    bool SyncBlockchain(
        const std::string& rpc_url,
        const std::string& rpc_user,
        const std::string& rpc_password,
        uint32_t start_height = 0,
        uint32_t max_blocks = 0
    );

private:
    // Private constructor (use factory methods)
    ReferenceWallet(
        const std::string& wallet_name,
        const std::string& data_dir
    );

    // Initialize database schema
    void InitializeDatabase();

    // Derive address from seed
    void DeriveKeysFromSeed(
        const std::string& mnemonic,
        const std::string& passphrase
    );

    // Member variables
    std::string wallet_name_;
    std::string data_dir_;
    std::string address_;
    std::vector<uint8_t> private_key_;
    std::vector<uint8_t> public_key_;

    std::unique_ptr<Database> database_;
    std::unique_ptr<UTXOManager> utxo_manager_;
    std::unique_ptr<TransactionBuilder> tx_builder_;
};

} // namespace reference
} // namespace wallet
} // namespace dinero

#endif // DINEROCOIN_WALLET_REFERENCE_WALLET_H
