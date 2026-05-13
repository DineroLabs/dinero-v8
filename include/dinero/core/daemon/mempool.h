#pragma once

#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <chrono>
#include <atomic>
#include <map>
#include <limits>
#include "wallet/transaction.h"
#include "common/logger.h"

// Forward declarations
namespace din::sp {
    class ScannerManager;
}

namespace dinero {

// Forward declarations
class Blockchain;

/**
 * @brief Transaction entry in the mempool with metadata
 */
struct MempoolEntry {
    Transaction tx;                                              // The transaction
    uint64_t fee;                                               // Transaction fee in una
    double fee_rate;                                            // Fee per byte (una/byte)
    std::chrono::time_point<std::chrono::steady_clock> time;    // When added to mempool
    uint32_t height;                                            // Block height when added
    size_t tx_size;                                             // Transaction size in bytes
    std::vector<std::string> depends;                           // Dependencies (parent tx hashes)
    std::vector<std::string> spends;                            // UTXOs this tx spends
    
    MempoolEntry() : fee(0), fee_rate(0.0), height(0), tx_size(0) {
        time = std::chrono::steady_clock::now();
    }
    
    MempoolEntry(const Transaction& transaction, uint64_t tx_fee, uint32_t block_height)
        : tx(transaction), fee(tx_fee), height(block_height) {
        time = std::chrono::steady_clock::now();
        tx_size = tx.Serialize().size() / 2; // Hex string size / 2 = bytes
        fee_rate = tx_size > 0 ? static_cast<double>(fee) / tx_size : 0.0;
    }
};

/**
 * @brief Memory pool for unconfirmed transactions
 * 
 * The mempool maintains a collection of valid, unconfirmed transactions
 * that are candidates for inclusion in the next block. It handles:
 * - Transaction validation and acceptance
 * - Fee-based prioritization
 * - Dependency tracking
 * - Size management and eviction
 * - Network relay coordination
 */
class Mempool {
public:
    explicit Mempool(std::shared_ptr<Blockchain> blockchain);
    ~Mempool();
    
    // Core mempool operations
    bool addTransaction(const Transaction& tx, bool relay = true);
    bool removeTransaction(const std::string& txid);
    bool hasTransaction(const std::string& txid) const;
    std::shared_ptr<Transaction> getTransaction(const std::string& txid) const;
    
    // Mempool queries
    std::vector<Transaction> getAllTransactions() const;
    std::vector<Transaction> getTransactionsByFeeRate(size_t max_count = 1000) const;
    std::vector<std::string> getTransactionIds() const;
    size_t size() const;
    uint64_t getTotalFees() const;
    size_t getTotalSize() const;
    
    // Block template creation
    std::vector<Transaction> selectTransactionsForBlock(
        size_t max_block_size = 1000000,  // 1MB default
        uint64_t max_block_weight = 4000000  // 4M weight units
    ) const;
    
    // Mempool management
    void removeConfirmedTransactions(const std::vector<std::string>& confirmed_txids);
    void removeExpiredTransactions();
    void limitMempoolSize();
    void clear();
    
    // Statistics and monitoring
    struct MempoolStats {
        size_t tx_count;
        size_t total_size;
        uint64_t total_fees;
        double avg_fee_rate;
        size_t min_fee_rate;
        size_t max_fee_rate;
        std::chrono::seconds oldest_tx_age;
    };
    MempoolStats getStats() const;
    
    // Configuration
    void setMaxSize(size_t max_size) { m_max_size = max_size; }
    void setMaxAge(std::chrono::hours max_age) { m_max_age = max_age; }
    void setMinFeeRate(double min_fee_rate) { m_min_fee_rate = min_fee_rate; }
    
    // Network integration
    void broadcastTransaction(const std::string& txid);
    
private:
    // Internal validation and management
    bool validateTransaction(const Transaction& tx, std::string& error) const;
    bool checkDoubleSpend(const Transaction& tx) const;
    bool checkDependencies(const Transaction& tx) const;
    uint64_t calculateFee(const Transaction& tx) const;
    void updateDependencies(const std::string& txid);
    void evictTransactions();
    
    // Data structures
    std::unordered_map<std::string, MempoolEntry> m_transactions;  // txid -> entry
    std::unordered_set<std::string> m_spent_outputs;               // outpoint -> spending txid
    std::multimap<double, std::string> m_fee_index;                // fee_rate -> txid (sorted)
    std::multimap<std::chrono::time_point<std::chrono::steady_clock>, std::string> m_time_index; // time -> txid
    
    // Thread safety
    mutable std::shared_mutex m_mutex;
    
    // Configuration
    size_t m_max_size;                    // Maximum mempool size (bytes)
    std::chrono::hours m_max_age;         // Maximum transaction age
    double m_min_fee_rate;                // Minimum fee rate (una/byte)
    
    // Dependencies
    std::shared_ptr<Blockchain> m_blockchain;
    
    // Silent Payments scanner
    std::unique_ptr<din::sp::ScannerManager> m_sp_scanner_manager;
    
    // Statistics
    std::atomic<size_t> m_total_tx_added{0};
    std::atomic<size_t> m_total_tx_removed{0};
    std::atomic<size_t> m_total_tx_rejected{0};
    
    // Constants
    static constexpr size_t DEFAULT_MAX_SIZE = 300 * 1024 * 1024;  // 300MB
    static constexpr std::chrono::hours DEFAULT_MAX_AGE{24};        // 24 hours
    static constexpr double DEFAULT_MIN_FEE_RATE = 1.0;            // 1 sat/byte
};

} // namespace dinero
