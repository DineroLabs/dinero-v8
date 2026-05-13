#include "common/rocksdb_manager.h"
#include <rocksdb/db.h>
#include <rocksdb/options.h>
#include <rocksdb/write_batch.h>
#include <iostream>
#include <string>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <mutex>
#include <fstream>
#include "compat/jsoncpp_compat.h"
#include <sys/utsname.h>
#include <unistd.h>

namespace Dinero {
namespace Common {

RocksDBManager::RocksDBManager() : db(nullptr), read_only_mode(false) {}
    
RocksDBManager::~RocksDBManager() {
    if (db) {
        delete db;
    }
}
    
bool RocksDBManager::initDatabase(const std::string& path, bool read_only, const std::string& id) {
    db_path = path;
    read_only_mode = read_only;
    miner_id = id.empty() ? generateMinerId() : id;
    
    // Configure RocksDB options for mining workloads
    options.create_if_missing = !read_only;
    options.OptimizeForPointLookup(1024);
    options.IncreaseParallelism();
    options.max_background_jobs = 4;
    options.write_buffer_size = 64 * 1024 * 1024; // 64MB
    options.max_write_buffer_number = 3;
    options.target_file_size_base = 64 * 1024 * 1024; // 64MB
    options.max_bytes_for_level_base = 256 * 1024 * 1024; // 256MB
    
    // Read-only mode support
    if (read_only) {
        options.error_if_exists = false;
        std::cout << "📖 Opening RocksDB in read-only mode: " << db_path << std::endl;
    }
    
    // Open database
    rocksdb::Status status = rocksdb::DB::Open(options, db_path, &db);
    if (!status.ok()) {
        std::cerr << "❌ Failed to open RocksDB: " << status.ToString() << std::endl;
        return false;
    }
    
    std::cout << "✅ RocksDB initialized: " << db_path << (read_only ? " (read-only)" : "") << std::endl;
    std::cout << "🆔 Miner ID: " << miner_id << std::endl;
    return true;
}

// Generate unique miner ID
std::string RocksDBManager::generateMinerId() {
    struct utsname uts;
    if (uname(&uts) == 0) {
        std::stringstream ss;
        ss << uts.nodename << "_" << getpid() << "_" << std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        return ss.str();
    }
    return "unknown_miner_" + std::to_string(getpid());
}

std::string RocksDBManager::getHostname() {
    struct utsname uts;
    if (uname(&uts) == 0) {
        return std::string(uts.nodename);
    }
    return "unknown";
}

// Miner heartbeat system
bool RocksDBManager::sendHeartbeat(const std::string& ip_hostname, int threads, uint32_t block_height) {
    if (!db || read_only_mode) return false;
    
    std::lock_guard<std::mutex> lock(db_mutex);
    
    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);
    std::stringstream ss;
    ss << std::put_time(std::localtime(&time_t), "%Y-%m-%d %H:%M:%S");
    
    Json::Value heartbeat;
    heartbeat["miner_id"] = miner_id;
    heartbeat["timestamp"] = ss.str();
    heartbeat["ip_hostname"] = ip_hostname.empty() ? getHostname() : ip_hostname;
    heartbeat["threads"] = threads;
    heartbeat["block_height"] = block_height;
    heartbeat["version"] = "1.0.0";
    
    Json::FastWriter writer;
    std::string heartbeat_json = writer.write(heartbeat);
    
    std::string key = "miner:heartbeat:" + miner_id;
    rocksdb::Status status = db->Put(rocksdb::WriteOptions(), key, heartbeat_json);
    
