#pragma once
#include <string>
#include <vector>
#include <optional>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#ifdef FFI_WALLET_ONLY
// iOS doesn't support std::filesystem::path - use std::string instead
#else
#include <filesystem>
#endif
#include <map>
#include <set>  // Phase 35: UTXO locking
#include <functional>  // rescanUtxoSet producer/sink
#include "interfaces/wallet_notifier.h"  // Phase 3D: Event-driven wallet updates
#include "wallet/keystore.h"  // Week 1 Day 5: WalletKeyStore interface
#include "wallet/key_identity.h"  // Week 1 Day 5: KeyID type
#include "wallet/key_origin.h"  // Week 1 Day 5: KeyOriginInfo

struct sqlite3; // forward decl

// Forward declarations
class HDWallet;
class UTXOIndex;  // Forward declaration for UTXO indexing

namespace din {
    class DescriptorStore;  // Phase 3C: Descriptor persistence
}

namespace dinero {

// Forward declarations.
// Block and Transaction are defined as `struct` in primitives/block.h and
// primitives/transaction.h. MSVC mangles class/struct differently so this
// must agree with the actual declaration.
struct Block;
struct Transaction;
class WalletManager;
class Mempool;
class ILogger;  // Dependency injection for logging
class UTXOIndex;  // UTXO indexing for address registration
class ChainDB;  // Chain database for UTXO discovery during rescan
class BlockStorage;

namespace lightning {
    class LightningService;  // Forward declaration for Lightning integration
}

// Helper function to access WalletManager for coinbase indexing (used by BlockAcceptor)
// This avoids direct extern dependency on dinero::legacy::g_wallet_manager() unique_ptr
WalletManager* GetWalletManagerForIndexing();

struct AddressRow {
    std::string address;
    std::optional<std::string> label;
    int account = 0;
    int change  = 0;
    int index   = 0;
    bool external = false;  // true for address book entries, false for HD addresses
    std::string type = "p2tr";  // Address type: Taproot-only (BIP86)
    std::string script_pubkey;  // Bitcoin-grade: Use scriptPubKey for ownership, not address strings
};

struct CovenantDescriptorRecord {
    std::string descriptor_id;
    std::string profile;
    std::string descriptor;
    std::vector<uint8_t> script_pubkey;
    std::string label;
    std::string parent_descriptor_id;
    int64_t created_at = 0;
};

struct Bip39RecoveryMaterial {
    std::string mnemonic;
    bool passphrase_required = false;
    bool backup_acknowledged = false;
};

// Week 1 Day 5: WalletManager now implements WalletKeyStore interface
// This enables IsMine logic to query wallet keys for script ownership
class WalletManager : public WalletNotifier, public dinero::wallet::WalletKeyStore {
public:
#ifdef FFI_WALLET_ONLY
    explicit WalletManager(const std::string& dataDir,
                           ILogger* logger = nullptr,
                           const std::string& walletSchemaPath = "");
#else
    explicit WalletManager(const std::filesystem::path& dataDir,
                           ILogger* logger = nullptr,
                           const std::string& walletSchemaPath = "");
#endif
    ~WalletManager() override;

    std::vector<std::string> listWallets() const;
    std::string getMostRecentlyOpenedWallet() const;
    bool exists(const std::string& name) const;

    void create(const std::string& name);
    void createFromBip39(const std::string& name,
                         const std::string& mnemonic,
                         const std::string& bip39_passphrase);
    void open(const std::string& name);
    void unload();
    void rename(const std::string& oldName, const std::string& newName);
    void remove(const std::string& name); // refuses if current or has UTXOs

    std::string current() const { return current_; }
    bool hasActiveWallet() const { return !current_.empty(); }
    bool isLocked() const { return wallet_locked_; }
    std::string getCurrentWalletName() const { return current_; }

    // Labels and Address Book
    // is_system: true for auto-generated labels (e.g., "Coinbase block #123"), won't overwrite user labels
    void setAddressLabel(const std::string& addr, const std::string& label, bool is_system = false);
    std::optional<std::string> getAddressLabel(const std::string& addr) const;
    bool isSystemLabel(const std::string& addr) const;
    std::vector<AddressRow> listAddresses(bool includeLabels = true) const;
    void removeAddress(const std::string& addr);  // removes from address book only
    
    // HD Wallet Address Management
    void addHDAddress(const std::string& addr, int account, int change, int index, const std::string& label = "");
    int getNextAddressIndex(int account = 0, int change = 0) const;
    bool isAddressMine(const std::string& addr) const;
    bool isScriptMine(const std::string& script_pubkey) const;

    // Address generation (Taproot-only; non-taproot requests are ignored/rejected by callers).
    std::string getNewAddress(const std::string& label = "", const std::string& address_type = "taproot");
    std::string getNewChangeAddress(const std::string& label = "", const std::string& address_type = "taproot");

    // HD Wallet integration (inject HDWallet pointer for proper address derivation)
    void setHDWallet(HDWallet* hd_wallet) { hd_wallet_ = hd_wallet; }
    HDWallet* getHDWallet() const { return hd_wallet_; }

    // UTXO Index integration (inject UTXOIndex pointer for address registration)
    void setUTXOIndex(dinero::UTXOIndex* utxo_index) { utxo_index_ = utxo_index; }
    dinero::UTXOIndex* getUTXOIndex() const { return utxo_index_; }

