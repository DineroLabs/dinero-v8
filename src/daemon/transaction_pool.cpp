#include "daemon/transaction_pool.h"
#include "consensus/subsidy.h"  // Canonical monetary policy
#include "crypto/sha256.h"
#include <iostream>
#include <algorithm>
#include <sstream>
#include <iomanip>

using namespace mempool;

// Transaction implementation
Json::Value Transaction::to_json() const {
    Json::Value json;
    json["txid"] = txid;
    json["version"] = version;
    json["lockTime"] = lockTime;
    json["fee"] = static_cast<double>(fee);
    json["size"] = static_cast<int>(size);
    json["fee_rate"] = get_fee_rate();
    
    // Convert received time to timestamp
    auto time_t = std::chrono::system_clock::to_time_t(received_time);
    json["received_time"] = static_cast<int64_t>(time_t);
    
    // Inputs array
    Json::Value inputs_array(Json::arrayValue);
    for (const auto& input : vin) {
        inputs_array.append(input);
    }
    json["inputs"] = inputs_array;
    
    // Outputs array  
    Json::Value outputs_array(Json::arrayValue);
    for (const auto& output : vout) {
        outputs_array.append(output);
    }
    json["outputs"] = outputs_array;
    
    // Raw data as hex
    std::ostringstream hex_stream;
    for (uint8_t byte : raw_data) {
        hex_stream << std::hex << std::setfill('0') << std::setw(2) << static_cast<int>(byte);
    }
    json["hex"] = hex_stream.str();
    
    return json;
}

std::unique_ptr<Transaction> Transaction::from_json(const Json::Value& json) {
    auto tx = std::make_unique<Transaction>();
    
    tx->txid = json["txid"].asString();
    tx->version = json["version"].asUInt();
    tx->lockTime = json["lockTime"].asUInt();
    tx->fee = json["fee"].asUInt64();
    tx->size = json["size"].asUInt();
    
    // Parse inputs
    const Json::Value& inputs_array = json["inputs"];
    for (const auto& input : inputs_array) {
        tx->vin.push_back(input.asString());
    }
    
    // Parse outputs
    const Json::Value& outputs_array = json["outputs"];
    for (const auto& output : outputs_array) {
        tx->vout.push_back(output.asString());
    }
    
    // Parse hex data
    std::string hex = json["hex"].asString();
    tx->raw_data.clear();
    for (size_t i = 0; i < hex.length(); i += 2) {
        std::string byte_str = hex.substr(i, 2);
        uint8_t byte = static_cast<uint8_t>(std::stoul(byte_str, nullptr, 16));
        tx->raw_data.push_back(byte);
    }
    
    // Set received time
    if (json.isMember("received_time")) {
        auto timestamp = json["received_time"].asInt64();
        tx->received_time = std::chrono::system_clock::from_time_t(timestamp);
    } else {
        tx->received_time = std::chrono::system_clock::now();
    }
    
    return tx;
}

std::unique_ptr<Transaction> Transaction::from_hex(const std::string& hex_data) {
    auto tx = std::make_unique<Transaction>();
    
    // Parse hex into raw bytes
    tx->raw_data.clear();
    for (size_t i = 0; i < hex_data.length(); i += 2) {
        if (i + 1 >= hex_data.length()) break;
        std::string byte_str = hex_data.substr(i, 2);
        try {
            uint8_t byte = static_cast<uint8_t>(std::stoul(byte_str, nullptr, 16));
            tx->raw_data.push_back(byte);
        } catch (...) {
            return nullptr; // Invalid hex
        }
    }
    
    if (tx->raw_data.size() < 10) { // Minimum transaction size
        return nullptr;
    }
    
    // Basic parsing (simplified)
    size_t offset = 0;
    
    // Version (4 bytes)
    if (offset + 4 > tx->raw_data.size()) return nullptr;
    tx->version = 0;
    for (int i = 0; i < 4; i++) {
        tx->version |= (static_cast<uint32_t>(tx->raw_data[offset + i]) << (i * 8));
    }
    offset += 4;
    
    // For simplicity, create dummy inputs/outputs
    tx->vin.push_back("dummy_input_" + std::to_string(tx->version));
    tx->vout.push_back("dummy_output_" + std::to_string(tx->version));
    
    // Locktime (last 4 bytes)
    if (tx->raw_data.size() >= 4) {
        size_t locktime_offset = tx->raw_data.size() - 4;
        tx->lockTime = 0;
        for (int i = 0; i < 4; i++) {
            tx->lockTime |= (static_cast<uint32_t>(tx->raw_data[locktime_offset + i]) << (i * 8));
        }
    }
    
    tx->size = tx->raw_data.size();
    tx->fee = 1000; // Default fee for testing
    tx->received_time = std::chrono::system_clock::now();
    
    // Calculate TXID
    tx->txid = tx->calculate_txid();
    
    return tx;
}

