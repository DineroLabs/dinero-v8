#pragma once

/**
 * Unified Balance Service
 *
 * Single interface for querying wallet balances across all address types:
 * - Transparent (BIP84 SegWit)
 * - Confidential (CT)
 * - Combined totals
 *
 * Provides real-time balance updates via callbacks and handles the complexity
 * of different UTXO types, confirmation states, and coinbase maturity.
 *
 * Usage:
 *   UnifiedBalanceService balance_service(wallet);
 *   auto balance = balance_service.GetBalance();
 *   std::cout << "Total: " << balance.total_spendable << " sat" << std::endl;
 *   std::cout << "Transparent: " << balance.transparent_confirmed << std::endl;
 *   std::cout << "Confidential: " << balance.confidential_confirmed << std::endl;
 */

#include <cstdint>
#include <string>
#include <vector>
#include <functional>
#include <memory>
#include <mutex>
#include <atomic>

namespace dinero {

// Forward declarations
class WalletManager;
class ChainDB;
class Mempool;

namespace wallet {

/**
 * Unified balance breakdown
 */
struct UnifiedBalance {
    // Total spendable (all confirmed, mature UTXOs)
    uint64_t total_spendable = 0;

    // Transparent balances
    uint64_t transparent_confirmed = 0;
    uint64_t transparent_unconfirmed = 0;
    uint64_t transparent_immature = 0;  // Coinbase < 100 confirmations
    int transparent_utxo_count = 0;

    // Confidential balances
    uint64_t confidential_confirmed = 0;
    uint64_t confidential_unconfirmed = 0;
    uint64_t confidential_immature = 0;
    int confidential_utxo_count = 0;

    // Pending (in mempool, not confirmed)
    uint64_t pending_incoming = 0;
    uint64_t pending_outgoing = 0;

    // Locked UTXOs (reserved for pending transactions)
    uint64_t locked_balance = 0;
    int locked_utxo_count = 0;

    // Computed totals
    uint64_t TotalConfirmed() const {
        return transparent_confirmed + confidential_confirmed;
    }

    uint64_t TotalUnconfirmed() const {
        return transparent_unconfirmed + confidential_unconfirmed;
    }

    uint64_t TotalImmature() const {
        return transparent_immature + confidential_immature;
    }

    int TotalUTXOCount() const {
        return transparent_utxo_count + confidential_utxo_count;
    }

    // Grand total (including unconfirmed)
    uint64_t GrandTotal() const {
        return TotalConfirmed() + TotalUnconfirmed();
    }
};

/**
 * Balance for a specific address
 */
struct AddressBalance {
    std::string address;
    uint64_t confirmed = 0;
    uint64_t unconfirmed = 0;
    int utxo_count = 0;
    bool is_confidential = false;

    uint64_t Total() const { return confirmed + unconfirmed; }
};

/**
 * Balance change event
 */
struct BalanceChangeEvent {
    UnifiedBalance previous;
    UnifiedBalance current;

    // Change amounts
    int64_t transparent_change = 0;
    int64_t confidential_change = 0;

    // Trigger info
    std::string trigger;  // "new_block", "mempool_tx", "reorg", etc.
    uint32_t block_height = 0;
    std::string txid;     // If triggered by specific transaction
};

/**
 * Balance change callback type
 */
using BalanceChangeCallback = std::function<void(const BalanceChangeEvent&)>;

/**
 * Unified Balance Service
 *
 * Provides a unified view of wallet balances across transparent and
 * confidential UTXOs. Handles real-time updates and notifications.
 */
class UnifiedBalanceService {
public:
    explicit UnifiedBalanceService(WalletManager* wallet_manager);
    ~UnifiedBalanceService();

    // Non-copyable
    UnifiedBalanceService(const UnifiedBalanceService&) = delete;
    UnifiedBalanceService& operator=(const UnifiedBalanceService&) = delete;

    // ========================================================================
    // Balance Queries
    // ========================================================================

    /**
     * Get current unified balance
     *
     * @param min_confirmations Minimum confirmations (default: 1)
     * @return Unified balance breakdown
     */
    UnifiedBalance GetBalance(int min_confirmations = 1) const;