    // Descriptor Store integration (inject DescriptorStore pointer for descriptor-based address generation)
    void setDescriptorStore(din::DescriptorStore* descriptor_store) { descriptor_store_ = descriptor_store; }
    din::DescriptorStore* getDescriptorStore() const { return descriptor_store_; }

    // Lightning integration (inject LightningService pointer for per-wallet Lightning)
    void setLightningService(dinero::lightning::LightningService* lightning_service) { lightning_service_ = lightning_service; }
    dinero::lightning::LightningService* getLightningService() const { return lightning_service_; }

    // Load all existing wallet addresses from watch_scripts table into UTXOIndex
    // Call this after opening a wallet to ensure all addresses are registered
    void LoadAddressesIntoUTXOIndex();

    // Add scriptPubKey to watch_scripts table for blockchain scanning
    // This is the ownership invariant: wallet must watch scripts it owns
    void addWatchScript(const std::vector<uint8_t>& script_pubkey, const std::string& path, bool is_change);

    /**
     * Persist a validated profile-v1 covenant descriptor and its watch script
     * atomically. Descriptors contain no secret key material.
     *
     * The caller must first parse and re-derive the descriptor with
     * wallet::covenant::RecoverCTVPlan/RecoverCCVPlan. This storage boundary
     * deliberately does not duplicate covenant parsing or consensus rules.
     */
    bool storeCovenantDescriptor(const CovenantDescriptorRecord& record);
    std::optional<CovenantDescriptorRecord> getCovenantDescriptor(
        const std::string& descriptor_id) const;
    std::vector<CovenantDescriptorRecord> listCovenantDescriptors() const;

    // Add address to addresses table for HD derivation tracking
    // This is the ownership invariant: wallet must track addresses it derives
    void addAddress(int account, int change, int idx, const std::string& address, const std::string& type);

    // ═══════════════════════════════════════════════════════════════════════
    // Taproot Descriptor Import (BIP341 Compliant)
    // ═══════════════════════════════════════════════════════════════════════

    /**
     * Register a Taproot address with UTXOIndex for UTXO scanning
     *
     * This registers the P2TR scriptPubKey (derived from tweaked output key)
     * with the UTXOIndex so incoming funds are detected during scanning.
     *
     * @param script_pubkey P2TR scriptPubKey: OP_1 PUSH32 <tweaked_output_key>
     * @param derivation_path Human-readable path identifier
     * @param internal_pubkey 32-byte x-only internal pubkey (before TapTweak)
     * @param output_pubkey 32-byte x-only output pubkey (after TapTweak)
     */
    void registerTaprootAddress(const std::vector<uint8_t>& script_pubkey,
                                const std::string& derivation_path,
                                const std::array<uint8_t, 32>& internal_pubkey,
                                const std::array<uint8_t, 32>& output_pubkey);

    /**
     * Register a v7 P2MR (Pay-to-Merkle-Root) scriptPubKey as wallet-owned.
     *
     * Mirrors registerTaprootAddress's two durable effects (minus the
     * Taproot-specific key mapping): live-registers with UTXOIndex so
     * incoming outputs matching this scriptPubKey are indexed as mine,
     * and persists to watch_scripts so LoadAddressesIntoUTXOIndex
     * re-registers it on subsequent wallet opens.
     *
     * Does NOT store any secret material here — PQ seeds live encrypted
     * in v7_p2mr_addresses. This function only records the ownership
     * mapping (scriptPubKey → BIP-44 derivation path).
     */
    void registerP2MRAddress(const std::vector<uint8_t>& script_pubkey,
                             const std::string& derivation_path);

    /**
     * Store Taproot internal private key for signing
     *
     * Stores the internal private key (NOT tweaked) which is used for signing.
     * The wallet uses SignSchnorrWithInternalKey() which applies the tweak
     * internally during signing (handles Y parity correctly).
     *
     * @param address P2TR address (for lookup)
     * @param internal_privkey 32-byte internal private key
     * @param internal_pubkey 32-byte x-only internal pubkey
     * @param output_pubkey 32-byte x-only output pubkey (tweaked)
     * @param label Human-readable label
     */
    void storeTaprootKey(const std::string& address,
                         const std::array<uint8_t, 32>& internal_privkey,
                         const std::array<uint8_t, 32>& internal_pubkey,
                         const std::array<uint8_t, 32>& output_pubkey,
                         const std::string& label);

    // Database access for RPC handlers
    sqlite3* getCurrentDatabase() const;
    
    // Wallet encryption/decryption
    void encryptWallet(const std::string& passphrase);
    void decryptWallet(const std::string& passphrase);
    void changePassphrase(const std::string& oldPassphrase, const std::string& newPassphrase);
    void lockWallet();
    void unlockWallet(const std::string& passphrase, int timeoutSeconds = 0);
    bool isWalletEncrypted() const;
    bool isWalletLocked() const;
    using ShieldedIncomingViewingKey = std::array<uint8_t, 32>;
    std::vector<ShieldedIncomingViewingKey> GetShieldedIncomingViewingKeys() const;

    // ═══════════════════════════════════════════════════════════════
    // V7 post-quantum wallet integration
    // ═══════════════════════════════════════════════════════════════
    // See docs/consensus/V7_WALLET_SCHEMA.md §5b for the sourcing
    // contract. These accessors are the ONLY bridge between the v5
    // unlocked-wallet state and v7 PQ RPC handlers. No secret material
    // leaves this file in any other way.
    //
    // Both methods return nullopt when the wallet is locked.