std::string Transaction::calculate_txid() const {
    // Calculate SHA-256 hash of raw transaction data
    dinero::crypto::CSHA256 hasher;
    hasher.Write(raw_data.data(), raw_data.size());
    
    unsigned char hash1[32];
    hasher.Finalize(hash1);
    
    // Double SHA-256
    dinero::crypto::CSHA256 hasher2;
    hasher2.Write(hash1, 32);
    
    unsigned char hash2[32];
    hasher2.Finalize(hash2);
    
    // Convert to hex string (reversed for Bitcoin-style display)
    std::ostringstream hex_stream;
    for (int i = 31; i >= 0; i--) {
        hex_stream << std::hex << std::setfill('0') << std::setw(2) << static_cast<int>(hash2[i]);
    }
    
    return hex_stream.str();
}

bool Transaction::is_coinbase() const {
    // Coinbase transactions have exactly one input with null hash and 0xffffffff index
    return vin.size() == 1 && vin[0].find("0000000000000000") == 0;
}

uint64_t Transaction::get_total_output_value() const {
    uint64_t total = 0;
    for (size_t i = 0; i < vout.size(); ++i) {
        total += 100000000; // 1 DIN in una
    }
    return total;
}

double Transaction::get_fee_rate() const {
    if (size == 0) return 0.0;
    return static_cast<double>(fee) / static_cast<double>(size);
}

// MempoolEntry implementation
MempoolEntry::MempoolEntry(std::unique_ptr<Transaction> transaction)
    : tx(std::move(transaction)), entry_time(std::chrono::system_clock::now()) {
    fee_rate = tx->get_fee_rate();
}

// TransactionPool implementation
TransactionPool::TransactionPool() : total_bytes_(0), total_fees_(0) {
}

TransactionPool::~TransactionPool() {
    clear();
}

bool TransactionPool::add_transaction(std::unique_ptr<Transaction> tx, std::string& error_reason) {
    if (!tx) {
        error_reason = "Null transaction";
        return false;
    }
    
    std::lock_guard<std::mutex> lock(pool_mutex_);
    
    // Check if already in pool
    if (transactions_.find(tx->txid) != transactions_.end()) {
        error_reason = "Transaction already in mempool";
        return false;
    }
    
    // Validate transaction
    if (!validate_transaction(*tx, error_reason)) {
        return false;
    }
    
    // Check for conflicts
    if (check_conflicts(*tx)) {
        error_reason = "Transaction conflicts with mempool";
        return false;
    }
    
    // Create mempool entry
    auto entry = std::make_unique<MempoolEntry>(std::move(tx));
    std::string txid = entry->tx->txid;
    
    // Update statistics
    total_bytes_ += entry->tx->size;
    total_fees_ += entry->tx->fee;
    
    // Add to indexes
    add_to_indexes(txid, *entry);
    
    // Store in pool
    transactions_[txid] = std::move(entry);
    
    std::cout << "Added transaction to mempool: " << txid.substr(0, 8) << "... (fee: " << transactions_[txid]->tx->fee << " sat)" << std::endl;
    
    // Notify handler
    if (tx_added_handler_) {
        tx_added_handler_(*transactions_[txid]->tx);
    }
    
    return true;
}

