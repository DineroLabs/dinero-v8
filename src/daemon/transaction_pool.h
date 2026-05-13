#pragma once

#include <json/json.h>
#include <string>
#include <vector>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <chrono>
#include <functional>

// Cross-platform transaction mempool (no Qt dependencies)
// Handles transaction validation, storage, and fee prioritization

namespace mempool {

// Mempool transaction representation (lightweight, for pool operations)
// NOTE: This is distinct from dinero::Transaction (wallet/consensus type)
struct Transaction {
    std::string txid;
    std::vector<uint8_t> raw_data;
    uint32_t version;
    std::vector<std::string> vin;  // Previous transaction references (legacy string format)
    std::vector<std::string> vout; // Output scripts and amounts (legacy string format)
    uint32_t lockTime;
    uint64_t fee;
    size_t size;
    std::chrono::system_clock::time_point received_time;

    // Serialization
    Json::Value to_json() const;
    static std::unique_ptr<Transaction> from_json(const Json::Value& json);
    static std::unique_ptr<Transaction> from_hex(const std::string& hex_data);

    // Validation helpers
    std::string calculate_txid() const;
    bool is_coinbase() const;
    uint64_t get_total_output_value() const;
    double get_fee_rate() const; // una per byte
};

struct MempoolEntry {
    std::unique_ptr<Transaction> tx;
    double fee_rate;
    std::chrono::system_clock::time_point entry_time;
    std::unordered_set<std::string> depends_on; // TXIDs this tx depends on
    std::unordered_set<std::string> depended_by; // TXIDs that depend on this tx

    MempoolEntry(std::unique_ptr<Transaction> transaction);
};

class TransactionPool {
public:
    using TransactionHandler = std::function<void(const Transaction& tx)>;

    TransactionPool();
    ~TransactionPool();

    // Transaction operations
    bool add_transaction(std::unique_ptr<Transaction> tx, std::string& error_reason);
    bool remove_transaction(const std::string& txid);
    std::unique_ptr<Transaction> get_transaction(const std::string& txid) const;
    bool has_transaction(const std::string& txid) const;

    // Mempool queries
    std::vector<std::string> get_all_txids() const;
    std::vector<Transaction> get_transactions_by_fee_rate(size_t max_count = 1000) const;
    size_t get_size() const;
    size_t get_bytes() const;

    // Block template generation
    std::vector<Transaction> select_transactions_for_block(
        size_t max_block_size = 1000000,
        uint64_t max_block_weight = 4000000
    ) const;

    // Validation
    bool validate_transaction(const Transaction& tx, std::string& error_reason) const;
    bool check_conflicts(const Transaction& tx) const;
    bool check_dependencies(const Transaction& tx) const;

    // Statistics
    Json::Value get_mempool_info() const;
    Json::Value get_raw_mempool(bool verbose = false) const;

    // Event handlers
    void set_transaction_added_handler(TransactionHandler handler) { tx_added_handler_ = handler; }
    void set_transaction_removed_handler(TransactionHandler handler) { tx_removed_handler_ = handler; }

    // Cleanup
    void remove_expired_transactions(std::chrono::seconds max_age = std::chrono::hours(24));
    void clear();

private:
    mutable std::mutex pool_mutex_;
    std::unordered_map<std::string, std::unique_ptr<MempoolEntry>> transactions_;

    // Indexes for fast lookups
    std::unordered_map<std::string, std::unordered_set<std::string>> input_to_txids_; // input -> txids using it
    std::multimap<double, std::string> fee_rate_index_; // fee_rate -> txid (sorted)

    // Statistics
    size_t total_bytes_;
    uint64_t total_fees_;

    // Event handlers
    TransactionHandler tx_added_handler_;
    TransactionHandler tx_removed_handler_;

    // Internal operations
    void add_to_indexes(const std::string& txid, const MempoolEntry& entry);
    void remove_from_indexes(const std::string& txid, const MempoolEntry& entry);
    void update_dependencies(const std::string& txid, const Transaction& tx);

    // Validation helpers
    bool validate_basic_format(const Transaction& tx, std::string& error) const;
    bool validate_inputs(const Transaction& tx, std::string& error) const;
    bool validate_outputs(const Transaction& tx, std::string& error) const;
    bool validate_fees(const Transaction& tx, std::string& error) const;

    // Utility functions
    std::string get_input_key(const std::string& input) const;
    bool is_dust_output(const std::string& output) const;
    uint64_t extract_output_value(const std::string& output) const;
};

} // namespace mempool