    /** v7 BIP-32 material: (private_key, chain_code) at a given path. */
    struct V7Bip32Material {
        std::array<uint8_t, 32> private_key;
        std::array<uint8_t, 32> chain_code;
    };

    /**
     * Read the cached 32-byte v7 PQ master key. Loaded during
     * unlockWallet; scrubbed in lockWallet. Never serialized over
     * JSON-RPC — callers must use it in-process only.
     */
    std::optional<std::array<uint8_t, 32>> GetV7PqMasterKey() const;

    /**
     * Walk m/88'/1448'/account'/change/address_index against the
     * unlocked master_seed_ and return the 32-byte private key and
     * 32-byte chain code at that leaf.
     *
     * Intermediate BIP32Deriver state is zeroized by its destructor
     * before this method returns (per BIP32Deriver's own RAII).
     */
    std::optional<V7Bip32Material> DeriveV7Bip32Material(
        uint32_t account, uint32_t change, uint32_t address_index) const;

    /**
     * Path to the per-wallet SQLite file that v7 P2MR addresses live in.
     * Located alongside the v5 wallet DB (dataDir_/wallets/v7_p2mr_<name>
     * .sqlite). Returns empty string when no wallet is open.
     *
     * The file is created on first write by V7P2MRStore::Open; reads
     * against a fresh wallet return an empty list.
     */
    std::string GetV7P2MRStorePath() const;

    // Primary addresses — deterministic from seed, computed lazily on first access
    std::string getPrimaryAddress();
    void derivePrimaryAddresses();

    // Birthday height (chain height at wallet creation, for rescan optimization)
    bool setBirthdayHeight(int height);
    int getBirthdayHeight() const;  // Returns -1 if not set

    // Wallet encryption metadata storage (Phase: Wallet Security)
    /**
     * Store encrypted wallet metadata after wallet creation with password.
     *
     * @param wallet_name Name of the wallet being encrypted
     * @param encrypted_seed_with_tag AES-256-GCM ciphertext + 16-byte authentication tag
     * @param salt 16-byte Argon2id salt
     * @param nonce 12-byte AES-GCM nonce
     * @param argon2_iterations Argon2id iteration count (time cost)
     * @param argon2_memory_kb Argon2id memory cost in KB
     * @param argon2_parallelism Argon2id parallelism factor
     * @param master_fingerprint BIP32 master key fingerprint (first 4 bytes of HASH160(master_pubkey))
     * @return true if stored successfully
     */
    bool storeEncryptedWallet(
        const std::string& wallet_name,
        const std::vector<uint8_t>& encrypted_seed_with_tag,
        const std::vector<uint8_t>& salt,
        const std::vector<uint8_t>& nonce,
        int argon2_iterations,
        int argon2_memory_kb,
        int argon2_parallelism,
        uint32_t master_fingerprint
    );

    /**
     * Store unencrypted wallet metadata after wallet creation without password.
     *
     * @param wallet_name Name of the wallet being stored
     * @param seed Raw master seed (64 bytes)
     * @param master_fingerprint BIP32 master key fingerprint
     * @return true if stored successfully
     */
    bool storeUnencryptedWallet(
        const std::string& wallet_name,
        const std::vector<uint8_t>& seed,
        uint32_t master_fingerprint,
        bool seed_already_stored = false
    );
    
    // Balance calculation from database
    struct Balance {
        double confirmed = 0.0;          // Confirmed and spendable (includes mature coinbase)
        double unconfirmed = 0.0;        // Unconfirmed (in mempool)
        double immature = 0.0;           // Immature coinbase (not yet spendable)
        double total = 0.0;              // Total balance (confirmed + unconfirmed + immature)
        double spendable = 0.0;          // Actually spendable (confirmed only)
        int utxo_count = 0;
        int immature_utxo_count = 0;
    };
    Balance getBalance(const void* mempool_ptr = nullptr) const;
    Balance getAddressBalance(const std::string& address, const void* mempool_ptr = nullptr) const;
    Balance getScriptPubKeyBalance(const std::string& script_pubkey, const void* mempool_ptr = nullptr) const;
    
    // Transaction history queries
    struct TransactionInfo {
        std::string txid;
        std::string address;
        double amount;
        int confirmations;
        std::string category; // "send", "receive", "generate"
        int64_t time;
        std::string label;
        bool is_coinbase;
    };
    std::vector<TransactionInfo> getTransactionHistory(int limit = 100, int offset = 0) const;
    std::vector<TransactionInfo> getAddressHistory(const std::string& address, int limit = 100) const;
    
    // UTXO queries for PSBT creation and spending
    struct WalletUTXO {
        std::string txid;
        uint32_t vout;
        uint64_t amount_una;
        double amount_din;
        std::string address;
        int confirmations;
        uint32_t height;        // Block height where this UTXO was created
        bool spendable;         // Considers both confirmations and coinbase maturity
        bool is_coinbase;
        bool is_mature;         // True if coinbase is mature (or not coinbase)
        std::string label;
        
        // Additional fields for spending
        std::string script_pubkey;
        bool is_spent;
        std::string derivation_path;  // BIP32 path e.g. "m/86'/1448'/0'/0/0"
        bool is_confidential = false; // True if this is a CT (confidential) output