bool TransactionPool::remove_transaction(const std::string& txid) {
    std::lock_guard<std::mutex> lock(pool_mutex_);
    
    auto it = transactions_.find(txid);
    if (it == transactions_.end()) {
        return false;
    }
    
    // Update statistics
    total_bytes_ -= it->second->tx->size;
    total_fees_ -= it->second->tx->fee;
    
    // Remove from indexes
    remove_from_indexes(txid, *it->second);
    
    // Notify handler
    if (tx_removed_handler_) {
        tx_removed_handler_(*it->second->tx);
    }
    
    // Remove from pool
    transactions_.erase(it);
    
    std::cout << "Removed transaction from mempool: " << txid.substr(0, 8) << "..." << std::endl;
    return true;
}

std::unique_ptr<Transaction> TransactionPool::get_transaction(const std::string& txid) const {
    std::lock_guard<std::mutex> lock(pool_mutex_);
    
    auto it = transactions_.find(txid);
    if (it == transactions_.end()) {
        return nullptr;
    }
    
    // Create a copy
    auto copy = std::make_unique<Transaction>();
    *copy = *it->second->tx;
    return copy;
}

bool TransactionPool::has_transaction(const std::string& txid) const {
    std::lock_guard<std::mutex> lock(pool_mutex_);
    return transactions_.find(txid) != transactions_.end();
}

std::vector<std::string> TransactionPool::get_all_txids() const {
    std::lock_guard<std::mutex> lock(pool_mutex_);
    
    std::vector<std::string> txids;
    txids.reserve(transactions_.size());
    
    for (const auto& pair : transactions_) {
        txids.push_back(pair.first);
    }
    
    return txids;
}

std::vector<Transaction> TransactionPool::get_transactions_by_fee_rate(size_t max_count) const {
    std::lock_guard<std::mutex> lock(pool_mutex_);

    std::vector<Transaction> result;
    result.reserve(std::min(max_count, transactions_.size()));
    
    // Use fee rate index (highest fee rate first)
    for (auto it = fee_rate_index_.rbegin(); it != fee_rate_index_.rend() && result.size() < max_count; ++it) {
        const std::string& txid = it->second;
        auto tx_it = transactions_.find(txid);
        if (tx_it != transactions_.end()) {
            result.push_back(*tx_it->second->tx);
        }
    }
    
    return result;
}

size_t TransactionPool::get_size() const {
    std::lock_guard<std::mutex> lock(pool_mutex_);
    return transactions_.size();
}

size_t TransactionPool::get_bytes() const {
    std::lock_guard<std::mutex> lock(pool_mutex_);
    return total_bytes_;
}

std::vector<Transaction> TransactionPool::select_transactions_for_block(size_t max_block_size, uint64_t max_block_weight) const {
    std::lock_guard<std::mutex> lock(pool_mutex_);

    std::vector<Transaction> selected;
    size_t current_size = 0;
    uint64_t current_weight = 0;
    
    // Simple greedy selection by fee rate
    for (auto it = fee_rate_index_.rbegin(); it != fee_rate_index_.rend(); ++it) {
        const std::string& txid = it->second;
        auto tx_it = transactions_.find(txid);
        
        if (tx_it != transactions_.end()) {
            const Transaction& tx = *tx_it->second->tx;
            
            // Check size and weight limits
            if (current_size + tx.size > max_block_size) {
                continue; // Skip if would exceed block size
            }
            
            uint64_t tx_weight = tx.size * 4; // Simplified weight calculation
            if (current_weight + tx_weight > max_block_weight) {
                continue; // Skip if would exceed block weight
            }
            
            // Check dependencies (simplified)
            bool deps_satisfied = true;
            for (const std::string& dep_txid : tx_it->second->depends_on) {
                bool found_in_selected = false;
                for (const auto& selected_tx : selected) {
                    if (selected_tx.txid == dep_txid) {
                        found_in_selected = true;
                        break;
                    }
                }
                if (!found_in_selected) {
                    deps_satisfied = false;
                    break;
                }
            }
            
            if (deps_satisfied) {
                selected.push_back(tx);
                current_size += tx.size;
                current_weight += tx_weight;
            }
        }
    }
    
    return selected;
}

