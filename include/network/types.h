// Copyright (c) 2025 The Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

#include <string>
#include <cstdint>

namespace dinero {

/**
 * Network type definitions
 * Used across networking, P2P, and connection management components
 */

// Peer identifier type
// Format: "ip:port" (e.g., "192.168.1.100:20999")
using peer_id_t = std::string;

// Message command types (12-byte fixed-length strings)
namespace MessageCommands {
    constexpr const char* VERSION = "version";
    constexpr const char* VERACK = "verack";
    constexpr const char* PING = "ping";
    constexpr const char* PONG = "pong";
    constexpr const char* ADDR = "addr";
    constexpr const char* INV = "inv";
    constexpr const char* GETDATA = "getdata";
    constexpr const char* BLOCK = "block";
    constexpr const char* TX = "tx";
    constexpr const char* GETBLOCKS = "getblocks";
    constexpr const char* GETHEADERS = "getheaders";
    constexpr const char* HEADERS = "headers";
    constexpr const char* GETADDR = "getaddr";
    constexpr const char* MEMPOOL = "mempool";
    constexpr const char* NOTFOUND = "notfound";
    constexpr const char* REJECT = "reject";
    constexpr const char* SENDHEADERS = "sendheaders";

    // Phase 7: Utreexo proof serving protocol
    constexpr const char* GETUTREEXOPROOF = "getutxoproof";
    constexpr const char* GETUTREEXOPROOFS = "getutxoproofs";  // Alias for plural naming
    constexpr const char* UTREEXOPROOF = "utxoproof";
    constexpr const char* UTREEXOPROOFS = "utxoproofs";        // Alias for plural naming
    constexpr const char* GETUTREEXOHDRS = "getutxohdrs";
    constexpr const char* UTREEXOHDRS = "utxohdrs";
    constexpr const char* UTREEXOHDRS_ALT = "utreexohdrs";     // Alias variant

    // Phase P.3: Utreexo block relay (block + proof combined)
    constexpr const char* UTXOBLK = "utxoblk";

    // Phase #4: Utreexo tx relay (tx + per-input proofs)
    constexpr const char* UTXOTX = "utxotx";

    // Backpressure: proof request rejection notification
    constexpr const char* UTREEXOPROOF_NACK = "utxoproofnack";

    // Phase 9.3: Proof gossip protocol (best-effort availability layer)
    constexpr const char* INVPROOF = "invproof";
    constexpr const char* GETPROOF = "getproof";
    constexpr const char* PROOFDATA = "proofdata";
} // namespace MessageCommands

// Service flags for node capabilities (Bitcoin P2P protocol standard)
namespace ServiceFlags {
    constexpr uint64_t NODE_NETWORK         = 1ULL << 0;   // Full node (can serve all blocks)
    constexpr uint64_t NODE_WITNESS         = 1ULL << 3;   // SegWit support
    constexpr uint64_t NODE_NETWORK_LIMITED = 1ULL << 10;  // Pruned node (limited block serving)
    constexpr uint64_t NODE_UTREEXO        = 1ULL << 24;  // Utreexo-aware node
    constexpr uint64_t NODE_UTREEXO_BRIDGE = 1ULL << 25;  // Can serve Utreexo proofs
} // namespace ServiceFlags

} // namespace dinero