        // Convenience getters for spend RPC handlers
        int64_t getAmount() const { return static_cast<int64_t>(amount_una); }
        int getVout() const { return static_cast<int>(vout); }
    };
    std::vector<WalletUTXO> listUnspentUTXOs(int min_confirmations = 1,
                                             int max_confirmations = 9999999,
                                             const Mempool* mempool = nullptr) const;
    std::vector<WalletUTXO> getUTXOsForAddress(const std::string& address, int min_confirmations = 1) const;

    // Phase 35: UTXO Locking (wallet.lockunspent)
    // Lock/unlock UTXOs to control which coins are used for spending

    /**
     * Lock specific UTXOs (exclude from automatic coin selection)
     * @param txid Transaction ID
     * @param vout Output index
     * @return true if locked successfully
     */
    bool lockUTXO(const std::string& txid, uint32_t vout);

    /**
     * Unlock specific UTXOs (include in automatic coin selection)
     * @param txid Transaction ID
     * @param vout Output index
     * @return true if unlocked successfully
     */
    bool unlockUTXO(const std::string& txid, uint32_t vout);

    /**
     * Check if a UTXO is locked
     * @param txid Transaction ID
     * @param vout Output index
     * @return true if locked
     */
    bool isUTXOLocked(const std::string& txid, uint32_t vout) const;

    /**
     * Get all locked UTXOs
     * @return List of locked UTXOs as "txid:vout" strings
     */
    std::vector<std::string> getLockedUTXOs() const;

    /**
     * Unlock all UTXOs
     * @return Number of UTXOs unlocked
     */
    size_t unlockAllUTXOs();

    /**
     * Get total amount of locked UTXOs in DIN
     * @return Total locked amount
     */
    double getLockedBalance() const;

    // Phase 35.4: Transaction Abandonment (wallet.abandontransaction)
    /**
     * Mark an unconfirmed transaction as abandoned
     * @param txid Transaction ID to abandon
     * @return true if successfully abandoned, false if not found or already confirmed
     */
    bool abandonTransaction(const std::string& txid);

    /**
     * Check if a transaction is abandoned
     * @param txid Transaction ID to check
     * @return true if transaction is marked as abandoned
     */
    bool isTransactionAbandoned(const std::string& txid) const;

    /**
     * Get information about an abandoned transaction (inputs returned, amount, etc.)
     * @param txid Transaction ID
     * @return JSON object with abandonment details
     */
    struct AbandonmentInfo {
        bool success;
        std::string error;
        int inputs_returned;
        double amount_returned;
    };
    AbandonmentInfo getAbandonmentInfo(const std::string& txid) const;

    // Blockchain height access for maturity calculations
    void setBlockchainHeight(uint32_t height) {
        {
            std::lock_guard<std::mutex> lk(height_mu_);
            current_blockchain_height_ = height;
        }
        height_cv_.notify_all();
        updateUTXOMaturity();
        persistTipHeight(height);
        persistScanHeight(height);
    }

    /**
     * Block until the wallet worker has processed blocks up to at least
     * `target_height`, or until `timeout` elapses. Returns the wallet's
     * tip height at the time the wait completes.
     *
     * Use before any RPC that reads wallet UTXOs to close the race
     * between the async block-connect worker and the RPC thread.
     * Without this gate, a UTXO mined in the latest block may not yet
     * be in the wallet's utxos table when listUnspentUTXOs runs.
     */
    uint32_t WaitForHeight(uint32_t target_height,
                           std::chrono::milliseconds timeout = std::chrono::milliseconds(5000)) {
        std::unique_lock<std::mutex> lk(height_mu_);
        height_cv_.wait_for(lk, timeout, [&] {
            return current_blockchain_height_ >= target_height;
        });
        return current_blockchain_height_;
    }

    uint32_t getBlockchainHeight() const {
        std::lock_guard<std::mutex> lk(height_mu_);
        return current_blockchain_height_;
    }

    void persistTipHeight(uint32_t height);
    void persistScanHeight(uint32_t height);
    void loadBlockchainHeight();
    void runHealthCheck(); // Run PRAGMA quick_check at startup
    std::string runIntegrityCheck(); // Run PRAGMA integrity_check for admin RPC
    uint32_t getBlocksUntilMature(uint32_t utxo_height) const; // Calculate blocks until UTXO matures
    void validateSchemaVersion(); // Validate schema version matches compiled version
    void checkFilePermissions(); // Check database file and directory permissions
    uint32_t getCurrentBlockchainHeight() const { return current_blockchain_height_; }
    
    // Address management for transaction scanning
    std::vector<std::string> getWalletAddresses() const;

    /**
     * Get scriptPubKey for a given address (temporary bridge function for migration).
     * ⚠️ TEMPORARY: Use this only during refactor migration. Prefer direct scriptPubKey usage.
     * Queries addresses table to get script_pubkey column by address.
     *
     * @param address The bech32 address to lookup
     * @return Hex-encoded scriptPubKey or std::nullopt if not found
     */
    std::optional<std::string> getScriptPubKeyForAddress(const std::string& address) const;

    // ========================================================================
    // Phase W.2.6: Wallet Scan Status API
    // ========================================================================

