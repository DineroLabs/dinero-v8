// Copyright (c) 2026 The Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// Unit test for AddressManager::getAddressesByService (relay-capable
// peer selection) and the addAddresses dup-update path that lets a
// peer's service flags reach addrman on re-advertisement.

#include "p2p/addrman.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <vector>

using dinero::p2p::AddressManager;
using dinero::p2p::NetworkAddress;

// Caller passes the bit; addrman is service-agnostic. NODE_RELAY = 1<<26.
static constexpr uint64_t kNodeRelay = 1ULL << 26;
static constexpr uint64_t kNodeNetwork = 1ULL << 0;

static int g_fails = 0;
static void check(bool cond, const char* what) {
    std::printf("  %s %s\n", cond ? "PASS" : "FAIL", what);
    if (!cond) ++g_fails;
}

static NetworkAddress mk(const char* ip, uint16_t port, uint64_t services) {
    NetworkAddress a;
    a.ip = ip;
    a.port = port;
    a.services = services;
    a.timestamp = std::chrono::system_clock::now();
    return a;
}

static bool has(const std::vector<NetworkAddress>& v, const char* ip) {
    for (const auto& a : v) {
        if (a.ip == ip) return true;
    }
    return false;
}

int main() {
    // Selection: only NODE_RELAY peers, spread across distinct /16s.
    {
        AddressManager am;
        am.addAddress(mk("8.8.10.1", 20999, kNodeRelay));
        am.addAddress(mk("8.8.20.2", 20999, kNodeRelay));   // same /16 as above
        am.addAddress(mk("9.9.30.3", 20999, kNodeRelay));
        am.addAddress(mk("1.1.40.4", 20999, kNodeRelay));
        am.addAddress(mk("13.13.50.5", 20999, kNodeNetwork)); // not a relay

        const auto relays = am.getAddressesByService(kNodeRelay, 8);
        check(!has(relays, "13.13.50.5"), "non-relay address is excluded");
        check(am.countAddressesByService(kNodeRelay) == 4,
              "service count includes all routable relay addresses before /16 selection");
        check(relays.size() == 3,
              "one relay per distinct /16 (the two 8.8.x collapse to one)");
        int n88 = 0;
        for (const auto& a : relays) {
            if (a.ip.rfind("8.8.", 0) == 0) ++n88;
        }
        check(n88 == 1, "/16 spread: only one of the two 8.8.x relays returned");
        check(has(relays, "9.9.30.3") && has(relays, "1.1.40.4"),
              "the distinct-/16 relays are present");
    }

    // Dup-update: a re-advertisement carrying real service flags updates
    // the stored entry; a services=0 (legacy/unknown) re-advert must not
    // wipe flags already known.
    {
        AddressManager am;
        am.addAddress(mk("13.13.60.6", 20999, 0));  // services unknown
        check(am.getAddressesByService(kNodeRelay, 8).empty(),
              "services=0 -> not selected as a relay");

        am.addAddress(mk("13.13.60.6", 20999, kNodeRelay));  // re-advert w/ relay bit
        check(am.countAddressesByService(kNodeRelay) == 1,
              "dup-update: relay count sees re-advertised NODE_RELAY");
        check(has(am.getAddressesByService(kNodeRelay, 8), "13.13.60.6"),
              "dup-update: re-advert with NODE_RELAY makes the peer discoverable");

        am.addAddress(mk("13.13.60.6", 20999, 0));  // legacy re-advert, services unknown
        check(has(am.getAddressesByService(kNodeRelay, 8), "13.13.60.6"),
              "dup-update: a services=0 re-advert does NOT wipe known NODE_RELAY");
    }

    if (g_fails) {
        std::printf("\n%d CHECK(S) FAILED\n", g_fails);
        return 1;
    }
    std::printf("\nALL ADDRMAN RELAY-SELECT CHECKS PASSED\n");
    return 0;
}
