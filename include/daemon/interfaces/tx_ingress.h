#pragma once

#include "daemon/interfaces/origin.h"
#include "daemon/interfaces/ingress_types.h"  // TxAcceptResult, TxRejectCode (no impl headers)
#include "primitives/transaction.h"
#include <memory>
#include <vector>

namespace dinero {

/**
 * Step 5: Canonical Transaction Ingress Interface
 *
 * This is the ONLY way external components should submit transactions.
 *
 * ┌─────────────────────────────────────────────────────────────────────────┐
 * │  HEADER ISOLATION RULES (Step 5 / Phase 5)                             │
 * ├─────────────────────────────────────────────────────────────────────────┤
 * │  USE THIS INTERFACE:                                                   │
 * │    • Transaction submission (from RPC, gRPC, P2P, wallet)              │
 * │    • Checking if tx exists before submission (duplicate detection)     │
 * │                                                                        │
 * │  USE DIRECT MEMPOOL ACCESS:                                            │
 * │    • Mempool statistics (getStats, size, etc.)                         │
 * │    • Query operations (getMempoolEntry, getTransactionIds)             │
 * │    • Fee estimation                                                    │
 * │    • Serving mempool txs to peers (getdata handler)                    │
 * │                                                                        │
 * │  RATIONALE:                                                            │
 * │    ITxIngress encapsulates the INGRESS path with:                      │
 * │    - Source attribution (TxOrigin)                                     │
 * │    - Relay policy (auto-relay for RPC/gRPC/WALLET, manual for P2P)     │
 * │    - Structured rejection reporting                                    │
 * │                                                                        │
 * │    Query operations don't need this - they're read-only.               │
 * └─────────────────────────────────────────────────────────────────────────┘
 *
 * Implementors:
 *   - MempoolService (production)
 *   - MockTxIngress (testing)
 *
 * Consumers:
 *   - RPC handlers (sendrawtransaction, etc.)
 *   - gRPC handlers
 *   - P2P message handlers (tx ingress)
 *   - Wallet (for broadcasting)
 */
struct ITxIngress {
    virtual ~ITxIngress() = default;

    /**
     * Submit a transaction for validation and mempool acceptance.
     *
     * @param tx The transaction to submit
     * @param origin Where this transaction came from (for logging/policy)
     * @return Structured result with accept/reject code and reason
     */
    virtual TxAcceptResult Submit(const Transaction& tx, TxOrigin origin) = 0;

    /**
     * Check if a transaction is in the mempool.
     *
     * @param txid Transaction ID to check
     * @return true if transaction is in mempool
     */
    virtual bool HasTransaction(const uint256& txid) const = 0;

    /**
     * Get a transaction from the mempool.
     *
     * @param txid Transaction ID to retrieve
     * @return Transaction if found, nullptr otherwise
     */
    virtual std::shared_ptr<Transaction> GetTransaction(const uint256& txid) const = 0;
};

/**
 * Interface for block template transaction selection.
 *
 * Separated from ITxIngress because:
 *   - Mining needs to READ transactions (not submit)
 *   - Different access pattern (bulk selection vs single submission)
 *   - Can have different implementations (fee-sorted, priority, etc.)
 *
 * Implementors:
 *   - MempoolService (production)
 *   - MockBlockTemplateSource (testing)
 *
 * Consumers:
 *   - BlockTemplateBuilder (mining)
 */
struct IBlockTemplateSource {
    virtual ~IBlockTemplateSource() = default;

    /**
     * Select transactions for inclusion in a block template.
     *
     * @param max_block_size Maximum block size in bytes
     * @param max_block_weight Maximum block weight
     * @param next_block_height Height at which the produced template will land.
     *        When non-zero and at-or-above the v5 freeze-fork activation height
     *        for the active chain, transactions violating any freeze-fork gate
     *        are filtered out. When zero, no freeze-fork filter is applied.
     * @return Vector of transactions sorted by fee rate (highest first)
     */
    virtual std::vector<Transaction> SelectTransactionsForBlock(
        size_t max_block_size = 1000000,
        uint64_t max_block_weight = 4000000,
        uint32_t next_block_height = 0
    ) const = 0;
};

} // namespace dinero
