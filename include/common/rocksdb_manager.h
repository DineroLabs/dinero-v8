#ifndef DINERO_ROCKSDB_MANAGER_H
#define DINERO_ROCKSDB_MANAGER_H

#include <string>
#include <vector>
#include <mutex>
#include "compat/jsoncpp_compat.h"
#include <rocksdb/db.h>

namespace Dinero {
namespace Common {

// RocksDB manager for shared database operations
class RocksDBManager {
private:
    rocksdb::DB* db;
    rocksdb::Options options;
    std::string db_path;
    std::mutex db_mutex;
    bool read_only_mode;
    std::string miner_id;
    
public:
    RocksDBManager();
    ~RocksDBManager();
    
    // Database initialization
    bool initDatabase(const std::string& path, bool read_only = false, const std::string& id = "");
    
    // Miner identification
    std::string generateMinerId();
    std::string getHostname();
    std::string getMinerId() const;
    
    // Miner heartbeat system
    bool sendHeartbeat(const std::string& ip_hostname = "", int threads = 0, uint32_t block_height = 0);
    std::vector<Json::Value> getActiveMiners();
    
    // Performance analytics
    bool storePerformanceAnalytics(double hash_rate, uint32_t accepted_blocks, uint32_t rejected_blocks = 0);
    std::vector<Json::Value> getMinerPerformanceHistory(const std::string& miner_id, int hours = 24);
    
    // Mining statistics (thread-safe)
    bool storeMiningStats(const std::string& key, const std::string& value);
    std::string getMiningStats(const std::string& key);
    bool updateMiningStats(uint64_t total_hashes, uint32_t blocks_found, double hash_rate);
    void getMiningStatsSummary();
    
    // Block templates (thread-safe)
    bool storeBlockTemplate(uint32_t height, const std::string& template_data);
    std::string getBlockTemplate(uint32_t height);
    
    // Mined blocks (thread-safe)
    bool storeMinedBlock(const std::string& block_hash, const std::string& block_data);
    std::string getMinedBlock(const std::string& block_hash);
    
    // Configuration (thread-safe)
    bool storeConfig(const std::string& key, const std::string& value);
    std::string getConfig(const std::string& key);
    
    // Export features
    bool exportStatsToJSON(const std::string& filename);
    
    // Database maintenance
    bool compactDatabase();
    
    // Utility functions
    std::string getDatabasePath() const;
    bool isReadOnly() const;
    uint64_t getDatabaseSize();
};

} // namespace Common
} // namespace Dinero

#endif // DINERO_ROCKSDB_MANAGER_H 