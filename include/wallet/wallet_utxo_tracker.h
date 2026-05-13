#pragma once

#include "wallet/wallet_utxo_state.h"  // Milestone 12.2
#include <string>
#include <vector>
#include <optional>
#include <cstdint>
#include <sqlite3.h>
#include <memory>

// Forward declarations
namespace dinero {
namespace consensus {
    class GlobalUTXOSet;  // Forward declaration - will be implemented later
}
}

namespace dinero {
namespace wallet {

/**
 * @file wallet_utxo_tracker.h
 * @brief Per-wallet UTXO tracking (which UTXOs this wallet owns)
 *
 * This is APPLICATION-LEVEL tracking, NOT consensus.
 * Tracks which UTXOs from the global set belong to this wallet.
 *
 * Key differences from GlobalUTXOSet:
 * - GlobalUTXOSet: ALL UTXOs in blockchain (consensus, RocksDB)
 * - WalletUTXOTracker: UTXOs owned by THIS wallet (application, SQLite)
 *
 * Storage location: ~/.dinero/wallets/{wallet_name}/wallet.db
 */

// ═══════════════════════════════════════════════════════════════════════════
// Wallet UTXO Structure (Application-Level)
// ═══════════════════════════════════════════════════════════════════════════

/**
 * @brief Wallet-owned UTXO (with derivation path and metadata)
 *
 * Links to the global UTXO set but adds wallet-specific tracking.
 *
 * Milestone 12.2: Now includes formal state model instead of boolean flags.
 */
struct WalletUTXO {
    std::string txid;                   // Transaction ID
    uint32_t vout;                      // Output index
    std::string derivation_path;        // BIP84/BIP44 path (e.g., "m/84'/1448'/0'/0/12")
    uint64_t cached_amount;             // Cached from global UTXO
    uint32_t cached_height;             // Cached from global UTXO

    // Milestone 12.2: Formal state tracking (replaces is_spent, is_locked bools)
    WalletUTXOState state;              // Current lifecycle state
    std::optional<std::string> spending_txid;  // TXID that spent this (if SPENT_LOCAL)
    uint32_t confirmations;             // Confirmation depth (0 = unconfirmed)
    bool is_coinbase;                   // Maturity rules apply (100 blocks)
    uint32_t ancestor_count;            // Cached from mempool (if unconfirmed)

    // Optional label for user tracking
    std::string label;

    WalletUTXO()
        : vout(0)
        , cached_amount(0)
        , cached_height(0)
        , state(WalletUTXOState::UNCONFIRMED)
        , confirmations(0)
        , is_coinbase(false)
        , ancestor_count(0)
    {}

    WalletUTXO(const std::string& txid_, uint32_t vout_, const std::string& path_,
               uint64_t amount_, uint32_t height_)
        : txid(txid_)
        , vout(vout_)
        , derivation_path(path_)
        , cached_amount(amount_)
        , cached_height(height_)
        , state(WalletUTXOState::CONFIRMED)  // Assume confirmed if height provided
        , confirmations(0)  // Will be computed from chain tip
        , is_coinbase(false)
        , ancestor_count(0)
    {}

    // Milestone 12.2: Helper to check if UTXO is spendable
    bool isSpendable() const {
        // Cannot spend if not in confirmed/unconfirmed state
        if (state != WalletUTXOState::CONFIRMED &&
            state != WalletUTXOState::UNCONFIRMED) {
            return false;
        }

        // Coinbase maturity check (100 confirmations)
        if (is_coinbase && confirmations < 100) {
            return false;
        }

        return true;
    }

    // Milestone 12.2: Helper to check if UTXO is locked
    bool isLocked() const {
        return state == WalletUTXOState::LOCKED;
    }

    // Milestone 12.2: Transition to new state (validated)
    bool setState(WalletUTXOState new_state, std::string& error) {
        if (!WalletUTXOStateMachine::canTransition(state, new_state)) {
            error = WalletUTXOStateMachine::getTransitionError(state, new_state);
            return false;
        }
        state = new_state;
        return true;
    }
};

// ═══════════════════════════════════════════════════════════════════════════
// Balance Details (with coinbase maturity)
// ═══════════════════════════════════════════════════════════════════════════

struct WalletBalance {
    uint64_t confirmed;         // Spendable balance (mature)
    uint64_t immature;          // Coinbase outputs < 100 confirmations
    uint64_t total;             // confirmed + immature
    uint64_t locked;            // Locked UTXOs (coin control)

    WalletBalance() : confirmed(0), immature(0), total(0), locked(0) {}
};

// ═══════════════════════════════════════════════════════════════════════════
// Wallet UTXO Tracker (SQLite-backed, Per-Wallet)
// ═══════════════════════════════════════════════════════════════════════════

/**
 * @brief Per-wallet UTXO tracker using SQLite
 *
 * Tracks which UTXOs from the global set belong to THIS wallet.
 * Works in coordination with GlobalUTXOSet for validation.
 *
 * Thread-safety: Caller must ensure thread safety (one wallet per thread)
 */
class WalletUTXOTracker {
public:
    /**
     * @brief Create wallet UTXO tracker
     *
     * @param db_path Path to wallet SQLite database (e.g., ~/.dinero/wallets/default/wallet.db)
     * @param global_utxo_set Pointer to global UTXO set for validation
     */
    explicit WalletUTXOTracker(const std::string& db_path,
                               consensus::GlobalUTXOSet* global_utxo_set);
    ~WalletUTXOTracker();