    /**
     * @brief Wallet scan progress tracking
     *
     * Tracks how much of the blockchain the wallet has scanned for transactions.
     * Used by sync UX to show accurate wallet sync progress.
     */
    struct WalletScanStatus {
        uint32_t scan_height = 0;      ///< Last block height scanned by wallet
        uint32_t chain_height = 0;     ///< Current blockchain tip height
        bool is_scanning = false;      ///< True if active rescan in progress

        /**
         * @brief Calculate scan progress [0.0, 1.0]
         */
        double progress() const {
            if (chain_height == 0) return 1.0;
            return std::min(1.0, static_cast<double>(scan_height) / chain_height);
        }

        /**
         * @brief Check if wallet is fully caught up
         */
        bool is_synced() const {
            return scan_height >= chain_height && !is_scanning;
        }
    };

    /**
     * @brief Get current wallet scan status
     *
     * Thread-safe, read-only snapshot of wallet scan progress.
     *
     * @param chain_height Current blockchain tip height (optional)
     * @return Current scan status
     */
    WalletScanStatus GetScanStatus(uint32_t chain_height = 0) const;

    // ========================================================================
    // Wallet rescan functionality
    // ========================================================================
    bool rescanBlockchain(int start_height = 0,
                          int gap_limit = 20,
                          dinero::ChainDB* chain_db = nullptr,
                          dinero::BlockStorage* block_storage = nullptr);

    // A single UTXO-set coin to match against this wallet's watch_scripts.
    struct UtxoSetEntry {
        std::string txid_hex;                 // canonical txid hex (consensus convention)
        uint32_t vout = 0;
        uint64_t amount_una = 0;              // value in una
        std::vector<uint8_t> script_pubkey;   // raw scriptPubKey bytes
        uint32_t height = 0;
        bool is_coinbase = false;
    };

    // Rescan a UTXO set (e.g. an AssumeUTXO snapshot's chainstate) for coins
    // owned by this wallet, complementing the block-replay rescan which cannot
    // see pre-snapshot coins (their block bodies are absent).
    //
    // `produce` is invoked with a sink callback; call sink(entry) once per coin
    // in the set. Coins whose scriptPubKey is in watch_scripts are recorded into
    // the local utxos table (idempotent: INSERT OR IGNORE on the PRIMARY KEY, so
    // re-running is safe). If snapshot_height exceeds the wallet's current scan
    // height, the watermark is advanced so a later block-replay rescan starts
    // above the snapshot instead of clearing/refetching pre-snapshot heights.
    //
    // Returns the number of owned UTXOs newly recorded.
    int rescanUtxoSet(
        const std::function<void(const std::function<void(const UtxoSetEntry&)>&)>& produce,
        uint32_t snapshot_height = 0);

    // Transaction recording for real-time updates
    // Phase 36: Added height parameter for reorg handling
    bool addTransaction(const std::string& txid, const std::string& address, double amount,
                       const std::string& category, bool is_coinbase = false,
                       const std::string& label = "", int64_t time = 0, uint32_t height = 0);
    bool confirmTransaction(const std::string& txid, uint32_t height);

    // Phase 36: Remove transactions from orphaned blocks during reorg
    bool removeTransactionsAtHeight(uint32_t height);

    // Settings management for mining address persistence
    void setSetting(const std::string& key, const std::string& value, const std::string& wallet = "", const std::string& network = "");
    std::string getSetting(const std::string& key, const std::string& wallet = "", const std::string& network = "") const;
    bool hasSetting(const std::string& key, const std::string& wallet = "", const std::string& network = "") const;
    void setMiningAddress(const std::string& address, const std::string& wallet, const std::string& network);
    std::string getMiningAddress(const std::string& wallet = "", const std::string& network = "") const;
    
    // UTXO Management for spending (using existing UTXO struct above)

    std::vector<WalletUTXO> getAvailableUTXOs() const;

    // ========================================================================
    // Week 1 Day 5: WalletKeyStore interface implementation
    // Enables IsMine script ownership queries for descriptor wallet
    // ========================================================================

    /**
     * Check if wallet has a key by its KeyID.
     * Implements WalletKeyStore interface.
     *
     * @param key_id KeyID to check (HASH160 of pubkey)
     * @return true if key exists in wallet
     */
    bool HaveKey(const dinero::wallet::KeyID& key_id) const override;

    /**
     * Get key metadata by KeyID.
     * Implements WalletKeyStore interface.
     *
     * @param key_id KeyID to lookup
     * @return WalletKey metadata if found, nullopt otherwise
     */
    std::optional<dinero::wallet::WalletKey> GetKey(const dinero::wallet::KeyID& key_id) const override;

    /**
     * Get key metadata by output_key_id (for Taproot).
     * CRITICAL for Taproot spending: scriptPubKey contains tweaked key,
     * we need to find the internal key via output_key_id lookup.
     * Implements WalletKeyStore interface.
     *
     * @param output_key_id Tweaked output KeyID from scriptPubKey
     * @return WalletKey metadata if found, nullopt otherwise
     */
    std::optional<dinero::wallet::WalletKey> GetKeyByOutputKeyID(const dinero::wallet::KeyID& output_key_id) const override;

    /**
     * Get all keys in wallet.
     * Implements WalletKeyStore interface.
     *
     * @return Vector of all WalletKey metadata
     */
    std::vector<dinero::wallet::WalletKey> GetAllKeys() const override;