Json::Value TransactionPool::get_mempool_info() const {
    std::lock_guard<std::mutex> lock(pool_mutex_);
    
    Json::Value info;
    info["size"] = static_cast<int>(transactions_.size());
    info["bytes"] = static_cast<int>(total_bytes_);
    info["usage"] = static_cast<int>(total_bytes_); // Memory usage approximation
    info["total_fee"] = static_cast<double>(total_fees_) / static_cast<double>(dinero::ConsensusSubsidy::UNA_PER_DIN); // Convert to DIN
    info["maxmempool"] = 300000000; // 300MB limit
    info["mempoolminfee"] = 0.00001; // 1000 una minimum
    
    // Calculate average fee rate
    if (transactions_.size() > 0) {
        double avg_fee_rate = static_cast<double>(total_fees_) / static_cast<double>(total_bytes_);
        info["avg_fee_rate"] = avg_fee_rate;
    } else {
        info["avg_fee_rate"] = 0.0;
    }
    
    return info;
}

Json::Value TransactionPool::get_raw_mempool(bool verbose) const {
    std::lock_guard<std::mutex> lock(pool_mutex_);
    
    if (!verbose) {
        // Simple array of TXIDs
        Json::Value result(Json::arrayValue);
        for (const auto& pair : transactions_) {
            result.append(pair.first);
        }
        return result;
    } else {
        // Detailed object with transaction info
        Json::Value result(Json::objectValue);
        for (const auto& pair : transactions_) {
            const std::string& txid = pair.first;
            const MempoolEntry& entry = *pair.second;
            
            Json::Value tx_info;
            tx_info["size"] = static_cast<int>(entry.tx->size);
            tx_info["fee"] = static_cast<double>(entry.tx->fee) / static_cast<double>(dinero::ConsensusSubsidy::UNA_PER_DIN);
            tx_info["fee_rate"] = entry.fee_rate;
            tx_info["time"] = static_cast<int64_t>(std::chrono::system_clock::to_time_t(entry.entry_time));
            
            // Dependencies
            Json::Value depends(Json::arrayValue);
            for (const std::string& dep : entry.depends_on) {
                depends.append(dep);
            }
            tx_info["depends"] = depends;
            
            result[txid] = tx_info;
        }
        return result;
    }
}

bool TransactionPool::validate_transaction(const Transaction& tx, std::string& error_reason) const {
    // Basic format validation
    if (!validate_basic_format(tx, error_reason)) {
        return false;
    }
    
    // Input validation
    if (!validate_inputs(tx, error_reason)) {
        return false;
    }
    
    // Output validation
    if (!validate_outputs(tx, error_reason)) {
        return false;
    }
    
    // Fee validation
    if (!validate_fees(tx, error_reason)) {
        return false;
    }
    
    return true;
}

bool TransactionPool::check_conflicts(const Transaction& tx) const {
    // Check if any inputs are already spent by transactions in mempool
    for (const std::string& input : tx.vin) {
        std::string input_key = get_input_key(input);
        auto it = input_to_txids_.find(input_key);
        if (it != input_to_txids_.end() && !it->second.empty()) {
            return true; // Conflict found
        }
    }
    return false;
}

bool TransactionPool::check_dependencies(const Transaction& tx) const {
    // For now, assume no dependencies (simplified)
    return true;
}

void TransactionPool::add_to_indexes(const std::string& txid, const MempoolEntry& entry) {
    // Add to fee rate index
    fee_rate_index_.emplace(entry.fee_rate, txid);
    
    // Add to input index
    for (const std::string& input : entry.tx->vin) {
        std::string input_key = get_input_key(input);
        input_to_txids_[input_key].insert(txid);
    }
}

