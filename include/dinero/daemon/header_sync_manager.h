#pragma once
#include "primitives/block.h"
#include "daemon/peer_connection.h"
#include <memory>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <thread>
#include <mutex>
#include <atomic>
#include <chrono>
#include <queue>

namespace dinero {

// Header sync state
enum class HeaderSyncState {
    IDLE,
    REQUESTING_HEADERS,
    RECEIVING_HEADERS,
    VALIDATING_HEADERS,
    SYNCING_BLOCKS,
    SYNC_COMPLETE,
    SYNC_FAILED
};

// Header sync request
struct HeaderSyncRequest {
    std::string peer_id;
    std::vector<std::string> locator_hashes;
    std::string stop_hash;
    uint32_t max_headers;
    std::chrono::time_point<std::chrono::steady_clock> timestamp;
    
    HeaderSyncRequest() : max_headers(2000) {
        timestamp = std::chrono::steady_clock::now();
    }
};

// Block sync request
struct BlockSyncRequest {
    std::string peer_id;
    std::vector<std::string> block_hashes;
    std::chrono::time_point<std::chrono::steady_clock> timestamp;
    
    BlockSyncRequest() {
        timestamp = std::chrono::steady_clock::now();
    }
};

// Header sync manager for mainnet-ready synchronization
class HeaderSyncManager {
public:
    HeaderSyncManager(std::shared_ptr<Blockchain> blockchain);
    ~HeaderSyncManager();
    
    // Sync control
    bool startSync();
    void stopSync();
    bool isSyncing() const { return m_sync_state != HeaderSyncState::IDLE; }
    HeaderSyncState getSyncState() const { return m_sync_state; }
    
    // Peer management
    void addPeer(std::shared_ptr<PeerConnection> peer);
    void removePeer(const std::string& peer_id);
    std::vector<std::string> getAvailablePeers() const;
    
    // Header sync
    bool requestHeaders(const std::string& peer_id, const std::vector<std::string>& locator_hashes, const std::string& stop_hash = "");
    bool processHeaders(const std::string& peer_id, const std::vector<BlockHeader>& headers);
    
    // Block sync
    bool requestBlocks(const std::string& peer_id, const std::vector<std::string>& block_hashes);
    bool processBlock(const std::string& peer_id, const Block& block);
    
    // Sync progress
    uint32_t getSyncProgress() const; // 0-100%
    uint32_t getBestHeaderHeight() const;
    uint32_t getBestBlockHeight() const;
    std::string getBestHeaderHash() const;
    std::string getBestBlockHash() const;
    
    // Statistics
    struct SyncStats {
        uint32_t headers_received;
        uint32_t headers_validated;
        uint32_t blocks_received;
        uint32_t blocks_validated;
        uint32_t sync_errors;
        std::chrono::milliseconds sync_duration;
    };
    SyncStats getSyncStats() const;

private:
    // Core sync logic
    void syncLoop();
    bool selectBestPeer();
    bool buildLocatorHashes(std::vector<std::string>& locator_hashes);
    bool validateHeaderChain(const std::vector<BlockHeader>& headers);
    bool validateBlock(const Block& block);
    
    // Peer selection
    std::string selectPeerForHeaders();
    std::string selectPeerForBlocks();
    bool isPeerAvailable(const std::string& peer_id) const;
    
    // State management
    void setSyncState(HeaderSyncState state);
    void handleSyncError(const std::string& error);
    
    // Threading
    std::thread m_sync_thread;
    std::atomic<bool> m_running;
    mutable std::mutex m_mutex;
    
    // State
    std::atomic<HeaderSyncState> m_sync_state;
    std::shared_ptr<Blockchain> m_blockchain;
    
    // Peers
    std::map<std::string, std::shared_ptr<PeerConnection>> m_peers;
    mutable std::mutex m_peers_mutex;
    
    // Sync requests
    std::queue<HeaderSyncRequest> m_header_requests;
    std::queue<BlockSyncRequest> m_block_requests;
    mutable std::mutex m_requests_mutex;
    
    // Progress tracking
    std::atomic<uint32_t> m_best_header_height;
    std::atomic<uint32_t> m_best_block_height;
    std::string m_best_header_hash;
    std::string m_best_block_hash;
    mutable std::mutex m_progress_mutex;
    
    // Statistics
    SyncStats m_stats;
    mutable std::mutex m_stats_mutex;
    
    // Configuration
    static constexpr uint32_t MAX_HEADERS_PER_REQUEST = 2000;
    static constexpr uint32_t MAX_BLOCKS_PER_REQUEST = 16;
    static constexpr std::chrono::seconds SYNC_TIMEOUT{30};
    static constexpr std::chrono::seconds PEER_SELECTION_INTERVAL{5};
};

} // namespace dinero
