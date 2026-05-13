#ifndef DINEROCOIN_WALLET_REFERENCE_UTXO_MANAGER_H
#define DINEROCOIN_WALLET_REFERENCE_UTXO_MANAGER_H

#include "wallet.h"
#include "database.h"
#include <vector>
#include <string>
#include <cstdint>
#include <memory>

namespace dinero {
namespace wallet {
namespace reference {

/**
 * UTXO Manager
 *
 * Responsibilities:
 * 1. Maintain sorted UTXO set (deterministic ordering)
 * 2. Select UTXOs for transactions (lowest-first algorithm)
 * 3. Track spent/unspent state
 * 4. Calculate balances
 *
 * Guarantees:
 * - UTXOs always sorted by (txid, vout) lexicographically
 * - Selection is deterministic (same inputs = same selection)
 * - No randomization or optimization
 */
class UTXOManager {
public:
    explicit UTXOManager(Database* database);
    ~UTXOManager();

    /**
     * Add new UTXO to the set
     * @param utxo UTXO to add
     */
    void AddUTXO(const UTXO& utxo);

    /**
     * Remove UTXO from the set (when spent)
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
     * Get all unspent UTXOs (sorted deterministically)
     * @param min_confirmations Minimum confirmations required
     * @param current_height Current blockchain height
     * @return Sorted vector of UTXOs
     */
    std::vector<UTXO> GetUnspentUTXOs(
        uint32_t min_confirmations,
        uint32_t current_height
    ) const;

    /**
     * Select UTXOs for transaction (deterministic algorithm)
     *
     * Algorithm:
     * 1. Sort UTXOs by (txid, vout)
     * 2. Select from lowest to highest until target reached
     * 3. No randomization, no optimization
     *
     * @param target_amount Amount needed (excluding fee)
     * @param fee Transaction fee
     * @param min_confirmations Minimum confirmations
     * @param current_height Current blockchain height
     * @return Selected UTXOs
     * @throws std::runtime_error if insufficient funds
     */
    std::vector<UTXO> SelectUTXOs(
        uint64_t target_amount,
        uint64_t fee,
        uint32_t min_confirmations,
        uint32_t current_height
    ) const;

    /**
     * Calculate balance
     * @param min_confirmations Minimum confirmations for confirmed balance
     * @param current_height Current blockchain height
     * @return Balance breakdown
     */
    Balance CalculateBalance(
        uint32_t min_confirmations,
        uint32_t current_height
    ) const;

    /**
     * Check if UTXO is spent
     * @param txid Transaction ID
     * @param vout Output index
     * @return true if spent
     */
    bool IsSpent(const std::string& txid, uint32_t vout) const;

    /**
     * Get total number of unspent UTXOs
     * @return Count
     */
    size_t GetUTXOCount() const;

private:
    Database* database_;

    // Helper: Calculate confirmations
    uint32_t GetConfirmations(
        uint32_t utxo_height,
        uint32_t current_height
    ) const {
        if (utxo_height == 0) return 0;  // Unconfirmed
        if (current_height < utxo_height) return 0;  // Future block?
        return current_height - utxo_height + 1;
    }

    // Helper: Check if coinbase is mature
    bool IsCoinbaseMature(
        const UTXO& utxo,
        uint32_t current_height
    ) const {
        if (!utxo.is_coinbase) return true;
        return GetConfirmations(utxo.height, current_height) > 100;  // Needs 101+ confirmations
    }
};

} // namespace reference
} // namespace wallet
} // namespace dinero

#endif // DINEROCOIN_WALLET_REFERENCE_UTXO_MANAGER_H
