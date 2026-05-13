#include "config/seed_nodes.h"
#include "common/logger.h"
#include <thread>
#include <chrono>
#include <random>
#include <algorithm>
#include <set>
#include "compat/net_compat.h"

namespace dinero {
namespace config {

/**
 * Network Bootstrap Manager
 * 
 * Handles reliable connection to the Dinero network with multiple fallback strategies.
 */
class NetworkBootstrap {
public:
    struct BootstrapResult {
        bool success = false;
        int connected_peers = 0;
        std::vector<std::string> connected_addresses;
        std::vector<std::string> failed_addresses;
        std::string error_message;
        int total_attempts = 0;
        int dns_discoveries = 0;
    };
    
    /**
     * Attempt to bootstrap network connectivity
     */
    static BootstrapResult bootstrap(const std::string& network = "mainnet", 
                                   bool enable_dns = true, 
                                   bool enable_upnp = true) {
        BootstrapResult result;
        
        g_logger.info("🌐 Starting network bootstrap for " + network);
        
        // Get seed nodes for the network
        const auto& seeds = getSeedNodes(network);
        if (seeds.empty()) {
            result.error_message = "No seed nodes configured for network: " + network;
            g_logger.error(result.error_message);
            return result;
        }
        
        // Strategy 1: Try primary seed nodes first
        result = tryPrimarySeeds(seeds);
        if (result.success) {
            g_logger.info("✅ Bootstrap successful via primary seeds");
            return result;
        }
        
        // Strategy 2: Try all seed nodes with geographic optimization
        result = tryAllSeeds(seeds);
        if (result.success) {
            g_logger.info("✅ Bootstrap successful via seed nodes");
            return result;
        }
        
        // Strategy 3: DNS seed discovery
        if (enable_dns) {
            auto dns_result = tryDnsSeeds(network);
            result.dns_discoveries = dns_result.dns_discoveries;
            if (dns_result.success) {
                result.success = true;
                result.connected_peers += dns_result.connected_peers;
                result.connected_addresses.insert(result.connected_addresses.end(),
                                                dns_result.connected_addresses.begin(),
                                                dns_result.connected_addresses.end());
                g_logger.info("✅ Bootstrap successful via DNS seeds");
                return result;
            }
        }
        
        // Strategy 4: UPnP discovery (local network)
        if (enable_upnp) {
            auto upnp_result = tryUpnpDiscovery();
            if (upnp_result.success) {
                result.success = true;
                result.connected_peers += upnp_result.connected_peers;
                result.connected_addresses.insert(result.connected_addresses.end(),
                                                upnp_result.connected_addresses.begin(),
                                                upnp_result.connected_addresses.end());
                g_logger.info("✅ Bootstrap successful via UPnP discovery");
                return result;
            }
        }
        
        // All strategies failed
        result.error_message = "All bootstrap strategies failed. Network may be down or unreachable.";
        g_logger.error(result.error_message);
        
        return result;
    }
    
private:
    struct ResolvedEndpoint {
        sockaddr_storage addr{};
        socklen_t addr_len = 0;
        int family = AF_UNSPEC;
        std::string numeric_host;
    };

    static std::vector<ResolvedEndpoint> resolveHost(const std::string& hostname, uint16_t port) {
        std::vector<ResolvedEndpoint> endpoints;

        struct addrinfo hints {};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
#ifdef AI_ADDRCONFIG
        hints.ai_flags = AI_ADDRCONFIG;
#endif

        struct addrinfo* result = nullptr;
        const std::string port_str = std::to_string(port);
        if (getaddrinfo(hostname.c_str(), port_str.c_str(), &hints, &result) != 0) {
            return endpoints;
        }

        for (auto* rp = result; rp != nullptr; rp = rp->ai_next) {
            if (!rp->ai_addr || rp->ai_addrlen <= 0) {
                continue;
            }

            ResolvedEndpoint endpoint;
            endpoint.family = rp->ai_family;
            endpoint.addr_len = static_cast<socklen_t>(rp->ai_addrlen);
            std::memcpy(&endpoint.addr, rp->ai_addr, rp->ai_addrlen);

            char host_buffer[NI_MAXHOST] = {0};
            if (getnameinfo(
                    rp->ai_addr,
                    static_cast<socklen_t>(rp->ai_addrlen),
                    host_buffer,
                    sizeof(host_buffer),
                    nullptr,
                    0,
                    NI_NUMERICHOST) == 0) {
                endpoint.numeric_host = host_buffer;
            }

            endpoints.push_back(std::move(endpoint));
        }

        freeaddrinfo(result);
        return endpoints;
    }

