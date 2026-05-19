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

    // NAT traversal Phase 1A: post-verack node identity exchange.
    // Sent immediately after VERSION and before VERACK when both peers
    // advertise NODE_DINERO_V2. Payload: pubkey_33 + sig_len_1 + sig.
    // Sig is DER-encoded ECDSA over remote peer's version nonce, signed
    // by node_identity.dat keypair. Proves the speaker controls node_id
    // = HASH160(pubkey). Older peers ignore unknown commands.
    constexpr const char* DINEROID = "dineroid";

    // NAT traversal Phase 1A.2 / BIP155: typed address gossip.
    // SENDADDRV2 (empty payload) signals support for ADDRV2 and is sent
    // in the same post-version, pre-verack window as DINEROID, gated on
    // NODE_DINERO_V2. When both peers send it, addr-gossip switches to
    // ADDRV2 — entries carry an explicit network-id byte (IPV4, IPV6,
    // TORV3 spec-compliant, I2P parsed-and-skipped for now) plus a
    // varlen address blob and BE port. Legacy ADDR is still parsed
    // forever for backward compat with rc7- peers.
    constexpr const char* SENDADDRV2 = "sendaddrv2";
    constexpr const char* ADDRV2 = "addrv2";

    // NAT traversal Phase C3 / circuit relay protocol — slice 1 lays
    // down the wire format and dispatch surface; later slices add the
    // registry, signature enforcement, and circuit splicing. End-to-end
    // encryption (so the relay can't read tunneled traffic) lands with
    // Phase B2 QUIC. None of these messages do anything functional yet
    // — handlers just log "received" + reject the call.
    //
    // RELAY_REGISTER  : NAT'd peer asks a relay node to advertise it.
    //                   Payload: node_id_20 | ttl_4LE | nonce_8 |
    //                            sig_len_1 | sig_DER
    //                   Signature is over SHA256(their_nonce_from_version
    //                   || node_id || ttl) so it can't be replayed on a
    //                   different connection.
    // RELAY_CONNECT   : External peer asks relay to bridge to a NAT'd
    //                   peer. Payload: target_node_id_20 | request_id_8.
    // RELAY_CONNECT_ACK: Relay's response to RELAY_CONNECT. Payload:
    //                   request_id_8 | circuit_id_8 (0=rejected) |
    //                   status_1 | msg_len_1 | msg_chars.
    //                   status: 0=ok, 1=no_such_peer, 2=relay_full,
    //                           3=rate_limited, 4=internal_error.
    // RELAY_DATA      : Opaque tunneled bytes over an established
    //                   circuit. Payload: circuit_id_8 | direction_1
    //                   (0=client→target, 1=target→client) |
    //                   payload_len_compact | payload_bytes.
    // RELAY_PING      : Keepalive (NAT mapping refresh on the relay's
    //                   path to the registered peer). Payload:
    //                   circuit_id_8 | nonce_8.
    // RELAY_HINTS     : Post-verack capability message — speaker
    //                   advertises which relays carry it (NODE_BEHIND_RELAY
    //                   bit + this message together form the BIP155-clean
    //                   alternative to a custom addrv2 type). Payload:
    //                   count_compact | { target_node_id_20 | net_type_1
    //                   | addr_len_1 | addr_bytes | port_2BE } × count.
    constexpr const char* RELAY_REGISTER     = "relayreg";
    constexpr const char* RELAY_CONNECT      = "relaycon";
    constexpr const char* RELAY_CONNECT_ACK  = "relayack";
    constexpr const char* RELAY_DATA         = "relaydat";
    constexpr const char* RELAY_PING         = "relaypng";
    constexpr const char* RELAY_HINTS        = "relayhnt";
} // namespace MessageCommands

// Service flags for node capabilities (Bitcoin P2P protocol standard)
namespace ServiceFlags {
    constexpr uint64_t NODE_NETWORK         = 1ULL << 0;   // Full node (can serve all blocks)
    constexpr uint64_t NODE_WITNESS         = 1ULL << 3;   // SegWit support
    constexpr uint64_t NODE_NETWORK_LIMITED = 1ULL << 10;  // Pruned node (limited block serving)
    constexpr uint64_t NODE_UTREEXO        = 1ULL << 24;  // Utreexo-aware node
    constexpr uint64_t NODE_UTREEXO_BRIDGE = 1ULL << 25;  // Can serve Utreexo proofs

    // ─── NAT traversal (Dinero v8 Phase 1) ───────────────────────────────
    // Per the NAT traversal plan: enabling node-identity exchange + circuit
    // relay so wallets behind hostile NAT / CGNAT can be reached inbound.
    // The three bits below MUST stay non-overlapping with Bitcoin Core's
    // assignments (1<<6 NODE_COMPACT_FILTERS, 1<<11 NODE_P2P_V2). 1<<26-28
    // are unused upstream and reserved for Dinero v8 use.
    constexpr uint64_t NODE_RELAY          = 1ULL << 26;  // Willing to relay circuits for NAT'd peers
    constexpr uint64_t NODE_DINERO_V2      = 1ULL << 27;  // Speaks post-verack `dineroid` + addrv2
    constexpr uint64_t NODE_BEHIND_RELAY   = 1ULL << 28;  // Self is NAT'd; reach me via relay_hints
} // namespace ServiceFlags

} // namespace dinero
