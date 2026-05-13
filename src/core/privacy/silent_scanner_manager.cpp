#include "privacy/silent_scanner_manager.h"
#include "privacy/silent_scanner.h"
#include "privacy/silent_txview_glue.h"
#include "wallet/transaction.h"
#include <chrono>
#include <algorithm>

namespace din::sp {

ScannerManager::ScannerManager() {
    stats_ = {0, 0, 0, 0, std::chrono::milliseconds(0)};
}

ScannerManager::~ScannerManager() {
    clearScanners();
}

void ScannerManager::addScanner(const std::string& wallet_id, 
                               const std::array<uint8_t,32>& scan_priv,
                               const std::array<uint8_t,33>& spend_pub) {
    std::lock_guard<std::mutex> lock(scanners_mutex_);
    
    // Remove existing scanner for this wallet
    scanners_.erase(
        std::remove_if(scanners_.begin(), scanners_.end(),
                     [&wallet_id](const ScannerEntry& entry) {
                         return entry.wallet_id == wallet_id;
                     }),
        scanners_.end()
    );
    
    // Add new scanner
    ScannerEntry entry;
    entry.wallet_id = wallet_id;
    entry.scanner = std::make_unique<Scanner>(scan_priv, spend_pub);
    entry.scan_priv = scan_priv;
    entry.spend_pub = spend_pub;
    
    scanners_.push_back(std::move(entry));
}

void ScannerManager::removeScanner(const std::string& wallet_id) {
    std::lock_guard<std::mutex> lock(scanners_mutex_);
    
    scanners_.erase(
        std::remove_if(scanners_.begin(), scanners_.end(),
                     [&wallet_id](const ScannerEntry& entry) {
                         return entry.wallet_id == wallet_id;
                     }),
        scanners_.end()
    );
}

void ScannerManager::clearScanners() {
    std::lock_guard<std::mutex> lock(scanners_mutex_);
    scanners_.clear();
}

void ScannerManager::scanMempoolTransaction(const std::string& tx_hex) {
    auto start_time = std::chrono::high_resolution_clock::now();
    
    try {
        TxView tx_view = extractTxViewFromHex(tx_hex, true);
        
        std::lock_guard<std::mutex> lock(scanners_mutex_);
        
        for (const auto& entry : scanners_) {
            auto detections = entry.scanner->scan_tx(tx_view);
            if (!detections.empty()) {
                // Extract txid from hex (simplified)
                std::string txid = "mempool_" + std::to_string(std::hash<std::string>{}(tx_hex));
                processScanResults(txid, 0, detections);
            }
        }
        
        auto end_time = std::chrono::high_resolution_clock::now();
        auto scan_time = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
        
        std::lock_guard<std::mutex> stats_lock(stats_mutex_);
        stats_.mempool_scans++;
        stats_.total_scans++;
        stats_.total_scan_time += scan_time;
        
    } catch (const std::exception& e) {
        // Log error but don't crash
        // TODO: Add proper logging
    }
}

void ScannerManager::scanBlockTransaction(const std::string& tx_hex, uint32_t block_height) {
    auto start_time = std::chrono::high_resolution_clock::now();
    
    try {
        TxView tx_view = extractTxViewFromHex(tx_hex, false);
        
        std::lock_guard<std::mutex> lock(scanners_mutex_);
        
        for (const auto& entry : scanners_) {
            auto detections = entry.scanner->scan_tx(tx_view);
            if (!detections.empty()) {
                // Extract txid from hex (simplified)
                std::string txid = "block_" + std::to_string(block_height) + "_" + 
                                 std::to_string(std::hash<std::string>{}(tx_hex));
                processScanResults(txid, block_height, detections);
            }
        }
        
        auto end_time = std::chrono::high_resolution_clock::now();
        auto scan_time = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
        
        std::lock_guard<std::mutex> stats_lock(stats_mutex_);
        stats_.block_scans++;
        stats_.total_scans++;
        stats_.total_scan_time += scan_time;
        
    } catch (const std::exception& e) {
        // Log error but don't crash
        // TODO: Add proper logging
    }
}

void ScannerManager::scanBlockTransactions(const std::vector<std::string>& tx_hexes, uint32_t block_height) {
    for (const auto& tx_hex : tx_hexes) {
        scanBlockTransaction(tx_hex, block_height);
    }
}

std::vector<ScannerManager::ScanResult> ScannerManager::getScanResults(const std::string& wallet_id) {
    std::lock_guard<std::mutex> lock(results_mutex_);
    
    if (wallet_id.empty()) {
        return scan_results_;
    }
    
    std::vector<ScanResult> filtered_results;
    for (const auto& result : scan_results_) {
        if (result.wallet_id == wallet_id) {
            filtered_results.push_back(result);
        }
    }
    
    return filtered_results;
}

void ScannerManager::clearScanResults(const std::string& wallet_id) {
    std::lock_guard<std::mutex> lock(results_mutex_);
    
    if (wallet_id.empty()) {
        scan_results_.clear();
    } else {
        scan_results_.erase(
            std::remove_if(scan_results_.begin(), scan_results_.end(),
                         [&wallet_id](const ScanResult& result) {
                             return result.wallet_id == wallet_id;
                         }),
            scan_results_.end()
        );
    }
}

ScannerManager::ScanStats ScannerManager::getStats() const {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    return stats_;
}

void ScannerManager::resetStats() {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    stats_ = {0, 0, 0, 0, std::chrono::milliseconds(0)};
}

TxView ScannerManager::extractTxViewFromHex(const std::string& tx_hex, bool is_mempool) {
    TxView tx_view;
    
    // TODO: Implement proper transaction parsing from hex
    // For now, create a minimal TxView structure
    
    // Simulate taproot outputs (P2TR outputs)
    std::array<uint8_t,32> tap_output{};
    for (int i = 0; i < 32; i++) {
        tap_output[i] = (i + 1) % 256;
    }
    tx_view.tap_outputs_xonly.push_back(tap_output);
    
    // Simulate input pubkeys (eligible inputs)
    std::array<uint8_t,33> sec1_pubkey{};
    sec1_pubkey[0] = 0x02; // even-Y
    for (int i = 1; i < 33; i++) {
        sec1_pubkey[i] = (i + 1) % 256;
    }
    tx_view.input_pubkeys_sec1.push_back(sec1_pubkey);
    
    // Simulate outpoint_L_le (36 bytes)
    for (int i = 0; i < 36; i++) {
        tx_view.outpoint_L_le[i] = i + 1;
    }
    
    return tx_view;
}

void ScannerManager::processScanResults(const std::string& txid, uint32_t block_height, 
                                      const std::vector<Detection>& detections) {
    std::lock_guard<std::mutex> lock(results_mutex_);
    
    ScanResult result;
    result.txid = txid;
    result.block_height = block_height;
    result.detections = detections;
    result.scan_time = std::chrono::system_clock::now();
    
    // Find the wallet_id for this scan (simplified - in real implementation, 
    // we'd track which scanner found the matches)
    if (!scanners_.empty()) {
        result.wallet_id = scanners_[0].wallet_id;
    }
    
    scan_results_.push_back(result);
    
    // Update stats
    std::lock_guard<std::mutex> stats_lock(stats_mutex_);
    stats_.total_hits += detections.size();
}

void ScannerManager::updateStats(size_t scans, size_t hits, std::chrono::milliseconds scan_time) {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    stats_.total_scans += scans;
    stats_.total_hits += hits;
    stats_.total_scan_time += scan_time;
}

} // namespace din::sp
