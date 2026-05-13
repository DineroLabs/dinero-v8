#pragma once

// ╔═══════════════════════════════════════════════════════════════════════════╗
// ║                    WALLET OWNERSHIP INVARIANT                              ║
// ╠═══════════════════════════════════════════════════════════════════════════╣
// ║                                                                           ║
// ║  RULE: A UTXO without a derivation path is NOT owned. No exceptions.      ║
// ║                                                                           ║
// ║  This invariant is CONSTITUTIONAL - it defines ownership itself.          ║
// ║  Ownership is defined exclusively by a valid derivation path proven at    ║
// ║  runtime. UTXOs without derivation paths are treated as non-owned in all  ║
// ║  circumstances, including reorg restoration.                              ║
// ║                                                                           ║
// ║  ENFORCED AT:                                                             ║
// ║    • AddUTXO()         - Rejects UTXOs without valid paths                ║
// ║    • GetUnspentUTXOs() - Excludes pathless UTXOs from balance             ║
// ║    • DisconnectBlock() - Re-evaluates ownership during reorgs             ║
// ║    • SignInput()       - Refuses to sign without derivation proof         ║
// ║                                                                           ║
// ║  PREVENTS:                                                                ║
// ║    • Ghost balances (phantom UTXOs)                                       ║
// ║    • Unspendable outputs                                                  ║
// ║    • Reorg-induced wallet corruption                                      ║
// ║    • Signing without provenance                                           ║
// ║    • Silent consensus ↔ wallet divergence                                 ║
// ║                                                                           ║
// ║  VALID PATHS:                                                             ║
// ║    • "m/86'/1448'/..." - BIP86 Taproot (PRIMARY)                          ║
// ║    • "m/84'/1448'/..." - BIP84 P2WPKH (LEGACY)                            ║
// ║    • "m/77'/1448'/..." - Confidential keys                                ║
// ║    • "genesis", "coinbase", "system" - External/system UTXOs              ║
// ║                                                                           ║
// ║  TEST: test_wallet_ownership_invariant.cpp locks this permanently         ║
// ║                                                                           ║
// ╚═══════════════════════════════════════════════════════════════════════════╝

#include <string>
#include <vector>
#include <map>
#include <optional>
#include <cstdint>
#include <mutex>
#include <functional>
#include <sqlite3.h>
#include "primitives/uint256.h"
#include "primitives/hash_domains.h"  // Phase M.4.3-B Step 2: TxId type
#include "primitives/amount.h"        // Phase M.6.2: Monetary type safety
#include "result.h"
// NOTE: UTXOIndex does NOT implement IUTXOProvider (consensus interface)
// Use WalletUTXOAdapter to bridge wallet to consensus interface

namespace dinero {

struct WalletUTXO {
    TxId txid;  // Phase M.4.3-B Step 2: Semantic type safety (malleability-proof)
    uint32_t vout;
    // Phase M.6.2: SIGN MISMATCH FIX - was int64_t (wrong), now AmountUna (wraps uint64_t)
    AmountUna value;           // una (AmountUna::Zero() for confidential outputs)
    std::vector<uint8_t> spk;   // scriptPubKey bytes
    std::string path;           // "m/84'/1448'/0'/0/12" etc
    int height;
    std::optional<int> spend_height; // nullopt = unspent
    bool is_coinbase = false;   // true if this is a coinbase output (needs 100 block maturity)

    // Phase 11a: Utreexo position tracking for proof generation
    std::optional<uint64_t> utreexo_position;  // Position in Utreexo accumulator (nullopt if not tracked)

    // Zero-Knowledge privacy fields (Phase F)
    bool is_confidential = false;
    std::vector<uint8_t> commitment;       // 33-byte Pedersen commitment (if confidential)
    std::vector<uint8_t> range_proof;      // ~5KB Bulletproof (if confidential)
    std::vector<uint8_t> blinding_factor;  // 32-byte blinding factor (if confidential)
    std::vector<uint8_t> nonce;            // 32-byte nonce (if confidential)

    WalletUTXO() = default;
    // Phase M.6.2: Constructor now takes AmountUna parameter (fixes sign mismatch)
    WalletUTXO(const TxId& txid_, uint32_t vout_, AmountUna value_,
         const std::vector<uint8_t>& spk_, const std::string& path_, int height_, bool coinbase_ = false)
        : txid(txid_), vout(vout_), value(value_), spk(spk_), path(path_), height(height_), is_coinbase(coinbase_) {}

