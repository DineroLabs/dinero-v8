#pragma once

#include <string>
#include <vector>
#include <cstdint>

namespace dinero {
namespace p2p {

/**
 * DNS Resolution for Seed Nodes
 *
 * Resolves hostnames to IP addresses for P2P network bootstrap.
 * Supports both IPv4 and IPv6 with timeout and error handling.
 */
class DNSResolver {
public:
    struct ResolvedAddress {
        std::string ip;
        uint16_t port;
        bool is_ipv6;

        std::string to_string() const {
            return is_ipv6 ? "[" + ip + "]:" + std::to_string(port) : ip + ":" + std::to_string(port);
        }
    };

    /**
     * Resolve a hostname to IP addresses
     *
     * @param hostname DNS hostname to resolve (e.g., "seed1.dinero-coin.com")
     * @param port Port number to associate with resolved IPs
     * @param timeout_ms Timeout in milliseconds (default: 5000ms)
     * @param max_results Maximum number of IPs to return (default: 8)
     * @return Vector of resolved addresses (empty if resolution failed)
     */
    static std::vector<ResolvedAddress> resolve(
        const std::string& hostname,
        uint16_t port,
        int timeout_ms = 5000,
        int max_results = 8
    );

    /**
     * Check if a string is already an IP address (no resolution needed)
     */
    static bool isIPAddress(const std::string& str);

    /**
     * Resolve DNS seeds and return IP addresses
     *
     * @param dns_seeds Vector of DNS seed hostnames
     * @param port Port to use for all resolved IPs
     * @param max_per_seed Maximum IPs to resolve per seed (default: 8)
     * @return Vector of all resolved addresses
     */
    static std::vector<ResolvedAddress> resolveSeeds(
        const std::vector<std::string>& dns_seeds,
        uint16_t port = 23999,
        int max_per_seed = 8
    );
};

} // namespace p2p
} // namespace dinero
