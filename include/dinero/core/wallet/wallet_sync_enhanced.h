#pragma once

#include <string>
#include <sqlite3.h>

namespace dinero {

/**
 * Enhanced wallet synchronization with proper gap-limit handling and incremental sync
 * 
 * Features:
 * - Incremental rescan from last_scanned_height
 * - Gap-limit handling for address derivation
 * - Seed and descriptor export
 * - Atomic database operations
 * - Performance optimizations
 */
class WalletSyncEnhanced {
public:
    explicit WalletSyncEnhanced(const std::string& wallet_db_path, const std::string& blockchain_db_path);
    ~WalletSyncEnhanced();
    
    // Database initialization
    bool Initialize();
    
    // Synchronization methods
    bool RescanFromHeight(int start_height, int gap_limit = 20);
    bool IncrementalSync();
    
    // Export functionality
    bool ExportSeed(const std::string& wallet_name, std::string& seed_hex);
    bool ExportDescriptor(const std::string& wallet_name, std::string& descriptor);
    
    // Status queries
    int GetCurrentBlockchainHeight() const;
    int GetLastScannedHeight() const;
    
private:
    // Internal methods
    bool UpdateSyncMeta(int start_height, int current_height, int gap_limit);
    bool UpdateLastScannedHeight(int height);
    bool DeriveAndRegisterWatchScripts(int gap_limit);
    bool PerformRescan(int start_height, int end_height);
    
    std::string wallet_db_path_;
    std::string blockchain_db_path_;
    sqlite3* wallet_db_;
    sqlite3* blockchain_db_;
};

} // namespace dinero