    if (status.ok()) {
        std::cout << "💓 Heartbeat sent: " << miner_id << std::endl;
        return true;
    } else {
        std::cerr << "❌ Failed to send heartbeat: " << status.ToString() << std::endl;
        return false;
    }
}

std::vector<Json::Value> RocksDBManager::getActiveMiners() {
    std::vector<Json::Value> miners;
    if (!db) return miners;
    
    std::lock_guard<std::mutex> lock(db_mutex);
    
    auto now = std::chrono::system_clock::now();
    auto cutoff = now - std::chrono::minutes(5); // Consider miners active if heartbeat within 5 minutes
    
    rocksdb::Iterator* it = db->NewIterator(rocksdb::ReadOptions());
    for (it->Seek("miner:heartbeat:"); it->Valid() && it->key().starts_with("miner:heartbeat:"); it->Next()) {
        std::string value = it->value().ToString();
        
        Json::Value heartbeat;
        try { heartbeat = Json::Reader().parse(value, result); } catch(...) { /* parse failed */ } {
            // Parse timestamp and check if recent
            std::string timestamp_str = heartbeat["timestamp"].asString();
            std::tm tm = {};
            std::istringstream ss(timestamp_str);
            ss >> std::get_time(&tm, "%Y-%m-%d %H:%M:%S");
            
            auto heartbeat_time = std::chrono::system_clock::from_time_t(std::mktime(&tm));
            if (heartbeat_time > cutoff) {
                miners.append(heartbeat);
            }
        }
    }
    delete it;
    
    return miners;
}

bool RocksDBManager::storePerformanceAnalytics(double hash_rate, uint32_t accepted_blocks, uint32_t rejected_blocks) {
    if (!db || read_only_mode) return false;
    
    std::lock_guard<std::mutex> lock(db_mutex);
    
    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);
    std::stringstream ss;
    ss << std::put_time(std::localtime(&time_t), "%Y-%m-%d %H:%M:%S");
    
    Json::Value analytics;
    analytics["miner_id"] = miner_id;
    analytics["timestamp"] = ss.str();
    analytics["hash_rate"] = hash_rate;
    analytics["accepted_blocks"] = accepted_blocks;
    analytics["rejected_blocks"] = rejected_blocks;
    analytics["efficiency"] = accepted_blocks > 0 ? (double)accepted_blocks / (accepted_blocks + rejected_blocks) : 0.0;
    
    Json::FastWriter writer;
    std::string analytics_json = writer.write(analytics);
    
    std::string key = "miner:analytics:" + miner_id + ":" + std::to_string(time_t);
    rocksdb::Status status = db->Put(rocksdb::WriteOptions(), key, analytics_json);
    
    return status.ok();
}

std::vector<Json::Value> RocksDBManager::getMinerPerformanceHistory(const std::string& miner_id, int hours) {
    std::vector<Json::Value> history;
    if (!db) return history;
    
    std::lock_guard<std::mutex> lock(db_mutex);
    
    auto now = std::chrono::system_clock::now();
    auto cutoff = now - std::chrono::hours(hours);
    
    rocksdb::Iterator* it = db->NewIterator(rocksdb::ReadOptions());
    for (it->Seek("miner:analytics:" + miner_id + ":"); it->Valid() && it->key().starts_with("miner:analytics:" + miner_id + ":"); it->Next()) {
        std::string value = it->value().ToString();
        
        Json::Value analytics;
        try { analytics = Json::Reader().parse(value, result); } catch(...) { /* parse failed */ } {
            std::string timestamp_str = analytics["timestamp"].asString();
            std::tm tm = {};
            std::istringstream ss(timestamp_str);
            ss >> std::get_time(&tm, "%Y-%m-%d %H:%M:%S");
            
            auto analytics_time = std::chrono::system_clock::from_time_t(std::mktime(&tm));
            if (analytics_time > cutoff) {
                history.append(analytics);
            }
        }
    }
    delete it;
    
    return history;
}

bool RocksDBManager::storeMiningStats(const std::string& key, const std::string& value) {
    if (!db || read_only_mode) return false;
    
    std::lock_guard<std::mutex> lock(db_mutex);
    
    std::string full_key = "mining:stats:" + key;
    rocksdb::Status status = db->Put(rocksdb::WriteOptions(), full_key, value);
    
    return status.ok();
}

std::string RocksDBManager::getMiningStats(const std::string& key) {
    if (!db) return "";
    
    std::lock_guard<std::mutex> lock(db_mutex);
    
    std::string full_key = "mining:stats:" + key;
    std::string value;
    rocksdb::Status status = db->Get(rocksdb::ReadOptions(), full_key, &value);
    
    return status.ok() ? value : "";
}

