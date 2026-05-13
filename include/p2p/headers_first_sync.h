#pragma once

#include "din_json.h"
#include <vector>
#include <string>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <chrono>

// Forward declarations
#ifdef QT_CORE_LIB
class PeerManager;
class Peer;
class QByteArray;
#endif

namespace dinero {
namespace p2p {

struct BlockHeader {
    uint32_t version;
    std::string prev_block_hash;
    std::string merkle_root;
    uint32_t timestamp;
    uint32_t bits;
    uint32_t nonce;
    std::string hash;
    uint32_t height;
    
    din::Json toJson() const;
    static BlockHeader fromJson(const din::Json& json);
};

struct HeadersRequest {
    std::string start_hash;
    std::string stop_hash;
    uint32_t max_headers = 2000;
    
    din::Json toJson() const;
};

struct HeadersResponse {
    std::vector<BlockHeader> headers;
    bool more_available = false;
    
    din::Json toJson() const;
    static HeadersResponse fromJson(const din::Json& json);
};

enum class SyncState {
    IDLE,
    REQUESTING_HEADERS,
    VALIDATING_HEADERS,
    REQUESTING_BLOCKS,
    SYNCED,
    ERROR
};

class HeadersFirstSync {
public:
    HeadersFirstSync();
    ~HeadersFirstSync();
    
    // Core sync operations
    void startSync(const std::string& peer_id);
    void stopSync();
    bool isSyncing() const;
    SyncState getCurrentState() const;
    
    // Header processing
    bool processHeaders(const std::string& peer_id, const HeadersResponse& response);
    void requestNextHeaders(const std::string& peer_id);
    
    // Block requests after headers validated
    void requestBlocks(const std::vector<std::string>& block_hashes);
    bool processBlock(const std::string& block_hash, const std::string& block_data);
    
    // Status and metrics
    uint32_t getBestHeight() const;
    std::string getBestBlockHash() const;
    uint32_t getHeadersCount() const;
    uint32_t getBlocksDownloaded() const;
    
    // Configuration
    void setMaxHeadersPerRequest(uint32_t max_headers);
    void setValidationEnabled(bool enabled);
    void setTimeout(std::chrono::seconds timeout);
    
    // P2P Network connection
#ifdef QT_CORE_LIB
    void setPeerManager(PeerManager* peer_manager);
    PeerManager* getPeerManager() const { return peer_manager_; }
#endif
    
    // JSON serialization for RPC/logging
    din::Json getStatus() const;
    din::Json getMetrics() const;

private:
    mutable std::mutex mutex_;
    
    // Sync state
    SyncState state_;
    std::string active_peer_;
    std::chrono::steady_clock::time_point last_activity_;
    std::chrono::seconds timeout_;
    
    // P2P networking
#ifdef QT_CORE_LIB
    PeerManager* peer_manager_;
#endif
    
    // Headers chain
    std::vector<BlockHeader> headers_chain_;
    std::unordered_map<std::string, size_t> hash_to_index_;
    uint32_t best_height_;
    std::string best_block_hash_;
    
    // Block download tracking
    std::vector<std::string> pending_blocks_;
    std::unordered_map<std::string, bool> downloaded_blocks_;
    uint32_t blocks_downloaded_;
    
    // Configuration
    uint32_t max_headers_per_request_;
    bool validation_enabled_;
    
    // Metrics
    uint32_t total_headers_received_;
    uint32_t total_blocks_requested_;
    std::chrono::steady_clock::time_point sync_start_time_;
    
    // Internal methods
    bool validateHeadersChain(const std::vector<BlockHeader>& headers);
    bool validateHeader(const BlockHeader& header, const BlockHeader* prev_header);
    void updateBestChain();
    void transitionToState(SyncState new_state);
    bool isTimeout() const;
    void logSyncProgress();
    
    // P2P message helpers
    void requestHeaders();
#ifdef QT_CORE_LIB
    QByteArray serializeHeadersRequest(const std::string& start_hash, uint32_t max_headers);
    QByteArray serializeBlockRequest(const std::string& block_hash);
#endif
};

// Architecture V3: No globals
// HeadersFirstSync is now managed via DaemonContext
// Access via: ctx->headers_sync or HeadersSyncService

// Deprecated: Use HeadersSyncService instead
// void InitializeHeadersSync();
// void ShutdownHeadersSync();

} // namespace p2p
} // namespace dinero
