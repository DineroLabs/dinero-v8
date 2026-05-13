#pragma once

#include "din_json.h"
#include "wallet/transaction.h"
#include <string>
#include <vector>
#include <unordered_map>
#include <set>
#include <memory>
#include <mutex>
#include <chrono>

namespace dinero {

// Forward declarations
class UTXOView;
struct MemPoolPolicy;

/**
 * Transaction mempool entry with all necessary metadata
 */
struct TxMempoolEntry {
    // Core transaction data
    Transaction tx;
    std::string txid;
    uint64_t size;
    uint64_t vsize;  // Virtual size for segwit
    uint64_t weight;
    
    // Helper methods to get transaction properties
    std::string GetTxId() const { return tx.GetTxId(); }
    uint64_t GetSize() const;
    uint64_t GetVSize() const { return GetSize(); }  // For now, same as size
    uint64_t GetWeight() const { return GetSize() * 4; }  // Non-segwit weight
    
    // Fee information
    uint64_t fee;
    double feerate;  // sat/vB
    
    // Timing and ancestry
    int64_t time;
    uint32_t height;  // Block height when added
    
    // Dependency tracking
    std::set<std::string> depends;     // TXIDs this tx depends on
    std::set<std::string> spentby;     // TXIDs that depend on this tx
    
    // Ancestor/descendant tracking for package limits
    uint64_t ancestor_count;
    uint64_t ancestor_size;
    uint64_t ancestor_fees;
    uint64_t descendant_count;
    uint64_t descendant_size;
    uint64_t descendant_fees;
    
    // Policy flags
    bool rbf_enabled;
    bool is_replacement;
    
    TxMempoolEntry() = default;
    TxMempoolEntry(const Transaction& tx, uint64_t fee, int64_t time);
    
    // Calculate ancestor score for mining priority
    double GetAncestorScore() const;
    double GetModifiedFeeRate() const;
};

/**
 * Mempool policy configuration
 */
struct MemPoolPolicy {
    // Size limits
    uint64_t max_size_mb = 300;           // 300MB default
    uint64_t max_size_bytes = 300 * 1024 * 1024;
    
    // Fee policy
    uint64_t min_relay_feerate = 1000;    // 1000 sat/kB
    uint64_t dust_threshold = 546;        // 546 una
    
    // Package limits
    uint32_t max_ancestors = 25;          // Max ancestor count
    uint32_t max_descendants = 25;        // Max descendant count
    uint64_t max_ancestor_size = 101000;  // Max ancestor size in bytes
    uint64_t max_descendant_size = 101000; // Max descendant size in bytes
    
    // RBF policy
    bool rbf_enabled = true;
    double rbf_fee_increment = 1.0;       // Minimum fee increment for RBF
    
    // DoS protection
    uint32_t max_orphans = 100;           // Max orphan transactions
    uint32_t max_orphan_size = 100000;    // Max size of orphan pool
    
    // Persistence
    bool persist_enabled = false;
    std::string persist_path = "./mempool.db";
    uint32_t persist_interval_sec = 300;  // 5 minutes
};

/**
 * Result of AcceptToMemoryPool operation
 */
enum class ATMPResult {
    Accepted,
    Rejected,
    MissingInputs,
    NonStandard,
    FeeTooLow,
    Conflict,
    Policy,
    AlreadyExists,
    InvalidTransaction,
    ExceedsLimits
};

struct ATMPOutcome {
    ATMPResult result;
    std::string reason;
    std::string txid;
    uint64_t fee = 0;
    double feerate = 0.0;
    
    bool IsAccepted() const { return result == ATMPResult::Accepted; }
    bool IsRejected() const { return result != ATMPResult::Accepted; }
};

/**
 * Core transaction mempool with multi-index support
 */
class TxMempool {
public:
    explicit TxMempool(const MemPoolPolicy& policy = MemPoolPolicy{});
    ~TxMempool() = default;
    
    // Core operations
    bool Exists(const std::string& txid) const;
    bool AddUnchecked(const TxMempoolEntry& entry);
    bool Remove(const std::string& txid, const std::string& reason = "");
    void Clear();
    
    // Retrieval
    const TxMempoolEntry* Get(const std::string& txid) const;
    std::vector<std::string> GetTxIds() const;
    std::vector<TxMempoolEntry> GetEntries() const;
    std::vector<TxMempoolEntry> GetEntriesByFeeRate(bool descending = true) const;
    std::vector<TxMempoolEntry> GetEntriesByAncestorScore(bool descending = true) const;
    