bool RocksDBManager::storeBlockTemplate(uint32_t height, const std::string& template_data) {
    if (!db || read_only_mode) return false;
    
    std::lock_guard<std::mutex> lock(db_mutex);
    
    std::string key = "block:template:" + std::to_string(height);
    rocksdb::Status status = db->Put(rocksdb::WriteOptions(), key, template_data);
    
    return status.ok();
}

std::string RocksDBManager::getBlockTemplate(uint32_t height) {
    if (!db) return "";
    
    std::lock_guard<std::mutex> lock(db_mutex);
    
    std::string key = "block:template:" + std::to_string(height);
    std::string value;
    rocksdb::Status status = db->Get(rocksdb::ReadOptions(), key, &value);
    
    return status.ok() ? value : "";
}

bool RocksDBManager::storeMinedBlock(const std::string& block_hash, const std::string& block_data) {
    if (!db || read_only_mode) return false;
    
    std::lock_guard<std::mutex> lock(db_mutex);
    
    std::string key = "block:mined:" + block_hash;
    rocksdb::Status status = db->Put(rocksdb::WriteOptions(), key, block_data);
    
    return status.ok();
}

std::string RocksDBManager::getMinedBlock(const std::string& block_hash) {
    if (!db) return "";
    
    std::lock_guard<std::mutex> lock(db_mutex);
    
    std::string key = "block:mined:" + block_hash;
    std::string value;
    rocksdb::Status status = db->Get(rocksdb::ReadOptions(), key, &value);
    
    return status.ok() ? value : "";
}

bool RocksDBManager::storeConfig(const std::string& key, const std::string& value) {
    if (!db || read_only_mode) return false;
    
    std::lock_guard<std::mutex> lock(db_mutex);
    
    std::string full_key = "config:" + key;
    rocksdb::Status status = db->Put(rocksdb::WriteOptions(), full_key, value);
    
    return status.ok();
}

std::string RocksDBManager::getConfig(const std::string& key) {
    if (!db) return "";
    
    std::lock_guard<std::mutex> lock(db_mutex);
    
    std::string full_key = "config:" + key;
    std::string value;
    rocksdb::Status status = db->Get(rocksdb::ReadOptions(), full_key, &value);
    
    return status.ok() ? value : "";
}

bool RocksDBManager::updateMiningStats(uint64_t total_hashes, uint32_t blocks_found, double hash_rate) {
    if (!db || read_only_mode) return false;
    
    std::lock_guard<std::mutex> lock(db_mutex);
    
    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);
    std::stringstream ss;
    ss << std::put_time(std::localtime(&time_t), "%Y-%m-%d %H:%M:%S");
    
    Json::Value stats;
    stats["miner_id"] = miner_id;
    stats["timestamp"] = ss.str();
    stats["total_hashes"] = Json::Value::UInt64(total_hashes);
    stats["blocks_found"] = blocks_found;
    stats["hash_rate"] = hash_rate;
    stats["uptime"] = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
    
    Json::FastWriter writer;
    std::string stats_json = writer.write(stats);
    
    std::string key = "mining:stats:" + miner_id;
    rocksdb::Status status = db->Put(rocksdb::WriteOptions(), key, stats_json);
    
    return status.ok();
}

void RocksDBManager::getMiningStatsSummary() {
    if (!db) return;
    
    std::lock_guard<std::mutex> lock(db_mutex);
    
    std::cout << "\n📊 Mining Statistics Summary:" << std::endl;
    std::cout << "================================" << std::endl;
    
    rocksdb::Iterator* it = db->NewIterator(rocksdb::ReadOptions());
    for (it->Seek("mining:stats:"); it->Valid() && it->key().starts_with("mining:stats:"); it->Next()) {
        std::string value = it->value().ToString();
        
        Json::Value stats;
        try { stats = Json::Reader().parse(value, result); } catch(...) { /* parse failed */ } {
            std::cout << "🆔 Miner: " << stats["miner_id"].asString() << std::endl;
            std::cout << "⏰ Last Update: " << stats["timestamp"].asString() << std::endl;
            std::cout << "🔢 Total Hashes: " << stats["total_hashes"].asUInt64() << std::endl;
            std::cout << "🏆 Blocks Found: " << stats["blocks_found"].asUInt() << std::endl;
            std::cout << "⚡ Hash Rate: " << stats["hash_rate"].asDouble() << " H/s" << std::endl;
            std::cout << "--------------------------------" << std::endl;
        }
    }
    delete it;
}

