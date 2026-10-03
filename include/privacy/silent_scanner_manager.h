#pragma once
#include "privacy/silent_scanner.h"
#include "privacy/silent_txview_glue.h"
#include <vector>
#include <memory>
#include <mutex>
#include <string>
#include <map>
#include <chrono>

namespace din::sp {

class ScannerManager {
public:
    ScannerManager();
    ~ScannerManager();
    
    // Scanner management
    void addScanner(const std::string& wallet_id, 
                   const std::array<uint8_t,32>& scan_priv,
                   const std::array<uint8_t,33>& spend_pub);
    void removeScanner(const std::string& wallet_id);
    void clearScanners();
    
    // An unsupported transaction family may proceed only while this owner
    // proves no configured scanner is being skipped. Manager outlives owner.
    class EmptyRegistry {
        friend class ScannerManager;
        explicit EmptyRegistry(std::mutex& mutex):lock_(mutex){}
        std::unique_lock<std::mutex> lock_;
    };
    std::unique_ptr<EmptyRegistry> acquireEmptyRegistry() {
        auto owner=std::unique_ptr<EmptyRegistry>(new EmptyRegistry(scanners_mutex_));
        if (!scanners_.empty()) return {};
        return owner;
    }

    // Scanning operations
    void scanMempoolTransaction(const std::string& tx_hex);
    void scanBlockTransaction(const std::string& tx_hex, uint32_t block_height);
    void scanBlockTransactions(const std::vector<std::string>& tx_hexes, uint32_t block_height);
    
    // Results management
    struct ScanResult {
        std::string wallet_id;
        std::string txid;
        uint32_t block_height;
        std::vector<Detection> detections;
        std::chrono::system_clock::time_point scan_time;
    };
    
    std::vector<ScanResult> getScanResults(const std::string& wallet_id = "");
    void clearScanResults(const std::string& wallet_id = "");
    
    // Statistics
    struct ScanStats {
        size_t total_scans;
        size_t total_hits;
        size_t mempool_scans;
        size_t block_scans;
        std::chrono::milliseconds total_scan_time;
    };
    
    ScanStats getStats() const;
    void resetStats();

private:
    struct ScannerEntry {
        std::string wallet_id;
        std::unique_ptr<Scanner> scanner;
        std::array<uint8_t,32> scan_priv;
        std::array<uint8_t,33> spend_pub;
    };
    
    std::vector<ScannerEntry> scanners_;
    std::vector<ScanResult> scan_results_;
    mutable std::mutex scanners_mutex_;
    mutable std::mutex results_mutex_;
    
    ScanStats stats_;
    mutable std::mutex stats_mutex_;
    
    // Helper methods
    TxView extractTxViewFromHex(const std::string& tx_hex, bool is_mempool = true);
    void processScanResults(const std::string& txid, uint32_t block_height, 
                          const std::vector<Detection>& detections);
    void updateStats(size_t scans, size_t hits, std::chrono::milliseconds scan_time);
};

} // namespace din::sp