    // Statistics
    size_t Size() const;
    uint64_t Bytes() const;
    uint64_t GetTotalFees() const;
    double GetAverageFeeRate() const;
    
    // Package tracking
    bool UpdateAncestorState(const std::string& txid);
    std::set<std::string> GetAncestors(const std::string& txid) const;
    std::set<std::string> GetDescendants(const std::string& txid) const;
    
    // Policy enforcement
    bool CheckLimits(const TxMempoolEntry& entry) const;
    std::vector<std::string> EvictForSpace(uint64_t needed_bytes);
    
    // Orphan handling
    bool AddOrphan(const Transaction& tx, const std::string& peer_id);
    bool RemoveOrphan(const std::string& txid);
    std::vector<Transaction> GetOrphansForParent(const std::string& parent_txid);
    void LimitOrphans();
    
    // RBF support
    bool IsRBFCandidate(const std::string& txid) const;
    std::vector<std::string> GetRBFConflicts(const Transaction& tx) const;
    
    // Persistence (optional)
    bool SaveToFile(const std::string& filepath) const;
    bool LoadFromFile(const std::string& filepath);
    
    // Configuration
    const MemPoolPolicy& GetPolicy() const { return policy_; }
    void UpdatePolicy(const MemPoolPolicy& policy);
    
    // Metrics and monitoring
    struct Stats {
        uint64_t tx_count = 0;
        uint64_t total_bytes = 0;
        uint64_t total_fees = 0;
        double avg_feerate = 0.0;
        uint64_t orphan_count = 0;
        uint64_t orphan_bytes = 0;
        uint64_t accepts_total = 0;
        uint64_t rejects_total = 0;
        std::unordered_map<std::string, uint64_t> reject_reasons;
        int64_t last_updated = 0;
    };
    
    Stats GetStats() const;
    void IncrementRejects(const std::string& reason);
    
private:
    mutable std::mutex mtx_;
    MemPoolPolicy policy_;
    
    // Main storage: txid -> entry
    std::unordered_map<std::string, TxMempoolEntry> entries_;
    
    // Indexes for efficient retrieval
    std::multiset<std::pair<double, std::string>> feerate_index_;     // feerate -> txid
    std::multiset<std::pair<double, std::string>> ancestor_index_;    // ancestor_score -> txid
    
    // Orphan pool
    std::unordered_map<std::string, Transaction> orphans_;
    std::unordered_map<std::string, std::string> orphan_peers_;  // txid -> peer_id
    
    // Statistics
    mutable Stats stats_;
    
    // Helper methods
    void UpdateIndexes(const TxMempoolEntry& entry, bool add);
    void RecalculateAncestors(const std::string& txid);
    void RecalculateDescendants(const std::string& txid);
    bool CheckPackageLimits(const TxMempoolEntry& entry) const;
    std::vector<std::string> SelectEvictionCandidates(uint64_t target_bytes) const;
};

/**
 * UTXO view interface for mempool validation
 */
class UTXOView {
public:
    virtual ~UTXOView() = default;
    
    // Check if output exists and is unspent
    virtual bool HaveUTXO(const std::string& txid, uint32_t vout) const = 0;
    
    // Get UTXO value and script
    virtual bool GetUTXO(const std::string& txid, uint32_t vout, 
                        uint64_t& value, std::string& script) const = 0;
    
    // Check if transaction exists (for conflict detection)
    virtual bool HaveTransaction(const std::string& txid) const = 0;
    
    // Get current blockchain height
    virtual uint32_t GetHeight() const = 0;
};

/**
 * Combined UTXO view that checks both blockchain and mempool
 */
class CombinedUTXOView : public UTXOView {
public:
    CombinedUTXOView(std::shared_ptr<UTXOView> base, const TxMempool& mempool);
    
    bool HaveUTXO(const std::string& txid, uint32_t vout) const override;
    bool GetUTXO(const std::string& txid, uint32_t vout, 
                 uint64_t& value, std::string& script) const override;
    bool HaveTransaction(const std::string& txid) const override;
    uint32_t GetHeight() const override;
    
private:
    std::shared_ptr<UTXOView> base_view_;
    const TxMempool& mempool_;
};

} // namespace dinero
