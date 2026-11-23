#pragma once
// The socket side of btc_p2p.h: one short conversation with one Bitcoin peer
// (docs/design/din-btc-swaps-without-bitcoin-core.md, section 3.1).
//
// RunBtcPeer connects, shakes hands, brings the header chain up to what the
// peer has, announces the transactions it was given, serves them when the
// peer asks, and hangs up. Everything the peer says goes through
// BtcPeerSession and BtcHeaderChain; this file adds only TCP and time limits.
//
// Blocking, with a limit on every wait: connecting, each read, and the whole
// conversation. It never throws; the result says what happened.

#include "wallet/swap/btc_p2p.h"

#include <cstdint>
#include <string>
#include <vector>

namespace dinero::swap {

struct BtcPeerEndpoint {
    std::string address;  // numeric IPv4 or IPv6
    uint16_t port{};
};

// Looks the host names up (DNS seeds answer with many peers) and returns up to
// `limit` distinct endpoints, interleaved across the hosts. Numeric addresses
// pass through. Unresolvable names are skipped.
std::vector<BtcPeerEndpoint> ResolveBtcPeers(const std::vector<std::string>& hosts, uint16_t port, size_t limit);

struct BtcPeerOptions {
    uint32_t connect_timeout_seconds{8};
    uint32_t idle_timeout_seconds{20};    // longest silence tolerated while we wait for an answer
    uint32_t total_timeout_seconds{180};  // the whole conversation
    uint32_t linger_seconds{10};          // how long to wait for the peer to fetch what we announced
};

struct BtcPeerOutcome {
    bool connected{false};
    bool handshake{false};
    bool headers_synced{false};          // the peer had nothing after our tip when we hung up
    size_t headers_accepted{0};
    std::vector<Bytes32> txs_sent;       // announced transactions the peer fetched (txid, wire order)
    int32_t peer_height{0};              // what the peer claims; informational only
    std::string error;                   // empty: the conversation ended as planned
};

BtcPeerOutcome RunBtcPeer(const BtcPeerEndpoint& peer, const BtcP2pNetwork& net, BtcHeaderChain& chain,
                          const std::vector<std::vector<uint8_t>>& txs_to_announce,
                          const BtcPeerOptions& options = {});

}  // namespace dinero::swap
