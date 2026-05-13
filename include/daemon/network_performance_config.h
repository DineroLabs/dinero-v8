#pragma once
#include <cstdint>
#include <chrono>
#include <string>
#include <map>

namespace dinero {

// Network performance configuration
struct NetworkPerformanceConfig {
    // Header sync configuration
    struct HeaderSync {
        uint32_t max_headers_per_request = 2000;
        uint32_t max_blocks_per_request = 16;
        std::chrono::seconds sync_timeout{30};
        std::chrono::seconds peer_selection_interval{5};
        uint32_t max_concurrent_requests = 3;
        std::chrono::seconds request_timeout{10};
        uint32_t max_retries = 3;
        std::chrono::seconds retry_delay{5};
    } header_sync;
    
    // Address manager configuration
    struct AddressManager {
        size_t max_addresses = 10000;
        size_t max_good_addresses = 1000;
        std::chrono::seconds address_timeout{24 * 60 * 60}; // 24 hours
        std::chrono::seconds ban_duration{60 * 60}; // 1 hour
        int32_t max_score = 100;
        int32_t min_score = -100;
        uint32_t max_attempts_per_address = 10;
        std::chrono::seconds cleanup_interval{300}; // 5 minutes
    } address_manager;
    
    // Compact block relay configuration
    struct CompactBlockRelay {
        size_t max_prefilled_txns = 10;
        size_t max_short_ids = 1000;
        std::chrono::seconds tx_pool_timeout{300}; // 5 minutes
        uint32_t max_tx_pool_size = 10000;
        std::chrono::seconds reconstruction_timeout{30};
        uint32_t max_reconstruction_attempts = 3;
    } compact_block_relay;
    
    // Peer scoring configuration
    struct PeerScoring {
        int32_t max_score = 1000;
        int32_t min_score = -1000;
        int32_t ban_threshold = -100;
        std::chrono::seconds default_ban_duration{3600}; // 1 hour
        std::chrono::seconds cleanup_interval{300}; // 5 minutes
        uint32_t max_events_per_peer = 1000;
        std::chrono::seconds event_timeout{24 * 60 * 60}; // 24 hours
    } peer_scoring;
    
    // Network connection configuration
    struct Connection {
        uint32_t max_connections = 125;
        uint32_t max_inbound_connections = 25;
        uint32_t max_outbound_connections = 100;
        std::chrono::seconds connection_timeout{30};
        std::chrono::seconds handshake_timeout{10};
        uint32_t max_message_size = 32 * 1024 * 1024; // 32 MB
        uint32_t max_inventory_size = 50000;
        std::chrono::seconds ping_interval{120}; // 2 minutes
        std::chrono::seconds pong_timeout{30};
    } connection;
    
    // Bandwidth configuration
    struct Bandwidth {
        uint64_t max_download_rate = 2 * 1024 * 1024; // 2 MB/s
        uint64_t max_upload_rate = 1 * 1024 * 1024; // 1 MB/s
        uint32_t max_concurrent_downloads = 3;
        uint32_t max_concurrent_uploads = 2;
        std::chrono::seconds rate_limit_window{60}; // 1 minute
        uint32_t max_burst_size = 1024 * 1024; // 1 MB
    } bandwidth;
    
    // Memory configuration
    struct Memory {
        uint64_t max_mempool_size = 300 * 1024 * 1024; // 300 MB
        uint64_t max_block_cache_size = 100 * 1024 * 1024; // 100 MB
        uint64_t max_peer_cache_size = 50 * 1024 * 1024; // 50 MB
        uint32_t max_cached_blocks = 1000;
        uint32_t max_cached_headers = 10000;
        std::chrono::seconds cache_cleanup_interval{300}; // 5 minutes
    } memory;
    
    // Load configuration from file
    bool loadFromFile(const std::string& filename);
    
    // Save configuration to file
    bool saveToFile(const std::string& filename) const;
    
    // Get configuration value
    template<typename T>
    T getValue(const std::string& key) const;
    
    // Set configuration value
    template<typename T>
    void setValue(const std::string& key, const T& value);
    
    // Validate configuration
    bool validate() const;
    
    // Get performance metrics
    struct PerformanceMetrics {
        uint64_t bytes_downloaded;
        uint64_t bytes_uploaded;
        uint32_t connections_active;
        uint32_t connections_total;
        uint32_t blocks_synced;
        uint32_t headers_synced;
        uint32_t transactions_relayed;
        uint32_t peers_banned;
        std::chrono::milliseconds avg_sync_time;
        std::chrono::milliseconds avg_connection_time;
    };
    
    PerformanceMetrics getMetrics() const;
    
private:
    std::map<std::string, std::string> m_config;
    mutable PerformanceMetrics m_metrics;
};

// Performance monitor for network components
class NetworkPerformanceMonitor {
public:
    NetworkPerformanceMonitor();
    ~NetworkPerformanceMonitor();
    
    // Start monitoring
    void start();
    
    // Stop monitoring
    void stop();
    
    // Record metrics
    void recordBytesDownloaded(uint64_t bytes);
    void recordBytesUploaded(uint64_t bytes);
    void recordConnectionEstablished();
    void recordConnectionClosed();
    void recordBlockSynced();
    void recordHeaderSynced();
    void recordTransactionRelayed();
    void recordPeerBanned();
    void recordSyncTime(std::chrono::milliseconds duration);
    void recordConnectionTime(std::chrono::milliseconds duration);
    
    // Get current metrics
    NetworkPerformanceConfig::PerformanceMetrics getCurrentMetrics() const;
    
    // Get performance report
    std::string getPerformanceReport() const;
    
    // Check if performance is within limits
    bool isPerformanceAcceptable() const;
    
    // Get recommendations
    std::vector<std::string> getRecommendations() const;

private:
    void updateMetrics();
    void generateReport();
    
    std::atomic<bool> m_running;
    NetworkPerformanceConfig::PerformanceMetrics m_metrics;
    mutable std::mutex m_mutex;
    
    // Performance thresholds
    static constexpr uint64_t MAX_BYTES_PER_SECOND = 10 * 1024 * 1024; // 10 MB/s
    static constexpr uint32_t MAX_CONNECTIONS = 200;
    static constexpr std::chrono::milliseconds MAX_SYNC_TIME{30000}; // 30 seconds
    static constexpr std::chrono::milliseconds MAX_CONNECTION_TIME{10000}; // 10 seconds
};

} // namespace dinero
