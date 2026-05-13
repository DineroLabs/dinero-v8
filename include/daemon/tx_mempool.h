#pragma once

#include "consensus/outpoint.h"
#include "primitives/uint256.h"
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
    uint256 txid;  // Phase M.0: Changed from std::string
    uint64_t size;
    uint64_t vsize;  // Virtual size for segwit
    uint64_t weight;

    // Helper methods to get transaction properties
    // Phase M.4: GetTxid() returns TxId, extract uint256
    uint256 GetTxId() const { return tx.GetTxid().AsUint256(); }
    std::string GetTxIdHex() const { return txid.GetHex(); }  // Phase M.0: RPC boundary only
    uint64_t GetSize() const;
    uint64_t GetVSize() const { return GetSize(); }  // For now, same as size
    uint64_t GetWeight() const { return GetSize() * 4; }  // Non-segwit weight

    // Fee information
    uint64_t fee;
    double feerate;  // sat/vB

    // Timing and ancestry
    int64_t time;
    uint32_t height;  // Block height when added

    // Dependency tracking (Phase M.0: Changed from std::string to uint256)
    std::set<uint256> depends;     // TXIDs this tx depends on
    std::set<uint256> spentby;     // TXIDs that depend on this tx
    
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
    uint32_t orphan_timeout_sec = 1200;   // Orphan timeout (20 minutes)
    uint32_t max_orphan_depth = 10;       // Max depth for orphan chains
    
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
    uint256 txid;  // Phase M.0: Changed from std::string
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
    
    // Core operations (Phase M.0: Changed std::string txid to uint256)
    bool Exists(const uint256& txid) const;
    bool AddUnchecked(const TxMempoolEntry& entry);
    bool Remove(const uint256& txid, const std::string& reason = "");
    void Clear();

    // Retrieval (Phase M.0: Changed std::string txid to uint256)
    const TxMempoolEntry* Get(const uint256& txid) const;
    std::vector<uint256> GetTxIds() const;
    std::vector<TxMempoolEntry> GetEntries() const;
    std::vector<TxMempoolEntry> GetEntriesByFeeRate(bool descending = true) const;
    std::vector<TxMempoolEntry> GetEntriesByAncestorScore(bool descending = true) const;

    // Statistics
    size_t Size() const;
    uint64_t Bytes() const;
    uint64_t GetTotalFees() const;
    double GetAverageFeeRate() const;

    // Package tracking (Phase M.0: Changed std::string txid to uint256)
    bool UpdateAncestorState(const uint256& txid);
    std::set<uint256> GetAncestors(const uint256& txid) const;
    std::set<uint256> GetDescendants(const uint256& txid) const;
    
    // Policy enforcement
    bool CheckLimits(const TxMempoolEntry& entry) const;
    std::vector<uint256> EvictForSpace(uint64_t needed_bytes);  // Phase M.0: Returns uint256

    // Orphan handling (Phase M.0: Changed std::string txid to uint256)
    bool AddOrphan(const Transaction& tx, const std::string& peer_id);
    bool RemoveOrphan(const uint256& txid);
    std::vector<Transaction> GetOrphansForParent(const uint256& parent_txid);
    void LimitOrphans();
    void EvictExpiredOrphans();  // Remove orphans older than timeout
    uint32_t CalculateOrphanDepth(const Transaction& tx) const;  // Calculate depth for new orphan

    // RBF support (Phase M.0: Changed std::string txid to uint256)
    bool IsRBFCandidate(const uint256& txid) const;
    std::vector<uint256> GetRBFConflicts(const Transaction& tx) const;
    
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

    // Main storage: txid -> entry (Phase M.0: Changed key from std::string to uint256)
    std::unordered_map<uint256, TxMempoolEntry> entries_;

    // Indexes for efficient retrieval (Phase M.0: Changed to uint256)
    std::multiset<std::pair<double, uint256>> feerate_index_;     // feerate -> txid
    std::multiset<std::pair<double, uint256>> ancestor_index_;    // ancestor_score -> txid

    // Orphan pool with metadata (Phase M.0: Changed to uint256 keys)
    struct OrphanMeta {
        Transaction tx;
        std::string peer_id;
        int64_t time_added;     // Timestamp when orphan was added
        uint32_t depth;         // Depth in orphan chain (0 = no parent, 1+ = child of orphan)
    };
    std::unordered_map<uint256, OrphanMeta> orphans_;
    std::unordered_map<uint256, std::vector<uint256>> orphan_children_;  // parent_txid -> [child_txids]

    // Statistics
    mutable Stats stats_;

    // Helper methods (Phase M.0: Changed std::string txid to uint256)
    void UpdateIndexes(const TxMempoolEntry& entry, bool add);
    void RecalculateAncestors(const uint256& txid);
    void RecalculateDescendants(const uint256& txid);
    bool CheckPackageLimits(const TxMempoolEntry& entry) const;
    std::vector<uint256> SelectEvictionCandidates(uint64_t target_bytes) const;
};

/**
 * UTXO view interface for mempool validation
 * Phase M.0: Changed std::string txid to uint256
 */
class UTXOView {
public:
    virtual ~UTXOView() = default;

    // Check if output exists and is unspent
    virtual bool HaveUTXO(const uint256& txid, uint32_t vout) const = 0;

    // Get UTXO value and script
    virtual bool GetUTXO(const uint256& txid, uint32_t vout,
                        uint64_t& value, std::string& script) const = 0;

    // Check if transaction exists (for conflict detection)
    virtual bool HaveTransaction(const uint256& txid) const = 0;

    // Get current blockchain height
    virtual uint32_t GetHeight() const = 0;

    // ========================================================================
    // Phase G.1: Confidential Transaction Support
    // ========================================================================

    /**
     * Get confidential UTXO commitment (if confidential)
     *
     * @param txid Transaction ID (Phase M.0: Changed to uint256)
     * @param vout Output index
     * @param commitment Output parameter for commitment (32 bytes)
     * @param is_confidential Output parameter: true if UTXO is confidential
     * @return true if UTXO exists, false otherwise
     *
     * Note: For transparent UTXOs, is_confidential will be false and
     *       commitment will be empty.
     */
    virtual bool GetConfidentialUTXO(
        const uint256& txid,
        uint32_t vout,
        std::vector<uint8_t>& commitment,
        bool& is_confidential
    ) const = 0;

    /**
     * Check if a commitment already exists in the UTXO set
     * Used to prevent replay attacks with duplicate commitments
     *
     * @param commitment 32-byte Pedersen commitment to check
     * @return true if commitment exists in UTXO set
     */
    virtual bool HaveCommitment(const std::vector<uint8_t>& commitment) const = 0;
};

/**
 * Combined UTXO view that checks both blockchain and mempool
 * Phase M.0: Changed std::string txid to uint256
 */
class CombinedUTXOView : public UTXOView {
public:
    CombinedUTXOView(std::shared_ptr<UTXOView> base, const TxMempool& mempool);

    bool HaveUTXO(const uint256& txid, uint32_t vout) const override;
    bool GetUTXO(const uint256& txid, uint32_t vout,
                 uint64_t& value, std::string& script) const override;
    bool HaveTransaction(const uint256& txid) const override;
    uint32_t GetHeight() const override;

private:
    std::shared_ptr<UTXOView> base_view_;
    const TxMempool& mempool_;
};

} // namespace dinero
