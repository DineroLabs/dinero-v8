#pragma once

#include <vector>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <memory>
#include <mutex>
#include <chrono>
#include <atomic>
#include <cstdint>
#include <random>

namespace dinero {
namespace p2p {

/**
 * Network address information for peers
 */
struct NetworkAddress {
    std::string ip;
    uint16_t port;
    uint64_t services;  // Service flags (NODE_NETWORK, etc.)
    std::chrono::system_clock::time_point timestamp;
    
    std::string toString() const;
    bool isValid() const;
    bool isRoutable() const;
    bool isLocal() const;
    
    bool operator==(const NetworkAddress& other) const;
    bool operator<(const NetworkAddress& other) const;
};

/**
 * Address entry with connection statistics
 */
struct AddressEntry {
    NetworkAddress addr;
    std::chrono::system_clock::time_point first_seen;
    std::chrono::system_clock::time_point last_seen;
    std::chrono::system_clock::time_point last_try;
    std::chrono::system_clock::time_point last_success;
    
    uint32_t attempts;          // Connection attempts
    uint32_t successes;         // Successful connections
    uint32_t ref_count;         // Reference count from other peers
    double success_rate;        // Success rate (0.0 to 1.0)
    
    bool is_terrible;           // Marked as terrible (too many failures)
    bool is_banned;             // Temporarily banned
    std::chrono::system_clock::time_point ban_until;
    
    AddressEntry() : attempts(0), successes(0), ref_count(0), 
                    success_rate(0.0), is_terrible(false), is_banned(false) {}
    
    void updateSuccessRate();
    bool shouldRetry() const;
    std::chrono::seconds getRetryDelay() const;
};

/**
 * Address manager for peer discovery and selection
 * 
 * Implements Bitcoin's addrman functionality:
 * - Maintains pools of known peer addresses
 * - Tracks connection success/failure statistics
 * - Provides address selection for outbound connections
 * - Handles address advertisement and discovery
 */
class AddressManager {
public:
    AddressManager();
    ~AddressManager();
    
    /**
     * Add new addresses from peer advertisement
     */
    void addAddresses(const std::vector<NetworkAddress>& addresses, 
                     const std::string& source_peer = "");
    
    /**
     * Add single address
     */
    void addAddress(const NetworkAddress& addr, const std::string& source_peer = "");
    
    /**
     * Get addresses for outbound connections
     */
    std::vector<NetworkAddress> getAddresses(size_t count = 1);

    /**
     * Select untried addresses for short-lived feeler connections.
     * Tried entries are deliberately excluded: feelers exist to validate
     * fresh AddrMan entries without replacing durable outbound peers.
     */
    std::vector<NetworkAddress> getNewAddressesForFeeler(size_t count = 1);

    /**
     * Get addresses advertising a given service bit (e.g. NODE_RELAY),
     * spread across distinct /16 subnets so no single network dominates.
     * The caller passes the bit; addrman stays service-agnostic.
     */
    std::vector<NetworkAddress> getAddressesByService(uint64_t service_bit,
                                                      size_t count = 8);

    /**
     * Count routable, non-banned, non-terrible addresses advertising a
     * service bit. Observability-only; selection still uses
     * getAddressesByService().
     */
    size_t countAddressesByService(uint64_t service_bit) const;

    /**
     * Get addresses to advertise to peers
     */
    std::vector<NetworkAddress> getAdvertisableAddresses(size_t count = 1000);
    
    /**
     * Mark connection attempt result
     */
    void markAttempt(const NetworkAddress& addr, bool success);
    
    /**
     * Mark address as good (successful connection)
     */
    void markGood(const NetworkAddress& addr);
    
    /**
     * Mark address as terrible (persistent failures)
     */
    void markTerrible(const NetworkAddress& addr);
    
    /**
     * Ban address temporarily
     */
    void banAddress(const NetworkAddress& addr, std::chrono::seconds duration);
    
    /**
     * Check if address is banned
     */
    bool isBanned(const NetworkAddress& addr) const;
    
