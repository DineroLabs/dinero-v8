#pragma once

#include "daemon/tx_mempool.h"
#include "consensus/transaction_validator.h"
#include "wallet/transaction.h"
#include <memory>

namespace dinero {

// Forward declarations
class TxMempool;
class UTXOView;
struct MemPoolPolicy;

/**
 * Accept transaction to memory pool with full validation
 * This is the main entry point for transaction validation and mempool acceptance
 */
ATMPOutcome AcceptToMemoryPool(
    const Transaction& tx,
    TxMempool& pool,
    const MemPoolPolicy& policy,
    const UTXOView& utxos,
    bool test_accept = false,
    bool bypass_limits = false
);

/**
 * Validate transaction for mempool acceptance
 * Performs comprehensive validation including:
 * - Basic transaction format validation
 * - Input validation (UTXO existence, signatures)
 * - Policy validation (fees, standardness, limits)
 * - Conflict detection
 */
class MempoolValidator {
public:
    explicit MempoolValidator(const MemPoolPolicy& policy);
    
    // Main validation entry point
    ATMPOutcome ValidateTransaction(
        const Transaction& tx,
        const TxMempool& pool,
        const UTXOView& utxos,
        bool test_accept = false
    ) const;
    
    // Individual validation steps
    ATMPOutcome CheckBasicValidation(const Transaction& tx) const;
    ATMPOutcome CheckInputs(const Transaction& tx, const UTXOView& utxos) const;
    ATMPOutcome CheckFees(const Transaction& tx, const UTXOView& utxos) const;
    ATMPOutcome CheckPolicy(const Transaction& tx, const TxMempool& pool) const;
    ATMPOutcome CheckConflicts(const Transaction& tx, const TxMempool& pool) const;
    ATMPOutcome CheckPackageLimits(const Transaction& tx, const TxMempool& pool) const;
    ATMPOutcome CheckRBF(const Transaction& tx, const TxMempool& pool) const;

    // Policy configuration
    void UpdatePolicy(const MemPoolPolicy& policy);
    const MemPoolPolicy& GetPolicy() const { return policy_; }
private:
    MemPoolPolicy policy_;
    std::unique_ptr<consensus::TransactionValidator> tx_validator_;
    
    // Helper methods
    bool IsStandardTransaction(const Transaction& tx) const;
    bool CheckDustOutputs(const Transaction& tx) const;
    uint64_t CalculateMinimumFee(const Transaction& tx) const;
    std::vector<std::string> GetConflictingTransactions(const Transaction& tx, const TxMempool& pool) const;
};

/**
 * Transaction validation context
 * Provides additional context for validation decisions
 */
struct ValidationContext {
    bool test_accept = false;           // Don't actually add to mempool
    bool bypass_limits = false;         // Bypass package limits (for mining)
    bool check_rbf = true;             // Check RBF rules
    bool require_standard = true;       // Require standard transaction
    std::string peer_id;               // Peer that sent the transaction
    int64_t validation_time = 0;       // Time validation started
    
    ValidationContext() = default;
    explicit ValidationContext(bool test_accept) : test_accept(test_accept) {}
};

/**
 * Validation result with detailed information
 */
struct ValidationResult {
    ATMPResult result;
    std::string reason;
    std::string txid;
    
    // Transaction details
    uint64_t size = 0;
    uint64_t vsize = 0;
    uint64_t weight = 0;
    uint64_t fee = 0;
    double feerate = 0.0;
    
    // Validation timing
    int64_t validation_time_ms = 0;
    
    // Conflicts and dependencies
    std::vector<std::string> conflicts;
    std::vector<std::string> missing_inputs;
    
    // Package information
    uint64_t ancestor_count = 0;
    uint64_t descendant_count = 0;
    
    bool IsAccepted() const { return result == ATMPResult::Accepted; }
    bool IsRejected() const { return result != ATMPResult::Accepted; }
};

/**
 * Batch validation for multiple transactions
 * Useful for block validation and initial mempool sync
 */
class BatchValidator {
public:
    explicit BatchValidator(TxMempool& pool, const UTXOView& utxos, const MemPoolPolicy& policy);
    
    // Validate multiple transactions in dependency order
    std::vector<ValidationResult> ValidateBatch(const std::vector<Transaction>& txs);
    
    // Add single transaction to batch
    void AddTransaction(const Transaction& tx);
    
    // Process accumulated batch
    std::vector<ValidationResult> ProcessBatch();
    
    // Clear batch
    void Clear();
    
private:
    TxMempool& pool_;
    const UTXOView& utxos_;
    MempoolValidator validator_;
    std::vector<Transaction> batch_;
    
    // Sort transactions by dependency order
    std::vector<Transaction> SortByDependencies(const std::vector<Transaction>& txs) const;
};

/**
 * Mempool synchronization utilities
 */
namespace mempool_sync {
    
    // Export mempool state for persistence or network sync
    din::Json ExportMempoolState(const TxMempool& pool);
    
    // Import mempool state from persistence or network sync
    std::vector<ValidationResult> ImportMempoolState(
        const din::Json& state, 
        TxMempool& pool, 
        const UTXOView& utxos,
        const MemPoolPolicy& policy
    );
    
    // Get mempool differences for incremental sync
    din::Json GetMempoolDiff(const TxMempool& pool, const std::vector<std::string>& known_txids);
    
} // namespace mempool_sync

} // namespace dinero
