#include "p2p/dns_resolver.h"
#include <iostream>
#include <cstring>

// Platform-specific includes
#ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
#else
    #include <sys/types.h>
    #include <sys/socket.h>
    #include <netdb.h>
    #include <arpa/inet.h>
#endif

namespace dinero {
namespace p2p {

std::vector<DNSResolver::ResolvedAddress> DNSResolver::resolve(
    const std::string& hostname,
    uint16_t port,
    int timeout_ms,
    int max_results
) {
    std::vector<ResolvedAddress> results;

    // If it's already an IP address, return it directly
    if (isIPAddress(hostname)) {
        ResolvedAddress addr;
        addr.ip = hostname;
        addr.port = port;
        addr.is_ipv6 = (hostname.find(':') != std::string::npos);
        results.push_back(addr);
        return results;
    }

    // Perform DNS resolution
    struct addrinfo hints{}, *result = nullptr;
    hints.ai_family = AF_UNSPEC;  // Allow IPv4 or IPv6
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    std::cout << "[DNS] Resolving hostname: " << hostname << std::endl;

    int status = getaddrinfo(hostname.c_str(), nullptr, &hints, &result);
    if (status != 0) {
        std::cerr << "[DNS] Failed to resolve " << hostname << ": "
                  << gai_strerror(status) << std::endl;
        return results;
    }

    // Extract IP addresses from result
    int count = 0;
    for (auto* rp = result; rp != nullptr && count < max_results; rp = rp->ai_next) {
        ResolvedAddress addr;
        addr.port = port;

        char ip_str[INET6_ADDRSTRLEN];

        if (rp->ai_family == AF_INET) {
            // IPv4
            struct sockaddr_in* ipv4 = reinterpret_cast<struct sockaddr_in*>(rp->ai_addr);
            inet_ntop(AF_INET, &(ipv4->sin_addr), ip_str, INET_ADDRSTRLEN);
            addr.ip = ip_str;
            addr.is_ipv6 = false;
            results.push_back(addr);
            count++;
            std::cout << "[DNS] Resolved " << hostname << " -> " << ip_str << " (IPv4)" << std::endl;

        } else if (rp->ai_family == AF_INET6) {
            // IPv6
            struct sockaddr_in6* ipv6 = reinterpret_cast<struct sockaddr_in6*>(rp->ai_addr);
            inet_ntop(AF_INET6, &(ipv6->sin6_addr), ip_str, INET6_ADDRSTRLEN);
            addr.ip = ip_str;
            addr.is_ipv6 = true;
            results.push_back(addr);
            count++;
            std::cout << "[DNS] Resolved " << hostname << " -> " << ip_str << " (IPv6)" << std::endl;
        }
    }

    freeaddrinfo(result);

    if (results.empty()) {
        std::cerr << "[DNS] No addresses resolved for " << hostname << std::endl;
    }

    return results;
}

bool DNSResolver::isIPAddress(const std::string& str) {
    // Try parsing as IPv4
    struct in_addr addr4;
    if (inet_pton(AF_INET, str.c_str(), &addr4) == 1) {
        return true;
    }

    // Try parsing as IPv6
    struct in6_addr addr6;
    if (inet_pton(AF_INET6, str.c_str(), &addr6) == 1) {
        return true;
    }

    return false;
}

std::vector<DNSResolver::ResolvedAddress> DNSResolver::resolveSeeds(
    const std::vector<std::string>& dns_seeds,
    uint16_t port,
    int max_per_seed
) {
    std::vector<ResolvedAddress> all_resolved;

    std::cout << "[DNS] Resolving " << dns_seeds.size() << " DNS seeds..." << std::endl;

    for (const auto& seed : dns_seeds) {
        auto resolved = resolve(seed, port, 5000, max_per_seed);

        // Add resolved addresses to results
        all_resolved.insert(all_resolved.end(), resolved.begin(), resolved.end());

        std::cout << "[DNS] Seed " << seed << " resolved to "
                  << resolved.size() << " addresses" << std::endl;
    }

    std::cout << "[DNS] Total resolved addresses: " << all_resolved.size() << std::endl;

    return all_resolved;
}

} // namespace p2p
} // namespace dinero