    /**
     * Try connecting to primary seed nodes only
     */
    static BootstrapResult tryPrimarySeeds(const std::vector<SeedNode>& seeds) {
        BootstrapResult result;
        
        // Filter to primary seeds only
        std::vector<SeedNode> primary_seeds;
        std::copy_if(seeds.begin(), seeds.end(), std::back_inserter(primary_seeds),
                    [](const SeedNode& seed) { return seed.is_primary; });
        
        if (primary_seeds.empty()) {
            result.error_message = "No primary seed nodes available";
            return result;
        }
        
        g_logger.info("Trying " + std::to_string(primary_seeds.size()) + " primary seed nodes");
        
        // Shuffle for load balancing
        std::random_device rd;
        std::mt19937 gen(rd());
        std::shuffle(primary_seeds.begin(), primary_seeds.end(), gen);
        
        // Try each primary seed
        for (const auto& seed : primary_seeds) {
            result.total_attempts++;
            
            if (testConnection(seed.hostname, seed.port)) {
                result.connected_peers++;
                result.connected_addresses.push_back(seed.getAddress());
                g_logger.info("✅ Connected to primary seed: " + seed.getAddress());
                
                // Success if we have minimum peers
                if (result.connected_peers >= BootstrapConfig::MIN_PEERS) {
                    result.success = true;
                    return result;
                }
            } else {
                result.failed_addresses.push_back(seed.getAddress());
                g_logger.warning("❌ Failed to connect to primary seed: " + seed.getAddress());
            }
        }
        
        return result;
    }
    
    /**
     * Try connecting to all seed nodes with geographic optimization
     */
    static BootstrapResult tryAllSeeds(const std::vector<SeedNode>& seeds) {
        BootstrapResult result;
        
        g_logger.info("Trying all " + std::to_string(seeds.size()) + " seed nodes");
        
        // Get geographically optimized seed order
        auto optimized_seeds = getClosestSeeds(seeds);
        
        // Try each seed with staggered connections
        for (size_t i = 0; i < optimized_seeds.size() && i < BootstrapConfig::MAX_SEED_CONNECTIONS; i++) {
            const auto& seed = optimized_seeds[i];
            result.total_attempts++;
            
            // Stagger connections to avoid overwhelming seeds
            if (i > 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
            }
            
            if (testConnection(seed.hostname, seed.port)) {
                result.connected_peers++;
                result.connected_addresses.push_back(seed.getAddress());
                g_logger.info("✅ Connected to seed: " + seed.getAddress() + " (" + seed.region + ")");
                
                // Success if we have minimum peers
                if (result.connected_peers >= BootstrapConfig::MIN_PEERS) {
                    result.success = true;
                    return result;
                }
            } else {
                result.failed_addresses.push_back(seed.getAddress());
                g_logger.warning("❌ Failed to connect to seed: " + seed.getAddress());
            }
        }
        
        return result;
    }
    
    /**
     * Try DNS seed discovery
     */
    static BootstrapResult tryDnsSeeds(const std::string& network) {
        BootstrapResult result;
        
        auto dns_seeds = getDnsSeeds(network);
        if (dns_seeds.empty()) {
            result.error_message = "No DNS seeds configured for network: " + network;
            return result;
        }
        
        g_logger.info("Trying DNS seed discovery with " + std::to_string(dns_seeds.size()) + " DNS seeds");
        
        for (const auto& dns_seed : dns_seeds) {
            auto discovered_nodes = resolveDnsSeed(dns_seed);
            result.dns_discoveries += discovered_nodes.size();
            
            for (const auto& node_address : discovered_nodes) {
                result.total_attempts++;
                
                // Parse address (format: "ip:port")
                size_t colon_pos = node_address.find(':');
                if (colon_pos == std::string::npos) continue;
                
                std::string ip = node_address.substr(0, colon_pos);
                uint16_t port = static_cast<uint16_t>(std::stoi(node_address.substr(colon_pos + 1)));
                
                if (testConnection(ip, port)) {
                    result.connected_peers++;
                    result.connected_addresses.push_back(node_address);
                    g_logger.info("✅ Connected to DNS-discovered node: " + node_address);
                    
                    // Success if we have minimum peers
                    if (result.connected_peers >= BootstrapConfig::MIN_PEERS) {
                        result.success = true;
                        return result;
                    }
                } else {
                    result.failed_addresses.push_back(node_address);
                }
            }
        }
        
        return result;
    }
    