    // Constructor for confidential outputs
    WalletUTXO(const TxId& txid_, uint32_t vout_, AmountUna value_,
         const std::vector<uint8_t>& commitment_, const std::vector<uint8_t>& range_proof_,
         const std::vector<uint8_t>& blinding_factor_, const std::vector<uint8_t>& nonce_,
         int height_)
        : txid(txid_), vout(vout_), value(value_), height(height_), is_confidential(true),
          commitment(commitment_), range_proof(range_proof_), blinding_factor(blinding_factor_), nonce(nonce_) {}
};

// Zero-Knowledge output (Phase F)
struct ZKOutput {
    TxId txid;  // Phase M.4.3-B Step 2: Semantic type safety (malleability-proof)
    uint32_t vout;
    // Phase M.6.2: Type-safe amount
    AmountUna amount;                     // Decrypted amount (from range proof rewind)
    std::vector<uint8_t> commitment;       // 33-byte Pedersen commitment
    std::vector<uint8_t> range_proof;      // ~5KB Bulletproof
    std::vector<uint8_t> blinding_factor;  // 32-byte blinding factor
    std::vector<uint8_t> nonce;            // 32-byte nonce (view key)
    uint32_t block_height;
    uint32_t confirmations;
    bool is_spent = false;

    ZKOutput() : vout(0), amount(AmountUna::Zero()), block_height(0), confirmations(0) {}
};

// Balance breakdown with coinbase maturity tracking
struct BalanceDetail {
    // Phase M.6.2: SIGN MISMATCH FIX - all balances now AmountUna (was int64_t)
    AmountUna confirmed;         // Mature spendable transparent balance
    AmountUna immature;          // Coinbase outputs with < 100 confirmations
    AmountUna total;             // confirmed + immature (transparent only)

    // Zero-Knowledge privacy balances (Phase F)
    AmountUna confidential;      // Confidential (ZK) balance
    AmountUna total_with_conf;   // total + confidential

    BalanceDetail() : confirmed(AmountUna::Zero()), immature(AmountUna::Zero()),
                     total(AmountUna::Zero()), confidential(AmountUna::Zero()),
                     total_with_conf(AmountUna::Zero()) {}
};

// ╔═══════════════════════════════════════════════════════════════════════════╗
// ║  UTXOIndex - Wallet-layer UTXO storage                                     ║
// ╠═══════════════════════════════════════════════════════════════════════════╣
// ║                                                                           ║
// ║  This class is WALLET LAYER - it does NOT implement IUTXOProvider.        ║
// ║  Use WalletUTXOAdapter to bridge wallet to consensus interface.           ║
// ║                                                                           ║
// ║  Direction of trust: Consensus → Wallet                                   ║
// ║    - Wallet is a CONSUMER of consensus state                              ║
// ║    - Wallet can be rebuilt from consensus                                 ║
// ║                                                                           ║
// ╚═══════════════════════════════════════════════════════════════════════════╝
class UTXOIndex {
public:
    explicit UTXOIndex(const std::string& db_path);
    ~UTXOIndex();

    // Database initialization
    bool Initialize();

    // UTXO operations (wallet layer - not consensus interface)
    bool AddUTXO(const WalletUTXO& utxo);
    bool SpendUTXO(const TxId& txid, uint32_t vout, uint32_t height);
    bool DeleteUTXO(const TxId& txid, uint32_t vout);
    bool IsUTXOSpent(const TxId& txid, uint32_t vout) const;

    // Transaction control (for atomic bulk operations like snapshot import)
    // Phase 46: Crash Safety - CRITICAL-002 fix
    bool BeginTransaction();
    bool CommitTransaction();
    bool RollbackTransaction();

    // Database reset (for AssumeUTXO rollback on validation failure)
    // Deletes ALL UTXOs and metadata - use with extreme caution
    bool ClearAll();

    // Metadata storage (for AssumeUTXO state persistence)
    // Phase 46: Crash Safety - CRITICAL-003 fix
    // Stores consensus-critical metadata that must persist across restarts
    // Must be called within a transaction to ensure atomicity with UTXO changes
    bool SetMetadata(const std::string& key, const std::string& value);
    std::optional<std::string> GetMetadata(const std::string& key) const;
    bool DeleteMetadata(const std::string& key);

    // Query functions
    std::vector<WalletUTXO> GetUnspentUTXOs() const;
    std::vector<WalletUTXO> GetUTXOsForAddress(const std::string& address) const;
    std::optional<WalletUTXO> GetUTXO(const TxId& txid, uint32_t vout) const;
    bool GetUTXO(const TxId& txid, uint32_t vout, WalletUTXO& utxo) const;              // Phase M.4.3-B Step 3: Legacy overload (bool + ref style)
    // Phase M.6.2: Type-safe balance queries
    AmountUna GetBalance() const;
    AmountUna GetBalanceForPath(const std::string& path_prefix) const;

