#pragma once

// Local interface IP enumeration with self-loop filtering.
//
// Used by P2P code to reject outbound connections to the node's own external
// IP and to filter the hardcoded seed/anchor lists. After the v5 genesis reset
// (Apr 12 2026), every fleet daemon dialed itself because MAINNET_SEED_IPS
// contains all four production server IPs — see the comment block on
// LocalInterfaceIps() below for the full incident write-up.
//
// Header-only so both p2p_service.cpp (load-time filter) and p2p_manager.cpp
// (dial-time filter) can use it without CMake changes. Each translation unit
// gets its own cached set, initialized once on first call.

#include <string>
#include <unordered_set>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <unistd.h>
#include <vector>
#endif

namespace dinero {
namespace network {

// Enumerate every IP bound to a local interface and cache the set.
//
// IMPORTANT — Linux sandboxing gotcha:
//   getifaddrs() on Linux uses an AF_NETLINK socket internally. The production
//   fleet runs dinerod under systemd with
//     RestrictAddressFamilies=AF_INET AF_INET6 AF_UNIX
//   which silently makes getifaddrs() return an empty result (the underlying
//   socket(AF_NETLINK,...) call returns -1/EAFNOSUPPORT). On Apr 13 2026 this
//   masked the entire self-IP filter — getifaddrs returned no addresses, the
//   set was empty, IsLocalInterfaceIp always returned false, and the filter
//   became a silent no-op even though the binary contained the new code.
//
//   Mitigation: if getifaddrs() yields no addresses, fall back to SIOCGIFCONF
//   over an AF_INET socket. SIOCGIFCONF uses ioctl on a regular INET socket
//   (already permitted by the systemd sandbox) so it works regardless of
//   AF_NETLINK restrictions. SIOCGIFCONF only enumerates IPv4, but that is
//   sufficient for the fleet's current self-loop case (all four servers'
//   MAINNET_SEED_IPS entries are IPv4 literals).
//
// On Windows the helper returns an empty set so the filter becomes a no-op,
// preserving prior behavior — the production fleet is Linux-only.
inline const std::unordered_set<std::string>& LocalInterfaceIps() {
    static const std::unordered_set<std::string> kLocalIps = []() {
        std::unordered_set<std::string> ips;
#ifndef _WIN32
        // Primary: getifaddrs() — covers IPv4 + IPv6, but needs AF_NETLINK.
        struct ifaddrs* ifap = nullptr;
        if (getifaddrs(&ifap) == 0) {
            for (struct ifaddrs* ifa = ifap; ifa != nullptr; ifa = ifa->ifa_next) {
                if (ifa->ifa_addr == nullptr) {
                    continue;
                }
                char buf[INET6_ADDRSTRLEN] = {};
                if (ifa->ifa_addr->sa_family == AF_INET) {
                    auto* sa = reinterpret_cast<struct sockaddr_in*>(ifa->ifa_addr);
                    if (inet_ntop(AF_INET, &sa->sin_addr, buf, sizeof(buf))) {
                        ips.emplace(buf);
                    }
                } else if (ifa->ifa_addr->sa_family == AF_INET6) {
                    auto* sa6 = reinterpret_cast<struct sockaddr_in6*>(ifa->ifa_addr);
                    if (inet_ntop(AF_INET6, &sa6->sin6_addr, buf, sizeof(buf))) {
                        ips.emplace(buf);
                    }
                }
            }
            freeifaddrs(ifap);
        }

        // Fallback: SIOCGIFCONF on AF_INET socket. Used when getifaddrs()
        // failed or returned nothing (notably under the dinerod systemd
        // sandbox, which blocks AF_NETLINK). IPv4 only.
        if (ips.empty()) {
            int sock = socket(AF_INET, SOCK_DGRAM, 0);
            if (sock >= 0) {
                std::vector<char> buffer(16 * sizeof(struct ifreq));
                struct ifconf ifc{};
                for (int attempt = 0; attempt < 6; ++attempt) {
                    ifc.ifc_len = static_cast<int>(buffer.size());
                    ifc.ifc_buf = buffer.data();
                    if (ioctl(sock, SIOCGIFCONF, &ifc) < 0) {
                        ifc.ifc_len = 0;
                        break;
                    }
                    if (static_cast<size_t>(ifc.ifc_len) + sizeof(struct ifreq) >= buffer.size()) {
                        buffer.resize(buffer.size() * 2);
                        continue;
                    }
                    break;
                }
                const size_t count = static_cast<size_t>(ifc.ifc_len) / sizeof(struct ifreq);
                auto* req_array = reinterpret_cast<struct ifreq*>(buffer.data());
                for (size_t i = 0; i < count; ++i) {
                    if (req_array[i].ifr_addr.sa_family == AF_INET) {
                        char buf[INET_ADDRSTRLEN] = {};
                        auto* sa = reinterpret_cast<struct sockaddr_in*>(&req_array[i].ifr_addr);
                        if (inet_ntop(AF_INET, &sa->sin_addr, buf, sizeof(buf))) {
                            ips.emplace(buf);
                        }
                    }
                }
                close(sock);
            }
        }
#endif
        return ips;
    }();
    return kLocalIps;
}

inline bool IsLocalInterfaceIp(const std::string& host) {
    return LocalInterfaceIps().count(host) > 0;
}

} // namespace network
} // namespace dinero