void TransactionPool::remove_from_indexes(const std::string& txid, const MempoolEntry& entry) {
    // Remove from fee rate index
    auto range = fee_rate_index_.equal_range(entry.fee_rate);
    for (auto it = range.first; it != range.second; ++it) {
        if (it->second == txid) {
            fee_rate_index_.erase(it);
            break;
        }
    }
    
    // Remove from input index
    for (const std::string& input : entry.tx->vin) {
        std::string input_key = get_input_key(input);
        auto it = input_to_txids_.find(input_key);
        if (it != input_to_txids_.end()) {
            it->second.erase(txid);
            if (it->second.empty()) {
                input_to_txids_.erase(it);
            }
        }
    }
}

bool TransactionPool::validate_basic_format(const Transaction& tx, std::string& error) const {
    if (tx.txid.empty()) {
        error = "Empty TXID";
        return false;
    }
    
    if (tx.raw_data.empty()) {
        error = "Empty transaction data";
        return false;
    }
    
    if (tx.size == 0 || tx.size > 1000000) { // Max 1MB transaction
        error = "Invalid transaction size";
        return false;
    }
    
    if (tx.vin.empty()) {
        error = "No inputs";
        return false;
    }
    
    if (tx.vout.empty()) {
        error = "No outputs";
        return false;
    }
    
    return true;
}

bool TransactionPool::validate_inputs(const Transaction& tx, std::string& error) const {
    // Simplified input validation
    if (tx.vin.size() > 1000) { // Max inputs
        error = "Too many inputs";
        return false;
    }
    
    return true;
}

bool TransactionPool::validate_outputs(const Transaction& tx, std::string& error) const {
    // Simplified output validation
    if (tx.vout.size() > 1000) { // Max outputs
        error = "Too many outputs";
        return false;
    }
    
    // Check for dust outputs
    for (const std::string& output : tx.vout) {
        if (is_dust_output(output)) {
            error = "Dust output detected";
            return false;
        }
    }
    
    return true;
}

bool TransactionPool::validate_fees(const Transaction& tx, std::string& error) const {
    if (tx.fee == 0) {
        error = "Zero fee transaction";
        return false;
    }
    
    if (tx.get_fee_rate() < 1.0) { // Minimum 1 sat/byte
        error = "Fee rate too low";
        return false;
    }
    
    return true;
}

void TransactionPool::remove_expired_transactions(std::chrono::seconds max_age) {
    std::lock_guard<std::mutex> lock(pool_mutex_);
    
    auto now = std::chrono::system_clock::now();
    std::vector<std::string> to_remove;
    
    for (const auto& pair : transactions_) {
        auto age = now - pair.second->entry_time;
        if (age > max_age) {
            to_remove.push_back(pair.first);
        }
    }
    
    for (const std::string& txid : to_remove) {
        // Unlock and re-lock to avoid deadlock in remove_transaction
        pool_mutex_.unlock();
        std::string dummy_error;
        remove_transaction(txid);
        pool_mutex_.lock();
    }
    
    if (!to_remove.empty()) {
        std::cout << "Removed " << to_remove.size() << " expired transactions from mempool" << std::endl;
    }
}

void TransactionPool::clear() {
    std::lock_guard<std::mutex> lock(pool_mutex_);
    
    transactions_.clear();
    input_to_txids_.clear();
    fee_rate_index_.clear();
    total_bytes_ = 0;
    total_fees_ = 0;
}

std::string TransactionPool::get_input_key(const std::string& input) const {
    // Simplified: use input string as key
    // In real implementation, would parse TXID:vout
    return input;
}

bool TransactionPool::is_dust_output(const std::string& output) const {
    // Simplified dust check
    uint64_t value = extract_output_value(output);
    return value < 546; // 546 una dust limit
}

uint64_t TransactionPool::extract_output_value(const std::string& output) const {
    // Simplified: assume fixed value for testing
    return 100000000; // 1 DIN
}