    /**
     * Get balance for a specific address
     *
     * @param address Address to query
     * @return Balance for the address
     */
    AddressBalance GetAddressBalance(const std::string& address) const;

    /**
     * Get balances for all addresses with non-zero balance
     *
     * @return Vector of address balances
     */
    std::vector<AddressBalance> GetAllAddressBalances() const;

    /**
     * Get transparent-only balance
     *
     * @param min_confirmations Minimum confirmations
     * @return Transparent balance
     */
    uint64_t GetTransparentBalance(int min_confirmations = 1) const;

    /**
     * Get confidential-only balance
     *
     * @param min_confirmations Minimum confirmations
     * @return Confidential balance
     */
    uint64_t GetConfidentialBalance(int min_confirmations = 1) const;

    /**
     * Get spendable balance (excluding immature and locked)
     *
     * @return Spendable balance
     */
    uint64_t GetSpendableBalance() const;

    // ========================================================================
    // UTXO Information
    // ========================================================================

    /**
     * UTXO details for display
     */
    struct UTXOInfo {
        std::string txid;
        uint32_t vout;
        uint64_t amount;
        int confirmations;
        bool is_confidential;
        bool is_coinbase;
        bool is_locked;
        std::string address;
    };

    /**
     * Get all spendable UTXOs
     *
     * @param include_confidential Include CT UTXOs
     * @param min_confirmations Minimum confirmations
     * @return Vector of UTXO details
     */
    std::vector<UTXOInfo> GetSpendableUTXOs(
        bool include_confidential = true,
        int min_confirmations = 1
    ) const;

    /**
     * Get UTXO count breakdown
     */
    struct UTXOCounts {
        int transparent_spendable = 0;
        int transparent_immature = 0;
        int confidential_spendable = 0;
        int confidential_immature = 0;
        int locked = 0;
    };
    UTXOCounts GetUTXOCounts() const;

    // ========================================================================
    // Balance Notifications
    // ========================================================================

    /**
     * Subscribe to balance changes
     *
     * @param callback Function to call when balance changes
     * @return Subscription ID for unsubscribing
     */
    uint64_t OnBalanceChange(BalanceChangeCallback callback);

    /**
     * Unsubscribe from balance changes
     *
     * @param subscription_id ID returned from OnBalanceChange
     */
    void Unsubscribe(uint64_t subscription_id);

    /**
     * Force refresh balance (recompute from UTXO set)
     */
    void Refresh();

    // ========================================================================
    // Chain State Integration
    // ========================================================================

    /**
     * Notify of new block (updates balance)
     *
     * @param block_height New block height
     * @param block_hash Block hash
     */
    void OnNewBlock(uint32_t block_height, const std::string& block_hash);

    /**
     * Notify of mempool transaction affecting wallet
     *
     * @param txid Transaction ID
     * @param is_incoming True if receiving funds
     */
    void OnMempoolTx(const std::string& txid, bool is_incoming);

    /**
     * Notify of reorg
     *
     * @param old_height Height before reorg
     * @param new_height Height after reorg
     */
    void OnReorg(uint32_t old_height, uint32_t new_height);

    // ========================================================================
    // Configuration
    // ========================================================================

    /**
     * Set minimum confirmations for "confirmed" balance
     */
    void SetMinConfirmations(int min_conf) { min_confirmations_ = min_conf; }

    /**
     * Set coinbase maturity (default: 100)
     */
    void SetCoinbaseMaturity(int maturity) { coinbase_maturity_ = maturity; }

private:
    void NotifyBalanceChange(const std::string& trigger,
                              uint32_t height = 0,
                              const std::string& txid = "");

    void UpdateCache();

    WalletManager* wallet_manager_;

    // Cached balance
    mutable std::mutex cache_mutex_;
    mutable UnifiedBalance cached_balance_;
    mutable std::atomic<bool> cache_valid_{false};

    // Notification subscribers
    std::mutex subscribers_mutex_;
    std::vector<std::pair<uint64_t, BalanceChangeCallback>> subscribers_;
    std::atomic<uint64_t> next_subscription_id_{1};

    // Configuration
    int min_confirmations_ = 1;
    int coinbase_maturity_ = 100;
};

} // namespace wallet
} // namespace dinero
