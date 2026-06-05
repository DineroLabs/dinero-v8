#pragma once
#include <string>
#include <vector>
#include <cstdint>

namespace dinero {
namespace config {

/**
 * Dinero Network Seed Nodes
 * 
 * These are the official seed nodes for the Dinero network.
 * They provide initial peer discovery and network bootstrap.
 */

struct SeedNode {
    std::string hostname;
    uint16_t port;
    std::string region;        // Geographic region for latency optimization
    bool is_primary;           // Primary vs backup seed
    
    SeedNode(const std::string& host, uint16_t p, const std::string& r = "global", bool primary = true)
        : hostname(host), port(p), region(r), is_primary(primary) {}
    
    std::string getAddress() const {
        return hostname + ":" + std::to_string(port);
    }
};

/**
 * Production Seed Nodes (Mainnet)
 *
 * Official Dinero network seed nodes for automatic peer discovery.
 * NOTE: Ports are dynamically resolved from ChainParams.p2p_port
 * Updated: 2025-11-05 - Dynamic port resolution
 */
const std::vector<SeedNode> MAINNET_SEED_IPS = {
    // Primary production seeds (high availability)
    // Port will be set dynamically from chainparams
    {"172.93.160.131", 0, "us-west", true},    // LA Server - Los Angeles
    {"173.249.195.59", 0, "us-east", true},    // VA Server - Virginia
    {"72.18.214.120", 0, "us-central", true},  // MO Server - Missouri
    {"96.9.226.98", 0, "ca-east", true},       // CN Server - Canada
};

/**
 * Anchor Peers (Eclipse Attack Recovery)
 *
 * Anchor peers are a subset of seed nodes that receive special treatment:
 * - Reserved outbound slots (not counted against max_outbound)
 * - Never evicted from connection manager
 * - Automatically reconnected on disconnect
 * - Survive node restarts (persisted separately from peers.dat)
 *
 * This prevents eclipse attacks by guaranteeing connectivity to known-good nodes.
 * Bitcoin Core reserves 2 anchor connections; we reserve 3 (geographic diversity).
 */
const std::vector<SeedNode> MAINNET_ANCHOR_PEERS = {
    {"173.249.195.59", 0, "us-east", true},    // VA Server - Virginia
    {"172.93.160.131", 0, "us-west", true},     // LA Server - Los Angeles
    {"72.18.214.120", 0, "us-central", true},   // MO Server - Missouri
};

/**
 * Testnet Seed Node IPs
 * Ports resolved from ChainParams
 */
const std::vector<SeedNode> TESTNET_SEED_IPS = {
    // testnet seeds disabled
};

/**
 * Regtest Seed Node IPs (Local Development)
 * Ports resolved from ChainParams
 */
const std::vector<SeedNode> REGTEST_SEED_IPS = {
    {"127.0.0.1", 0, "local", true},
    {"localhost", 0, "local", true},
};

/**
 * DNS Seeds (for additional peer discovery)
 *
 * These DNS names return A records with IP addresses of active Dinero nodes.
 * This provides decentralized peer discovery beyond hardcoded seed nodes.
 *
 * PRODUCTION ACTIVE (October 27, 2025):
 * - seed1.dinero-coin.com → 172.93.160.131 (California)
 * - seed2.dinero-coin.com → 173.249.195.59 (Virginia)
 */
const std::vector<std::string> DNS_SEEDS = {
    // dinerolabs.org migration: single multi-A hostname returns the full fleet
    // (LA/VA/MO/CN/SJ). This is the live DNS-seed path (getDnsSeeds()).
    "seed.dinerolabs.org",
    // dinero-coin.com retained for back-compat with already-deployed nodes.
    // (seed3/seed4 were previously absent here while present in DNS + chainparams,
    //  so MO/CN were never queried as DNS seeds — restored for full redundancy.)
    "seed1.dinero-coin.com",
    "seed2.dinero-coin.com",
    "seed3.dinero-coin.com",
    "seed4.dinero-coin.com",
};

/**
 * Bootstrap Configuration
 */
struct BootstrapConfig {
    static constexpr int MIN_PEERS = 3;              // Minimum peers before considering connected
    static constexpr int MAX_SEED_CONNECTIONS = 8;   // Max simultaneous seed connections
    static constexpr int CONNECTION_TIMEOUT_MS = 15000;  // 15 second timeout
    static constexpr int RETRY_INTERVAL_MS = 30000;  // 30 second retry interval
    static constexpr int MAX_RETRIES = 5;            // Max connection retries per seed
    static constexpr int DNS_TIMEOUT_MS = 10000;     // 10 second DNS timeout
    static constexpr bool ENABLE_UPnP = true;        // Enable UPnP by default
    static constexpr bool ENABLE_DNS_SEEDS = true;   // Enable DNS seed discovery
};

/**
 * Get seed nodes for the specified network with dynamic port resolution
 *
 * This function returns seed nodes with ports resolved from ChainParams.
 * This ensures port configuration is centralized in chainparams_impl.cpp
 */
inline std::vector<SeedNode> getSeedNodes(const std::string& network = "mainnet") {
    // Select the appropriate seed IP list
    const std::vector<SeedNode>* seed_ips;
    if (network == "testnet") {
        seed_ips = &TESTNET_SEED_IPS;
    } else if (network == "regtest") {
        seed_ips = &REGTEST_SEED_IPS;
    } else {
        seed_ips = &MAINNET_SEED_IPS;
    }

    // Create a copy with ports filled in from ChainParams
    // Note: This requires ChainParams to be selected before calling this function
    std::vector<SeedNode> seeds_with_ports;
    seeds_with_ports.reserve(seed_ips->size());

    for (const auto& seed_ip : *seed_ips) {
        SeedNode seed = seed_ip;
        // Port will be set by the caller using ChainParams.p2p_port
        // Here we just copy with port=0 as a placeholder
        seeds_with_ports.push_back(seed);
    }

    return seeds_with_ports;
}

/**
 * Get anchor peers for the specified network
 * Anchor peers get reserved outbound slots and are never evicted
 */
inline std::vector<SeedNode> getAnchorPeers(const std::string& network = "mainnet") {
    if (network == "mainnet") {
        return MAINNET_ANCHOR_PEERS;
    }
    return {};  // No anchors for testnet/regtest
}

/**
 * Check if an IP address is an anchor peer
 */
inline bool isAnchorPeer(const std::string& ip, const std::string& network = "mainnet") {
    if (network != "mainnet") return false;
    for (const auto& anchor : MAINNET_ANCHOR_PEERS) {
        if (anchor.hostname == ip) return true;
    }
    return false;
}

/**
 * Get DNS seeds for the specified network
 */
inline std::vector<std::string> getDnsSeeds(const std::string& network = "mainnet") {
    if (network == "testnet") {
        return {};
    } else if (network == "regtest") {
        return {}; // No DNS seeds for regtest
    } else {
        return DNS_SEEDS;
    }
}

/**
 * Validate seed node connectivity
 */
bool validateSeedNode(const SeedNode& seed, int timeout_ms = 5000);

/**
 * Get geographically closest seed nodes
 */
std::vector<SeedNode> getClosestSeeds(const std::vector<SeedNode>& seeds, const std::string& preferred_region = "");

} // namespace config
} // namespace dinero

/**
 * PRODUCTION DEPLOYMENT NOTES:
 * 
 * 1. **Domain & DNS**:
 *    - Use dinero-coin.com as the canonical domain
 *    - Set up DNS A records pointing to actual seed node IPs
 *    - Configure DNS seeds to return active node IPs
 * 
 * 2. **Seed Node Infrastructure**:
 *    - Deploy seed nodes on multiple cloud providers (AWS, GCP, Azure)
 *    - Use different geographic regions for redundancy
 *    - Implement health monitoring and auto-failover
 * 
 * 3. **Security Considerations**:
 *    - Use DDoS protection for seed nodes
 *    - Implement rate limiting and connection throttling
 *    - Monitor for malicious nodes and blacklist as needed
 * 
 * 4. **Community Nodes**:
 *    - Encourage community members to run seed nodes
 *    - Provide incentives for reliable seed node operators
 *    - Implement reputation system for seed node quality
 * 
 * 5. **Monitoring & Maintenance**:
 *    - Monitor seed node uptime and connectivity
 *    - Update seed node list based on network health
 *    - Provide fallback mechanisms for seed node failures
 */