bool RocksDBManager::exportStatsToJSON(const std::string& filename) {
    if (!db) return false;
    
    std::lock_guard<std::mutex> lock(db_mutex);
    
    Json::Value export_data;
    export_data["export_timestamp"] = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    export_data["database_path"] = db_path;
    export_data["miner_id"] = miner_id;
    
    Json::Value mining_stats;
    Json::Value heartbeats;
    Json::Value analytics;
    
    rocksdb::Iterator* it = db->NewIterator(rocksdb::ReadOptions());
    
    // Export mining stats
    for (it->Seek("mining:stats:"); it->Valid() && it->key().starts_with("mining:stats:"); it->Next()) {
        std::string value = it->value().ToString();
        Json::Value stats;
        try { stats = Json::Reader().parse(value, result); } catch(...) { /* parse failed */ } {
            mining_stats.append(stats);
        }
    }
    
    // Export heartbeats
    for (it->Seek("miner:heartbeat:"); it->Valid() && it->key().starts_with("miner:heartbeat:"); it->Next()) {
        std::string value = it->value().ToString();
        Json::Value heartbeat;
        try { heartbeat = Json::Reader().parse(value, result); } catch(...) { /* parse failed */ } {
            heartbeats.append(heartbeat);
        }
    }
    
    // Export analytics
    for (it->Seek("miner:analytics:"); it->Valid() && it->key().starts_with("miner:analytics:"); it->Next()) {
        std::string value = it->value().ToString();
        Json::Value analytics_entry;
        try { analytics_entry = Json::Reader().parse(value, result); } catch(...) { /* parse failed */ } {
            analytics.append(analytics_entry);
        }
    }
    
    delete it;
    
    export_data["mining_stats"] = mining_stats;
    export_data["heartbeats"] = heartbeats;
    export_data["analytics"] = analytics;
    
    std::ofstream file(filename);
    if (file.is_open()) {
        Json::StyledWriter writer;
        file << writer.write(export_data);
        file.close();
        std::cout << "✅ Statistics exported to: " << filename << std::endl;
        return true;
    } else {
        std::cerr << "❌ Failed to open file for export: " << filename << std::endl;
        return false;
    }
}

bool RocksDBManager::compactDatabase() {
    if (!db || read_only_mode) return false;
    
    std::lock_guard<std::mutex> lock(db_mutex);
    
    std::cout << "🔧 Compacting database..." << std::endl;
    rocksdb::Status status = db->CompactRange(rocksdb::CompactRangeOptions(), nullptr, nullptr);
    
    if (status.ok()) {
        std::cout << "✅ Database compaction completed" << std::endl;
        return true;
    } else {
        std::cerr << "❌ Database compaction failed: " << status.ToString() << std::endl;
        return false;
    }
}

std::string RocksDBManager::getDatabasePath() const {
    return db_path;
}

bool RocksDBManager::isReadOnly() const {
    return read_only_mode;
}

std::string RocksDBManager::getMinerId() const {
    return miner_id;
}

uint64_t RocksDBManager::getDatabaseSize() {
    if (!db) return 0;
    
    std::lock_guard<std::mutex> lock(db_mutex);
    
    // This is a simplified size calculation
    // In a real implementation, you might want to iterate through all keys
    uint64_t size = 0;
    rocksdb::Iterator* it = db->NewIterator(rocksdb::ReadOptions());
    for (it->SeekToFirst(); it->Valid(); it->Next()) {
        size += it->key().size() + it->value().size();
    }
    delete it;
    
    return size;
}

} // namespace Common
} // namespace Dinero 