    /**
     * Try UPnP discovery for local network nodes
     */
    static BootstrapResult tryUpnpDiscovery() {
        BootstrapResult result;
        
        g_logger.info("Attempting UPnP discovery for local network nodes");
        
        // Simple local network scan (192.168.x.x, 10.x.x.x)
        std::vector<std::string> local_ranges = {
            "192.168.1.", "192.168.0.", "10.0.0.", "10.0.1."
        };
        
        for (const auto& range : local_ranges) {
            for (int i = 1; i < 255 && result.connected_peers < BootstrapConfig::MIN_PEERS; i++) {
                std::string ip = range + std::to_string(i);
                result.total_attempts++;
                
                // Try common Dinero ports
                for (uint16_t port : {23999, 20999, 18333}) {
                    if (testConnection(ip, port, 1000)) { // Short timeout for local
                        result.connected_peers++;
                        result.connected_addresses.push_back(ip + ":" + std::to_string(port));
                        g_logger.info("✅ Found local node: " + ip + ":" + std::to_string(port));
                        
                        if (result.connected_peers >= BootstrapConfig::MIN_PEERS) {
                            result.success = true;
                            return result;
                        }
                        break; // Found node on this IP, try next IP
                    }
                }
            }
        }
        
        return result;
    }
    
    /**
     * Test connection to a specific node
     */
    static bool testConnection(const std::string& hostname, uint16_t port, int timeout_ms = BootstrapConfig::CONNECTION_TIMEOUT_MS) {
        const auto endpoints = resolveHost(hostname, port);
        for (const auto& endpoint : endpoints) {
            int sock = socket(endpoint.family, SOCK_STREAM, 0);
            if (sock < 0) {
                continue;
            }

            compat_set_nonblocking(sock);
            int result = connect(sock, reinterpret_cast<const struct sockaddr*>(&endpoint.addr), endpoint.addr_len);

            if (result == 0) {
                COMPAT_CLOSE_SOCKET(sock);
                return true;
            }

#ifdef _WIN32
            if (WSAGetLastError() == WSAEWOULDBLOCK) {
#else
            if (errno == EINPROGRESS) {
#endif
                fd_set write_fds;
                FD_ZERO(&write_fds);
                FD_SET(sock, &write_fds);

                struct timeval tv;
                tv.tv_sec = timeout_ms / 1000;
                tv.tv_usec = (timeout_ms % 1000) * 1000;

                int select_result = select(sock + 1, nullptr, &write_fds, nullptr, &tv);

                if (select_result > 0 && FD_ISSET(sock, &write_fds)) {
                    int error = 0;
                    socklen_t len = sizeof(error);
                    getsockopt(sock, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&error), &len);
                    COMPAT_CLOSE_SOCKET(sock);
                    if (error == 0) {
                        return true;
                    }
                    continue;
                }
            }

            COMPAT_CLOSE_SOCKET(sock);
        }

        return false;
    }
    
    /**
     * Resolve DNS seed to get node addresses
     */
    static std::vector<std::string> resolveDnsSeed(const std::string& dns_seed) {
        std::vector<std::string> addresses;
        std::set<std::string> unique_addresses;

        for (const auto& endpoint : resolveHost(dns_seed, 23999)) {
            if (endpoint.numeric_host.empty()) {
                continue;
            }
            unique_addresses.insert(endpoint.numeric_host + ":23999");
        }

        addresses.assign(unique_addresses.begin(), unique_addresses.end());
        g_logger.info("DNS seed " + dns_seed + " resolved to " + std::to_string(addresses.size()) + " addresses");
        return addresses;
    }
};

// Implementation of header functions
bool validateSeedNode(const SeedNode& seed, int timeout_ms) {
    return NetworkBootstrap::testConnection(seed.hostname, seed.port, timeout_ms);
}

std::vector<SeedNode> getClosestSeeds(const std::vector<SeedNode>& seeds, const std::string& preferred_region) {
    std::vector<SeedNode> result = seeds;
    
    // Simple geographic optimization - prioritize by region
    std::sort(result.begin(), result.end(), [&preferred_region](const SeedNode& a, const SeedNode& b) {
        // Primary seeds first
        if (a.is_primary != b.is_primary) {
            return a.is_primary > b.is_primary;
        }
        
        // Preferred region next
        if (!preferred_region.empty()) {
            bool a_preferred = a.region == preferred_region;
            bool b_preferred = b.region == preferred_region;
            if (a_preferred != b_preferred) {
                return a_preferred > b_preferred;
            }
        }
        
        // Then by region name (for consistent ordering)
        return a.region < b.region;
    });
    
    return result;
}

} // namespace config
} // namespace dinero
