#pragma once

// SPDX-License-Identifier: MIT
// Phase W.4.5: Wallet Fee Bump UX Hooks

#include "primitives/uint256.h"
#include "mining/tx_inclusion_analyzer.h"
#include "wallet/fee_bump_engine.h"
#include <vector>
#include <cstdint>

// Forward declarations
namespace dinero {
    class WalletManager;
}
class Mempool;

namespace dinero {

/**
 * @brief Wallet-level fee bump UX hooks
 *
 * Provides convenience methods for wallets to:
 * - Identify transactions needing fee bumps
 * - Get actionable recommendations for stuck transactions
 * - Monitor wallet health regarding unconfirmed transactions
 *
 * This is the UX layer on top of Phase W.4.1-W.4.4 components.
 */
class WalletFeeBumpHooks {
public:
    /**
     * @brief Summary of wallet fee bump status
     */
    struct WalletBumpSummary {
        uint32_t total_unconfirmed = 0;      ///< Total unconfirmed transactions
        uint32_t likely_to_confirm = 0;      ///< Transactions likely to confirm soon
        uint32_t stalled = 0;                ///< Stalled transactions (low priority)
        uint32_t blocked = 0;                ///< Blocked transactions (very unlikely)
        uint32_t can_rbf = 0;                ///< Transactions with RBF available
        uint32_t can_cpfp = 0;               ///< Transactions with CPFP available
        std::vector<uint256> needs_attention; ///< TxIDs that need user attention

        /**
         * @brief Check if wallet has any stuck transactions
         */
        bool has_stuck_transactions() const {
            return (stalled + blocked) > 0;
        }

        /**
         * @brief Check if any fee bump options are available
         */
        bool has_bump_options() const {
            return (can_rbf + can_cpfp) > 0;
        }
    };

    /**
     * @brief Get summary of all wallet transactions needing attention
     *
     * Scans all unconfirmed wallet transactions and categorizes them
     * by inclusion likelihood and fee bump options.
     *
     * @param wallet Wallet to analyze
     * @param mempool Current mempool state (optional)
     * @return Summary of wallet fee bump status
     */
    WalletBumpSummary GetWalletBumpSummary(
        const WalletManager* wallet,
        const Mempool* mempool = nullptr
    ) const;

    /**
     * @brief Get list of transaction IDs that need fee bumping
     *
     * Returns transactions that are stalled or blocked and would
     * benefit from a fee bump to reach target confirmation time.
     *
     * @param wallet Wallet to check
     * @param mempool Current mempool state (optional)
     * @param target_blocks Target confirmation time (default: 1 block)
     * @return Vector of transaction IDs needing bump
     */
    std::vector<uint256> GetTransactionsNeedingBump(
        const WalletManager* wallet,
        const Mempool* mempool = nullptr,
        uint32_t target_blocks = 1
    ) const;

    /**
     * @brief Check inclusion status for a specific wallet transaction
     *
     * Wrapper around TxInclusionAnalyzer that adds wallet-specific
     * context (RBF/CPFP availability).
     *
     * @param txid Transaction ID to check
     * @param wallet Wallet owning the transaction
     * @param mempool Current mempool state (optional)
     * @return Detailed inclusion status
     */
    TxInclusionStatus CheckTransactionStatus(
        const uint256& txid,
        const WalletManager* wallet,
        const Mempool* mempool = nullptr
    ) const;

    /**
     * @brief Get fee bump recommendation for a wallet transaction
     *
     * Wrapper around FeeBumpEngine that provides actionable
     * recommendation for bumping transaction fee.
     *
     * @param txid Transaction ID to analyze
     * @param wallet Wallet owning the transaction
     * @param mempool Current mempool state (optional)
     * @param target_blocks Target confirmation time (default: 1 block)
     * @return Fee bump recommendation
     */
    FeeBumpRecommendation GetBumpRecommendation(
        const uint256& txid,
        const WalletManager* wallet,
        const Mempool* mempool = nullptr,
        uint32_t target_blocks = 1
    ) const;

    /**
     * @brief Check if wallet can bump a transaction
     *
     * Quick check whether RBF or CPFP is available for this transaction.
     *
     * @param txid Transaction ID to check
     * @param wallet Wallet owning the transaction
     * @param mempool Current mempool state (optional)
     * @return true if RBF or CPFP available
     */
    bool CanBumpTransaction(
        const uint256& txid,
        const WalletManager* wallet,
        const Mempool* mempool = nullptr
    ) const;

    /**
     * @brief Check if a transaction needs immediate attention
     *
     * Returns true if transaction is stalled/blocked and would benefit
     * from a fee bump to confirm within target blocks.
     *
     * @param txid Transaction ID to check
     * @param wallet Wallet owning the transaction
     * @param mempool Current mempool state (optional)
     * @param target_blocks Target confirmation time (default: 6 blocks)
     * @return true if transaction needs attention
     */
    bool NeedsAttention(
        const uint256& txid,
        const WalletManager* wallet,
        const Mempool* mempool = nullptr,
        uint32_t target_blocks = 6
    ) const;
};

/**
 * @brief Serialize WalletBumpSummary to JSON
 */
std::string WalletBumpSummaryToString(const WalletFeeBumpHooks::WalletBumpSummary& summary);

} // namespace dinero
