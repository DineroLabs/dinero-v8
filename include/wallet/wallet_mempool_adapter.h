#pragma once

/**
 * @file wallet_mempool_adapter.h
 * @brief Milestone 12.6 - Wallet ↔ Mempool Submission Adapter
 *
 * Single responsibility: Connect wallet transaction flow to mempool.
 *
 * Design Rule:
 * - Wallet never touches mempool internals
 * - Mempool never touches wallet keys/signing
 * - Adapter is the ONLY bridge between them
 *
 * This enables:
 * - Policy dry-run before signing (avoid wasted crypto work)
 * - Clean separation of concerns
 * - Testable with mock mempools
 * - Future protocol changes don't break wallet
 */

#include "wallet/mempool_interface.h"
#include "wallet/unsigned_tx_builder.h"
#include "wallet/transaction_signer.h"

namespace dinero {
namespace wallet {

/**
 * @brief Adapter between wallet and mempool
 *
 * Milestone 12.6: Wire wallet to mempool without architectural leakage.
 *
 * Responsibilities:
 * - Test unsigned transactions against mempool policy
 * - Submit signed transactions to mempool
 * - Translate wallet types to mempool types
 *
 * Does NOT:
 * - Build transactions (see UnsignedTxBuilder)
 * - Sign transactions (see TransactionSigner)
 * - Enforce policy (mempool's job)
 * - Calculate fees (already done in builder)
 */
class WalletMempoolAdapter {
public:
    /**
     * @brief Construct adapter with mempool interface
     *
     * @param mempool Mempool interface implementation
     */
    explicit WalletMempoolAdapter(IMempoolInterface& mempool);

    /**
     * @brief Test unsigned transaction against mempool policy (dry-run)
     *
     * Use this BEFORE signing to check if transaction would be accepted.
     * Avoids wasted cryptographic work if policy would reject.
     *
     * @param unsigned_tx Unsigned transaction from UnsignedTxBuilder
     * @return Policy validation result with diagnostics
     */
    TxPolicyResult test(const UnsignedTransaction& unsigned_tx) const;

    /**
     * @brief Submit signed transaction to mempool
     *
     * Broadcasts transaction to network if accepted.
     *
     * @param signed_tx Signed transaction from TransactionSigner
     * @return Submission result with acceptance status
     */
    SubmitResult submit(const SignedTransaction& signed_tx);

    /**
     * @brief Get current mempool information
     *
     * @return Mempool statistics and configuration
     */
    MempoolInfo getMempoolInfo() const;

    /**
     * @brief Check if transaction exists in mempool
     *
     * @param txid Transaction ID
     * @return true if tx is in mempool
     */
    bool hasTransaction(const std::string& txid) const;

private:
    IMempoolInterface& mempool_;
};

} // namespace wallet
} // namespace dinero
