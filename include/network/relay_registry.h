// Copyright (c) 2026 The Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// Relay registry — NAT traversal Phase C3 slice 2.
//
// The relay registry is the in-memory directory a NODE_RELAY-flagged
// daemon maintains of NAT'd peers that have asked it to advertise
// them. Each entry maps a 20-byte node_id → the TCP connection over
// which we'll reach the registered peer + the proven pubkey + a
// hard expiry timestamp.
//
// Why an in-memory directory (not addrman): addrman is IP-keyed and
// stores opinion-of-quality scores. Relay registrations are
// short-lived (≤ 2h) and keyed by node_id, not IP — putting them in
// addrman would conflate two different "what peers do we know about"
// concepts. Keeping it separate also makes the size cap and TTL
// sweep trivial.
//
// What lives WHERE in the relay flow:
//   - This class    : node_id → {pubkey, peer_address, expiry}.
//   - P2PManager    : owns one RelayRegistry; calls Register from
//                     handle_relayreg, Lookup from handle_relaycon,
//                     UnregisterByPeerAddress on connection close.
//   - P2PService    : nothing today — slice 4 will wire client-side
//                     outbound logic that doesn't touch the registry.

#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

namespace dinero::network {

struct RelayRegistration {
    std::array<uint8_t, 20> node_id{};
    std::array<uint8_t, 33> pubkey{};       // peer's proven secp256k1 compressed pubkey
    std::string peer_address;                // TCP connection key in P2PManager::connected_peers_
    std::chrono::steady_clock::time_point expires_at{};
};

class RelayRegistry {
public:
    // Hard caps. Both are deliberately conservative to start; an
    // operator-tunable knob lives in P2PService later.
    static constexpr size_t kMaxRegistrations = 100;
    static constexpr uint32_t kMaxTtlSeconds = 7200;   // 2 hours

    // Insert OR refresh an entry. The caller MUST have already
    // verified the registrant's signature. Returns:
    //   true  — accepted (new entry or refreshed expiry).
    //   false — registry is at kMaxRegistrations and this is a new
    //           node_id (defends against a single peer flooding the
    //           registry with bogus identities; existing-entry
    //           refresh always succeeds).
    bool Register(const RelayRegistration& reg);

    // Return a copy of the registration for node_id, or nullopt if
    // not present OR present-but-expired. Caller doesn't see stale
    // entries; the sweep is opportunistic at Lookup time.
    std::optional<RelayRegistration> Lookup(
        const std::array<uint8_t, 20>& node_id) const;

    // Connection-closed hook: drop every entry whose peer_address
    // matches `peer_address`. Multiple node_ids registered through
    // the same connection are uncommon (one peer = one identity) but
    // the cleanup is idempotent and bounded by entries_.size().
    void UnregisterByPeerAddress(const std::string& peer_address);

    // Periodic maintenance — removes everything past expiry. Returns
    // the number of entries removed (useful for log/metrics).
    size_t Sweep();

    size_t size() const;

private:
    mutable std::mutex mutex_;
    // Keyed on the hex string of node_id since std::array<uint8_t,20>
    // hashing requires a custom Hash struct. Hex is cheap (40 chars)
    // and makes log lines greppable.
    std::unordered_map<std::string, RelayRegistration> entries_;
};

}  // namespace dinero::network
