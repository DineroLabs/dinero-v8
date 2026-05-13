#pragma once

/**
 * @file batch_transaction_builder.h
 * @brief Milestone 12.7 - Transaction Batching (FINAL v0.12.0)
 *
 * Single responsibility: Pay multiple recipients in one transaction.
 *
 * Design Rule (CRITICAL):
 * Batching is NOT a new transaction type.
 * It is just multiple outputs passed into the existing builder.
 * If this requires special-case code → design regression.
 *
 * Benefits of batching:
 * - Lower total fees (one tx instead of N txs)
 * - Better privacy (no linkability via separate txs)
 * - Fewer UTXOs consumed (shared inputs)
 * - Lower mempool pressure (one broadcast)
 */

#include "wallet/unsigned_tx_builder.h"
#include <string>
#include <vector>
#include <cstdint>

namespace dinero {

/**
 * @brief Single payment in a batch transaction
 */
struct BatchPayment {
    std::string address;      // Recipient address (Bech32)
    uint64_t amount;          // Amount to send (una)

    BatchPayment() : amount(0) {}
    BatchPayment(const std::string& addr, uint64_t amt)
        : address(addr), amount(amt) {}
};

/**
 * @brief Transaction batch builder (thin wrapper)
 *
 * Milestone 12.7: Pay multiple recipients in one transaction.
 *
 * This is NOT a new transaction builder - it's just a convenience
 * wrapper that converts batch payments into the existing builder's
 * output format.
 *
 * All the hard work is done by:
 * - UnsignedTxBuilder (fee calculation, change logic)
 * - CoinSelector (UTXO selection for total amount)
 * - TransactionSigner (signing)
 * - WalletMempoolAdapter (policy + submission)
 *
 * This class does exactly one thing: convert BatchPayment[] → TxOutputRequest[]
 */
class BatchTransactionBuilder {
public:
    /**
     * @brief Build batch transaction paying multiple recipients
     *
     * Creates one transaction with multiple payment outputs.
     * Change output added automatically if needed.
     *
     * Fee savings vs individual transactions:
     * - Shared inputs (fewer total inputs)
     * - Shared overhead (one tx header instead of N)
     * - One change output instead of N
     * - Lower average feerate per payment
     *
     * @param selected_utxos UTXOs selected for spending (from CoinSelector)
     * @param payments List of recipients and amounts
     * @param options Build options (fee rate, RBF, change address, etc.)
     * @return Build result with unsigned transaction
     *
     * Example:
     * ```cpp
     * std::vector<BatchPayment> payments = {
     *     {"din1q...", 100000},
     *     {"din1q...", 200000},
     *     {"din1q...", 300000}
     * };
     *
     * // Select coins for total amount (600000 + fees)
     * auto utxos = coin_selector.select(600000 + estimated_fee);
     *
     * // Build batch transaction
     * auto result = BatchTransactionBuilder::buildBatch(utxos, payments, options);
     *
     * // Result: one tx with 3 payment outputs + optional change
     * ```
     */
    static BuildResult buildBatch(
        const std::vector<CanonicalWalletUTXO>& selected_utxos,  // Phase M.3: CanonicalWalletUTXO
        const std::vector<BatchPayment>& payments,
        const BuildOptions& options
    );

    /**
     * @brief Calculate total amount needed for batch payments
     *
     * Helper for coin selection - returns sum of all payment amounts.
     * Does NOT include fees (caller must add estimated fee).
     *
     * @param payments List of batch payments
     * @return Total amount needed (una)
     */
    static uint64_t calculateTotalAmount(const std::vector<BatchPayment>& payments);
};

} // namespace dinero