    // Phase 44.1: UTXO count for verification (AssumeUTXO)
    Result<uint64_t> GetUTXOCount() const;

    // Maturity-aware balance computation (respects 100-block coinbase maturity)
    BalanceDetail GetBalanceWithMaturity(int current_height) const;

    // Phase 11a: Utreexo position lookup for proof generation
    // Returns the Utreexo accumulator position for a UTXO, or nullopt if not tracked
    std::optional<uint64_t> getUtreexoPosition(const TxId& txid, uint32_t vout) const;

    // Zero-Knowledge privacy queries (Phase F)
    std::vector<WalletUTXO> GetConfidentialUTXOs() const;
    // Phase M.6.2: Type-safe balance queries
    AmountUna GetConfidentialBalance() const;
    AmountUna GetTotalBalance() const;  // transparent + confidential
    bool AddConfidentialUTXO(const ZKOutput& zk_output);
    std::vector<ZKOutput> ScanForNewConfidentialOutputs(int last_scanned_height, int current_height, const std::vector<uint8_t>& view_key);
    
    // Block processing
    void ProcessBlock(int height, const std::vector<std::string>& block_txs);
    void RevertBlock(int height);

    // Priority 3 FIX: Validate wallet UTXOs against consensus
    // Removes phantom UTXOs (wallet thinks unspent, but consensus doesn't have them)
    // Returns count of phantom UTXOs removed
    // The checker function should return true if consensus has the UTXO
    size_t ValidateAgainstConsensus(std::function<bool(const TxId& txid, uint32_t vout)> consensus_has_utxo);

    // Idempotent block scanning (for wallet worker thread)
    // Safe to call multiple times for same block - uses INSERT OR IGNORE
    // Phase M.6.2: Output amounts now use AmountUna in tuple
    void ScanBlockIdempotent(int height, const std::string& block_hash,
                            const std::vector<std::tuple<std::string, // txid
                                                        std::vector<std::pair<std::vector<uint8_t>, AmountUna>>, // outputs
                                                        std::vector<std::pair<std::string, uint32_t>>, // inputs (prevout txid:vout)
                                                        bool>>& transactions); // is_coinbase

    // Address recognition (wallet-specific - determines script ownership)
    std::optional<std::string> IsOurScript(const std::vector<uint8_t>& scriptPubKey) const;

    // CRITICAL: Register wallet addresses/scripts for tracking
    void RegisterAddress(const std::vector<uint8_t>& scriptPubKey, const std::string& derivation_path);
    void ClearRegisteredAddresses();
    
private:
    sqlite3* db_;
    std::string db_path_;
    
    // Helper functions
    bool CreateTables();
    bool PrepareStatements();
    void FinalizeStatements();
    
    // Prepared statements for performance
    sqlite3_stmt* stmt_add_utxo_;
    sqlite3_stmt* stmt_spend_utxo_;
    sqlite3_stmt* stmt_get_unspent_;
    sqlite3_stmt* stmt_get_balance_;
    sqlite3_stmt* stmt_is_spent_;
    sqlite3_stmt* stmt_get_utxo_;       // For GetUTXO(txid, vout)
    sqlite3_stmt* stmt_get_position_;   // Phase 11a: For getUtreexoPosition(txid, vout)
    
    // CRITICAL: Map of scriptPubKey -> derivation path for wallet addresses
    std::map<std::vector<uint8_t>, std::string> watched_scripts_;
    
    // ✅ Fine-grained locking for thread safety
    mutable std::mutex db_mutex_;       // Protects all SQLite operations (not thread-safe)
    mutable std::mutex scripts_mutex_;  // Protects watched_scripts_ map
};

// Transaction processing helpers
class TransactionProcessor {
public:
    // Phase M.6.2: Output amounts now use AmountUna
    static void ProcessTransaction(UTXOIndex& index, const std::string& txid,
                                 const std::vector<std::pair<std::vector<uint8_t>, AmountUna>>& outputs,
                                 const std::vector<std::pair<std::string, uint32_t>>& inputs,
                                 int height);

    static std::vector<uint8_t> ParseScriptPubKey(const std::string& hex);
    static bool IsP2WPKH(const std::vector<uint8_t>& script);
    static bool IsP2TR(const std::vector<uint8_t>& script);    // BIP341 Taproot detection
    static std::vector<uint8_t> ExtractPubKeyHash(const std::vector<uint8_t>& script);
    static std::vector<uint8_t> ExtractTaprootPubkey(const std::vector<uint8_t>& script);  // Extract 32-byte x-only pubkey
};

} // namespace dinero
