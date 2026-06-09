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
    // Active production fleet (high availability). Port set dynamically from
    // chainparams. LA/VA/CN retired 2026-06; replaced by DineroNA + DineroEU1.
    {"173.249.200.59", 0, "us-west", true},    // DineroSJ  - San Jose (archival)
    {"172.93.167.32", 0, "us-east", true},     // DineroNA  - North America (archival)
    {"92.118.190.62", 0, "eu", true},          // DineroEU1 - Europe (archival)
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
    // 3 stable full-archival bridges, geographically diverse. DineroLA is omitted
    // (filling up / being retired) so anchors stay on long-lived nodes.
    {"173.249.200.59", 0, "us-west", true},    // DineroSJ  - San Jose (archival)
    {"172.93.167.32", 0, "us-east", true},     // DineroNA  - North America (archival)
    {"92.118.190.62", 0, "eu", true},          // DineroEU1 - Europe (archival)
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
 * PRIMARY domain: dinerolabs.org (since 2026-06). Retired LA/VA/CN-era seed
 * names are deliberately omitted so new binaries never bootstrap to dead nodes.
 */
const std::vector<std::string> DNS_SEEDS = {
    // PRIMARY: dinerolabs.org named per-node seed subdomains — each a single-A
    // record for one active fleet node (seed->EU1, seed2->SJ, seed3->NA).
    // `seed1` pointed at retired LA and must not be compiled into new clients.
    "seed.dinerolabs.org",
    "seed2.dinerolabs.org",
    "seed3.dinerolabs.org",
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
 *    - Canonical domain: dinerolabs.org — named per-node subdomains seed/seed1/
 *      seed2/seed3 (single-A each), all queried in DNS_SEEDS. dinero-coin.com is
 *      kept as a fallback for already-deployed binaries.
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