    /**
     * Add key metadata to wallet.
     * Implements WalletKeyStore interface.
     *
     * @param key WalletKey metadata to add
     * @return true if added successfully
     */
    bool AddKey(const dinero::wallet::WalletKey& key) override;

    /**
     * Check if wallet has master seed (for spending).
     * Implements WalletKeyStore interface.
     *
     * @return true if master seed available (SPENDABLE wallet)
     */
    bool HaveMasterSeed() const override;

    /**
     * Get master seed for key derivation.
     * Implements WalletKeyStore interface.
     *
     * @return Master seed bytes if available, nullopt if watch-only
     */
    std::optional<std::vector<uint8_t>> GetMasterSeed() const override;

    /**
     * Derive private key on-demand from KeyOriginInfo.
     * This is the CORE of descriptor wallets - enables spending without storing privkeys.
     * Implements WalletKeyStore interface.
     *
     * @param origin KeyOriginInfo specifying derivation path
     * @return Private key bytes if derivation succeeds, nullopt otherwise
     */
    std::optional<std::vector<uint8_t>> DerivePrivateKey(
        const dinero::wallet::KeyOriginInfo& origin) const override;

    // ========================================================================
    // Phase 3D: WalletNotifier interface implementation
    // Event-driven wallet updates from blockchain
    // ========================================================================

    /**
     * @brief Called when a block is connected to the active chain
     *
     * Automatically scans all transactions in the block and updates the wallet's
     * UTXO set for any outputs belonging to wallet addresses. This replaces
     * manual isAddressMine() checks in generatetoaddress and other locations.
     *
     * @param block The block being connected
     * @param height The height of the block in the chain
     */
    void onBlockConnected(const Block& block, uint32_t height) override;

    /**
     * @brief Called when a block is disconnected during a reorg
     * @param block The block being disconnected
     * @param height The height of the block being removed
     */
    void onBlockDisconnected(const Block& block, uint32_t height) override;

    /**
     * @brief Called when a transaction enters the mempool
     * @param tx The transaction entering the mempool
     */
    void onMempoolTransaction(const Transaction& tx) override;
    bool addUTXO(const std::string& txid, int vout, int64_t amount,
                 const std::string& address, const std::string& script_pubkey,
                 int height, bool is_coinbase);
    bool spendUTXO(const std::string& txid, int vout);
    bool removeUTXO(const std::string& txid, int vout);  // Phase 4B: Reorg support
    void updateUTXOMaturity();

    // ═══════════════════════════════════════════════════════════════
    // HD Wallet Private Key Derivation (Phase 1-3)
    // ═══════════════════════════════════════════════════════════════

    /**
     * Derive private key for a given scriptPubKey from HD wallet.
     * This is the main method used by PSBT signing to retrieve keys.
     * ⚠️ OWNERSHIP LOGIC - Uses scriptPubKey (consensus data), NOT address string.
     *
     * @param script_pubkey The hex-encoded scriptPubKey to get the private key for
     * @return Private key bytes (32 bytes) or std::nullopt if not found
     */
    std::optional<std::vector<uint8_t>> deriveKeyForScriptPubKey(const std::string& script_pubkey);

    /**
     * Check whether the wallet has signing material for a scriptPubKey.
     * This is a metadata-only capability check and does not require the wallet
     * to be unlocked.
     */
    bool hasSigningMaterialForScriptPubKey(const std::string& script_pubkey) const;

    /**
     * Get private key for a given derivation path from HD wallet.
     * This is used by WalletTxService for transaction signing.
     *
     * @param derivation_path The BIP32 derivation path (e.g., "m/84'/1448'/0'/0/5")
     * @return Private key as hex string (64 chars), empty on failure
     */
    std::string getPrivateKeyForPath(const std::string& derivation_path);

    /**
     * Get BIP44 derivation path for a given scriptPubKey.
     * Queries address_derivation_paths table by script_pubkey column (consensus data).
     * Uses script ownership metadata rather than address strings.
     *
     * @param script_pubkey The hex-encoded scriptPubKey to lookup
     * @return Derivation path (e.g., "m/84'/1448'/0'/0/0") or std::nullopt if not found
     */
    std::optional<std::string> getDerivationPath(const std::string& script_pubkey) const;

    /**
     * Look up the derivation path for a script from the watch_scripts table.
     * Unlike getDerivationPath() which queries address_derivation_paths by hex text,
     * this queries watch_scripts by raw binary blob — required for covenant outputs
     * registered via addWatchScript() whose path is NOT in address_derivation_paths.
     *
     * @param script_pubkey Raw binary scriptPubKey bytes
     * @return Derivation path or std::nullopt if not found
     */
    std::optional<std::string> getWatchScriptPath(const std::vector<uint8_t>& script_pubkey) const;

    // =========================================================================
    // WIF (Wallet Import Format) Support
    // =========================================================================

    /**
     * Decode a WIF-encoded private key.
     * Supports both mainnet (0x9E prefix) and testnet (0xEF prefix).
     *
     * @param wif The WIF-encoded private key string
     * @return 32-byte private key, or empty vector on failure
     */
    std::vector<uint8_t> decodeWIF(const std::string& wif);

