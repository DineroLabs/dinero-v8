// Copyright (c) 2026 The Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
//
// relayhn2: "node X can be reached through relay R until T", signed by X
// itself. Unlike the legacy relayhnt, nobody but X can vouch for X, so the
// hint can be forwarded past X's direct peers (bounded by a hop limit,
// per-hint dedup and a token bucket) and any node can find any NAT'd node.
//
// Wire payload (one hint):
//   node_id_20 | pubkey_33 | net_1 | addr_len_1 | addr | port_2BE |
//   expiry_u32LE | hop_1 | sig_len_1 | sig_DER
// sig = NodeIdentity signature of
//   "dinero-relay-hint-v2" | node_id | net | addr_len | addr | port_BE | expiry_LE
// The hop count is not signed: each forwarder increments it.

#pragma once

#include "p2p/addr_v2.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <span>
#include <tuple>
#include <map>
#include <vector>

#include "network/token_bucket.h"

namespace dinero::daemon {
class NodeIdentity;
}

namespace dinero::p2p {

struct HintFields {
    std::array<uint8_t, 20> node_id{};
    std::array<uint8_t, 33> pubkey{};
    NetworkType relay_net{NetworkType::IPV4};
    std::vector<uint8_t> relay_addr;  // per-network bytes (addrv2)
    uint16_t relay_port{0};
    uint32_t expiry{0};  // unix seconds
    uint8_t hop{0};
};

// Forwarded while the received hop count is below this.
constexpr uint8_t kHintMaxHops = 3;
// A hint further in the future than this is refused (targets sign 30 min).
constexpr uint32_t kHintMaxLifetimeSeconds = 3600;

// Signs `fields` with `identity` (node_id and pubkey are taken from the
// identity) and returns the wire payload; empty if signing fails.
std::vector<uint8_t> SignHint(const dinero::daemon::NodeIdentity& identity,
                              const HintFields& fields);

// The hint, if the payload parses, HASH160(pubkey) == node_id, the
// signature is valid, now < expiry <= now + kHintMaxLifetimeSeconds, and the
// relay address is a public one (nodes dial it): no loopback, private,
// link-local, CGNAT, multicast or unspecified address unless
// `allow_private_relay` (regtest and tests).
std::optional<HintFields> VerifyHint(std::span<const uint8_t> payload, uint32_t now,
                                     bool allow_private_relay = false);

// True for an IPv4/IPv6 address that is reachable on the public internet.
bool IsPublicRelayAddress(NetworkType net, const std::vector<uint8_t>& addr);

// The same payload with its (unsigned) hop count replaced.
std::vector<uint8_t> WithHop(std::span<const uint8_t> payload, uint8_t hop);

// Decides whether a verified hint is forwarded: once per (target, relay,
// expiry), only while hop < kHintMaxHops, and at most `per_minute` a minute.
class HintForwarder {
public:
    explicit HintForwarder(double per_minute = 50.0);
    bool ShouldForward(const HintFields& hint, uint32_t now_unix,
                       std::chrono::steady_clock::time_point now);

private:
    using Key = std::tuple<std::array<uint8_t, 20>, uint8_t, std::vector<uint8_t>, uint16_t,
                           uint32_t>;
    std::map<Key, uint32_t> seen_;  // -> expiry, pruned once past (at most once a minute)
    dinero::network::TokenBucket bucket_;
    std::chrono::steady_clock::time_point last_prune_{};
};

}  // namespace dinero::p2p