    // ───────────────────────────────────────────────────────────────────────
    // Initialization
    // ───────────────────────────────────────────────────────────────────────

    /**
     * @brief Initialize the wallet UTXO tracker
     *
     * Creates tables if needed.
     *
     * @return true if initialization succeeded
     */
    bool initialize();

    /**
     * @brief Close the database
     */
    void close();

    /**
     * @brief Check if database is open
     */
    bool isOpen() const { return db_ != nullptr; }

    // ───────────────────────────────────────────────────────────────────────
    // Wallet UTXO Operations
    // ───────────────────────────────────────────────────────────────────────

    /**
     * @brief Add a UTXO that this wallet owns
     *
     * Called when wallet receives coins.
     *
     * @param utxo Wallet UTXO to track
     * @return true if added successfully
     */
    bool addOwnedUTXO(const WalletUTXO& utxo);

    /**
     * @brief Mark a UTXO as spent by this wallet
     *
     * Called when wallet creates a transaction spending this UTXO.
     *
     * @param txid Transaction ID
     * @param vout Output index
     * @return true if marked spent
     */
    bool markSpent(const std::string& txid, uint32_t vout);

    /**
     * @brief Check if this wallet owns a UTXO
     *
     * @param txid Transaction ID
     * @param vout Output index
     * @return true if wallet owns this UTXO
     */
    bool ownsUTXO(const std::string& txid, uint32_t vout) const;

    /**
     * @brief Get a wallet UTXO
     *
     * @param txid Transaction ID
     * @param vout Output index
     * @return UTXO if found and owned by wallet
     */
    std::optional<WalletUTXO> getWalletUTXO(const std::string& txid, uint32_t vout) const;

    /**
     * @brief Get all unspent UTXOs owned by this wallet
     *
     * @param include_locked Include locked UTXOs (default: false)
     * @return Vector of unspent wallet UTXOs
     */
    std::vector<WalletUTXO> getUnspentUTXOs(bool include_locked = false) const;

    // ───────────────────────────────────────────────────────────────────────
    // Balance Calculation
    // ───────────────────────────────────────────────────────────────────────

    /**
     * @brief Get wallet balance with coinbase maturity
     *
     * Queries global UTXO set to verify UTXOs still exist.
     *
     * @param current_height Current blockchain height (for maturity calculation)
     * @return Balance breakdown
     */
    WalletBalance getBalance(uint32_t current_height) const;

    /**
     * @brief Get spendable UTXOs for transaction creation
     *
     * Returns only confirmed, mature, unlocked UTXOs.
     *
     * @param current_height Current blockchain height
     * @param min_confirmations Minimum confirmations required (default: 1)
     * @return Vector of spendable UTXOs
     */
    std::vector<WalletUTXO> getSpendableUTXOs(uint32_t current_height,
                                               uint32_t min_confirmations = 1) const;

    // ───────────────────────────────────────────────────────────────────────
    // Coin Control (Advanced)
    // ───────────────────────────────────────────────────────────────────────

    /**
     * @brief Lock a UTXO (prevent spending)
     *
     * @param txid Transaction ID
     * @param vout Output index
     * @return true if locked successfully
     */
    bool lockUTXO(const std::string& txid, uint32_t vout);

    /**
     * @brief Unlock a UTXO (allow spending)
     *
     * @param txid Transaction ID
     * @param vout Output index
     * @return true if unlocked successfully
     */
    bool unlockUTXO(const std::string& txid, uint32_t vout);

    /**
     * @brief Check if UTXO is locked
     */
    bool isLocked(const std::string& txid, uint32_t vout) const;

    /**
     * @brief Set label for UTXO
     */
    bool setLabel(const std::string& txid, uint32_t vout, const std::string& label);

    // ───────────────────────────────────────────────────────────────────────
    // Synchronization with Global UTXO Set
    // ───────────────────────────────────────────────────────────────────────

    /**
     * @brief Verify wallet UTXOs against global UTXO set
     *
     * Checks that all wallet UTXOs still exist in global set.
     * Useful for detecting chain reorgs or double spends.
     *
     * @return Number of UTXOs that no longer exist in global set
     */
    uint64_t verifyAgainstGlobalSet() const;

    /**
     * @brief Rescan wallet UTXOs
     *
     * Removes UTXOs that no longer exist in global set.
     *
     * @return Number of UTXOs removed
     */
    uint64_t rescan();

    // ───────────────────────────────────────────────────────────────────────
    // Statistics
    // ───────────────────────────────────────────────────────────────────────

    /**
     * @brief Get total number of UTXOs tracked by wallet
     */
    uint64_t getUTXOCount() const;

    /**
     * @brief Get number of unspent UTXOs
     */
    uint64_t getUnspentCount() const;

private:
    // SQLite database
    sqlite3* db_;
    std::string db_path_;

    // Reference to global UTXO set (for validation)
    consensus::GlobalUTXOSet* global_utxo_set_;

    // Prepared statements for performance
    sqlite3_stmt* stmt_add_utxo_;
    sqlite3_stmt* stmt_mark_spent_;
    sqlite3_stmt* stmt_get_utxo_;
    sqlite3_stmt* stmt_get_unspent_;
    sqlite3_stmt* stmt_owns_utxo_;

    // Helper: Create database schema
    bool createSchema();

    // Helper: Prepare statements
    bool prepareStatements();

    // Helper: Finalize statements
    void finalizeStatements();

    // Helper: Check if UTXO is mature (for coinbase)
    bool isMature(const WalletUTXO& utxo, uint32_t current_height) const;
};

} // namespace wallet
} // namespace dinero