    /**
     * Encode a private key to WIF format.
     *
     * @param privkey 32-byte private key
     * @param compressed Whether to use compressed format (default: true)
     * @param testnet Whether to encode for testnet (default: false)
     * @return WIF-encoded string
     */
    std::string encodeWIF(const std::vector<uint8_t>& privkey, bool compressed = true, bool testnet = false);

    /**
     * Import a private key into the wallet.
     * Generates the corresponding address and adds it to the keystore.
     *
     * @param privkey 32-byte private key
     * @param label Optional label for the address
     * @return The generated address, or empty string on failure
     */
    std::string importPrivateKey(const std::vector<uint8_t>& privkey, const std::string& label = "");

    /**
     * Check if a WIF string is valid and decode info without importing.
     *
     * @param wif The WIF string to validate
     * @param is_compressed Output: whether key is compressed
     * @param is_testnet Output: whether key is for testnet
     * @return true if valid WIF format
     */
    bool validateWIF(const std::string& wif, bool& is_compressed, bool& is_testnet);

    /**
     * Store master seed in database with encryption.
     * Required for wallet.restore to persist the seed.
     * Uses PBKDF2 + AES-256-GCM encryption with user passphrase.
     *
     * @param seed The 512-bit master seed to store
     * @param passphrase User passphrase for encryption (empty = no encryption)
     * @return true if stored successfully
     */
    bool storeMasterSeed(const std::vector<uint8_t>& seed,
                         const std::string& passphrase,
                         bool reset_address_state = true);

    /**
     * Persist the entropy behind an authoritative BIP39 mnemonic.
     *
     * The entropy is AES-GCM sealed under a domain-separated key derived from
     * the active WalletManager seed. The supplied mnemonic/passphrase must
     * reproduce that exact seed before anything is written. Raw-seed legacy
     * wallets therefore cannot acquire a fabricated mnemonic.
     */
    bool storeAuthoritativeBip39Mnemonic(const std::string& mnemonic,
                                         const std::string& bip39_passphrase,
                                         std::string* error_out = nullptr);

    /**
     * Load and authenticate the mnemonic bound to the active wallet seed.
     * Encrypted wallets must be unlocked first. Returns nullopt for legacy
     * raw-seed wallets, locked wallets, or corrupt/mismatched records.
     */
    std::optional<Bip39RecoveryMaterial> loadAuthoritativeBip39Mnemonic(
        std::string* error_out = nullptr) const;

    bool hasAuthoritativeBip39Mnemonic() const;

    /**
     * Record that a client re-presented the exact authoritative mnemonic.
     * Passphrase-backed wallets additionally require explicit confirmation
     * that the separate BIP39 passphrase was backed up.
     */
    bool acknowledgeBip39Backup(const std::string& mnemonic,
                                bool passphrase_backed_up,
                                std::string* error_out = nullptr);

private:
    void createWithInitialSeed(const std::string& name,
                               const std::vector<uint8_t>& initial_master_seed,
                               const std::string* authoritative_mnemonic,
                               const std::string& bip39_passphrase);

#ifdef FFI_WALLET_ONLY
    std::string dataDir_;
#else
    std::filesystem::path dataDir_;
#endif
    std::string current_;
    int current_wallet_id_ = -1;

    // ═══════════════════════════════════════════════════════════════
    // Per-Wallet Database Architecture
    // ═══════════════════════════════════════════════════════════════
    sqlite3* db_ = nullptr;               // Current wallet's database (wallet_<name>.db)
    sqlite3* registry_db_ = nullptr;      // Wallet registry database (wallet_registry.db)
    
    // Encryption state
    bool wallet_encrypted_ = false;
    bool wallet_locked_ = true;
    std::string encryption_key_;
    int64_t unlock_timeout_ = 0;
    int64_t unlock_time_ = 0;
    
    // Blockchain state for maturity calculations.
    // Guarded by height_mu_; condition variable height_cv_ is notified by
    // setBlockchainHeight so WaitForHeight can block until the wallet
    // worker has processed a target block.
    mutable std::mutex              height_mu_;
    mutable std::condition_variable height_cv_;
    uint32_t current_blockchain_height_ = 0;

    // ═══════════════════════════════════════════════════════════════
    // Phase 35: UTXO Lock State (wallet.lockunspent)
    // ═══════════════════════════════════════════════════════════════
    // Locked UTXOs (excluded from automatic coin selection)
    // Format: "txid:vout" -> set of locked outpoints
    std::set<std::string> locked_utxos_;

    // Phase 35.4: Abandoned Transactions (wallet.abandontransaction)
    // ═══════════════════════════════════════════════════════════════
    // Abandoned transaction IDs (marked as abandoned in wallet)
    // Inputs from these transactions return to spendable set
    std::set<std::string> abandoned_transactions_;

    // ═══════════════════════════════════════════════════════════════
    // HD Wallet Internal State
    // ═══════════════════════════════════════════════════════════════

    // Logger (dependency injection) - defaults to ProductionLogger if nullptr
    ILogger* logger_ = nullptr;
    std::string wallet_schema_path_override_;

    // Pointer to HDWallet for proper BIP84 address derivation (injected from daemon)
    HDWallet* hd_wallet_ = nullptr;

    // Pointer to UTXOIndex for address registration and UTXO tracking (injected from daemon)
    dinero::UTXOIndex* utxo_index_ = nullptr;

    // Pointer to DescriptorStore for descriptor-based address generation (Phase 3C)
    din::DescriptorStore* descriptor_store_ = nullptr;