    /**
     * Get statistics
     */
    struct AddrmanStats {
        size_t total_addresses;
        size_t new_addresses;      // Untried addresses
        size_t tried_addresses;    // Previously connected addresses
        size_t terrible_addresses; // Marked as terrible
        size_t banned_addresses;   // Currently banned
        double avg_success_rate;
    };
    
    AddrmanStats getStats() const;
    
    /**
     * Maintenance operations
     */
    void performMaintenance();
    void clearBanned();
    void clearTerrible();
    
    /**
     * Persistence
     */
    bool saveToFile(const std::string& filename) const;
    bool loadFromFile(const std::string& filename);
    
    /**
     * Configuration
     */
    void setMaxAddresses(size_t max_new, size_t max_tried);
    void setBanDuration(std::chrono::seconds duration);
    void setTerribleThreshold(uint32_t failures);

private:
    // Address pools
    enum class AddressPool {
        NEW,    // Untried addresses
        TRIED   // Previously connected addresses
    };
    
    // Internal address management
    void moveToTried(const NetworkAddress& addr);
    void evictOldest(AddressPool pool);
    bool shouldEvict(const AddressEntry& entry) const;
    
    // Selection algorithms
    NetworkAddress selectFromNew();
    NetworkAddress selectFromTried();
    double calculatePriority(const AddressEntry& entry) const;
    
    // Validation
    bool isValidAddress(const NetworkAddress& addr) const;
    bool isDuplicate(const NetworkAddress& addr) const;
    
    // Maintenance helpers
    void cleanupExpired();
    void updateStatistics();
    
    mutable std::mutex mutex_;
    
    // Address storage
    std::unordered_map<std::string, AddressEntry> new_addresses_;    // addr_key -> entry
    std::unordered_map<std::string, AddressEntry> tried_addresses_;  // addr_key -> entry
    std::unordered_map<std::string, std::chrono::system_clock::time_point> banned_addresses_;
    
    // Configuration
    size_t max_new_addresses_;
    size_t max_tried_addresses_;
    std::chrono::seconds default_ban_duration_;
    uint32_t terrible_threshold_;
    
    // Statistics
    mutable std::atomic<size_t> total_added_{0};
    mutable std::atomic<size_t> total_attempts_{0};
    mutable std::atomic<size_t> total_successes_{0};
    
    // Random number generation
    mutable std::mt19937 rng_;
    
    // Helper functions
    std::string getAddressKey(const NetworkAddress& addr) const;
    AddressEntry* findAddress(const NetworkAddress& addr);
    const AddressEntry* findAddress(const NetworkAddress& addr) const;
    
    // Helper: Extract /16 subnet from IP address ("192.168.1.100" -> "192.168")
    static std::string extractSubnet16(const std::string& ip);

    // Count addresses from a /16 subnet in a given pool
    size_t countInSubnet16(const std::unordered_map<std::string, AddressEntry>& pool,
                           const std::string& subnet) const;

    // Constants
    static constexpr size_t DEFAULT_MAX_NEW = 20000;
    static constexpr size_t DEFAULT_MAX_TRIED = 5000;
    static constexpr std::chrono::hours DEFAULT_BAN_DURATION{24};
    static constexpr uint32_t DEFAULT_TERRIBLE_THRESHOLD = 10;
    static constexpr std::chrono::hours ADDRESS_EXPIRE_TIME{30 * 24}; // 30 days

    // Eclipse attack prevention: per-/16-subnet address limits
    static constexpr size_t MAX_PER_SUBNET16_NEW = 64;    // Max addresses from same /16 in new pool
    static constexpr size_t MAX_PER_SUBNET16_TRIED = 8;   // Max addresses from same /16 in tried pool
};

// Architecture V3: No globals
// AddressManager is now managed via DaemonContext
// Access via: ctx->address_manager or AddressManagerService

// Deprecated: Use AddressManagerService instead
// extern std::unique_ptr<AddressManager> g_addrman;
// void InitializeAddressManager();
// void ShutdownAddressManager();

} // namespace p2p
} // namespace dinero