    // Pointer to LightningService for per-wallet Lightning initialization (injected from daemon)
    dinero::lightning::LightningService* lightning_service_ = nullptr;

    // Decrypted master seed (512 bits / 64 bytes) - kept in memory when wallet unlocked
    std::vector<uint8_t> master_seed_;

    // Shielded incoming viewing keys are receive/view-only. They are derived
    // after a successful passphrase unlock and intentionally survive both the
    // spend-unlock timeout and wallet.lock so incoming shielded notes can still
    // be detected while spending stays locked.
    std::vector<ShieldedIncomingViewingKey> shielded_incoming_viewing_keys_;

    // ═══════════════════════════════════════════════════════════════
    // V7 post-quantum wallet master key (spec V7_WALLET_SCHEMA.md §5b)
    // ═══════════════════════════════════════════════════════════════
    // 32-byte AEAD key used to seal per-address pq_seed blobs in the
    // v7_p2mr_addresses table. Loaded in unlockWallet, OPENSSL_cleansed
    // in lockWallet. Persisted encrypted under encryption_key_ in the
    // "v7_pq_master_key_encrypted" wallet-settings row.
    std::array<uint8_t, 32> pq_master_key_{};
    bool                     pq_master_key_loaded_ = false;

    // Primary addresses — deterministic from seed, cached on open/unlock
    std::string primary_address_;          // Transparent (Taproot, index 0)

    // Private key cache for performance (address -> private_key)
    // Keys are encrypted in memory using encryption_key_
    std::map<std::string, std::vector<uint8_t>> private_key_cache_;

    // ═══════════════════════════════════════════════════════════════
    // HD Wallet Helper Methods
    // ═══════════════════════════════════════════════════════════════

    /**
     * Load and decrypt master seed from database.
     * Seed is kept in memory (master_seed_) while wallet is unlocked.
     *
     * @param passphrase User passphrase for decryption
     * @return Decrypted master seed or std::nullopt if wrong passphrase or not found
     */
    std::optional<std::vector<uint8_t>> loadMasterSeed(const std::string& passphrase);

    std::string resolveWalletSchemaPath() const;

    /**
     * Cache a derived private key in memory for performance.
     * Key is encrypted using wallet encryption_key_.
     *
     * @param address The address this key belongs to
     * @param key The 32-byte private key to cache
     */
    void cachePrivateKey(const std::string& address, const std::vector<uint8_t>& key);

    /**
     * Clear the private key cache (called when wallet is locked).
     */
    void clearPrivateKeyCache();

    void initializeDatabase();
    void ensureWalletIdentityRow();
    void initializeRegistry();
    void close();
    void closeRegistry();
    static std::string sanitize(const std::string& name);
    int getWalletId(const std::string& name) const;

    // ═══════════════════════════════════════════════════════════════
    // Wallet Registry Helper Methods
    // ═══════════════════════════════════════════════════════════════
    /**
     * Register a new wallet in the registry database.
     *
     * @param name Wallet name
     * @param path Path to wallet database file
     * @param network Network (mainnet/testnet/regtest)
     * @param encrypted Whether wallet is encrypted
     * @param fingerprint BIP32 master fingerprint (4 bytes)
     * @return true if registered successfully
     */
    bool registerWalletInRegistry(
        const std::string& name,
        const std::string& path,
        const std::string& network,
        bool encrypted,
        const std::vector<uint8_t>& fingerprint
    );

    /**
     * Get wallet path from registry.
     *
     * @param name Wallet name
     * @return Path to wallet database file or empty string if not found
     */
    std::string getWalletPathFromRegistry(const std::string& name) const;

    /** Repair stale absolute path in registry (iOS container UUID change). */
    void updateWalletPathInRegistry(const std::string& name, const std::string& newPath);

    /**
     * Update last_opened timestamp for a wallet in registry.
     *
     * @param name Wallet name
     */
    void updateLastOpened(const std::string& name);
    
    // Transaction analysis for blockchain scanning
    bool analyzeTransaction(const char* raw_hex, const std::vector<std::string>& wallet_addresses, 
                          TransactionInfo& tx, uint32_t height) const;
    double calculateMiningReward(uint32_t height) const;
    void setCurrentWallet(const std::string& name, int wallet_id);
    
    // Address validation
    bool isValidDineroBech32(const std::string& addr) const;

    // Address extraction from scriptPubKey
    std::string extractAddressFromScript(const std::vector<uint8_t>& scriptPubKey) const;

    // Encryption helpers
    std::string deriveKey(const std::string& passphrase, const std::string& salt) const;
    std::string deriveKeyLegacy(const std::string& passphrase, const std::string& salt) const;
    std::string encryptData(const std::string& data, const std::string& key) const;
    std::string decryptData(const std::string& encryptedData, const std::string& key) const;
    void checkUnlockTimeout();

    // SQLite helpers/migration
    static void exec(sqlite3* db, const char* sql);
    static int  getUserVersion(sqlite3* db);
    static void setUserVersion(sqlite3* db, int v);
    static bool tableExists(sqlite3* db, const char* name);
    static bool columnExists(sqlite3* db, const char* table, const char* col);
    void assertNoRetiredLegacyCoinTypeInWalletDatabase(const std::string& wallet_name) const;
    void migrate(sqlite3* db);
};

} // namespace dinero
