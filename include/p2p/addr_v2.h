// Copyright (c) 2026 The Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// BIP155 (addrv2) helpers — typed peer address encoding for the P2P layer.
//
// Wire format mirrors Bitcoin's BIP155 so future tooling that already
// understands BIP155 can interoperate. Reach goals:
//   - Plumb network-type aware address gossip without inventing a custom
//     format (the existing Dinero `addr` is non-standard and only carries
//     IPv4/IPv6 strings).
//   - Become the carrier of TORv3 onion addresses in the gossip layer
//     (currently onions only enter addrman via -addnode / static seeds).
//   - Provide the slot for future relay-circuit reachability hints
//     (carried via a separate `relay_hints` capability message — addrv2
//     itself stays BIP155-clean).
//
// Per-network sizes (in bytes), enforced by encoder + decoder:
//   IPV4   = 4   ([a, b, c, d] in network order)
//   IPV6   = 16  (raw)
//   TORV3  = 32  (ed25519 pubkey only — checksum + version regenerated on
//                 decode-to-onion-string at the call site)
//   I2P    = 32  (Destination's first 32 bytes — i2p-style)
//
// CompactSize varint encoding matches Bitcoin Core's ReadCompactSize.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace dinero::p2p {

// BIP155 network ids. We omit the deprecated TORV2 (3) and CJDNS (6)
// since neither makes sense for Dinero today. Unknown values seen on
// the wire MUST be skipped (length-prefixed addr_bytes lets us advance
// the cursor without parsing the body).
enum class NetworkType : uint8_t {
    Unknown = 0,
    IPV4    = 1,
    IPV6    = 2,
    TORV3   = 4,
    I2P     = 5,
};

// One entry in an addrv2 payload.
struct AddrV2Entry {
    uint32_t time{0};            // unix seconds, last-seen
    uint64_t services{0};        // service flag bits (same shape as in version msg)
    NetworkType net{NetworkType::Unknown};
    std::vector<uint8_t> addr;   // network-specific bytes, sized per net
    uint16_t port{0};            // host order in memory; serialized as BE
};

// Returns true and populates `out` if `addr` matches the canonical
// length for `net`. Defensive — used at both encode and decode time
// so we never put a malformed entry on the wire.
bool NetworkTypeExpectedLength(NetworkType net, size_t* out);

// Encode a vector of entries to a BIP155-aligned addrv2 payload (the
// raw payload bytes, NOT a full P2P message frame). Entries whose
// `net == Unknown` or whose `addr` size doesn't match are dropped
// silently (the caller pre-filters anyway, but we stay defensive).
std::vector<uint8_t> EncodeAddrV2(const std::vector<AddrV2Entry>& entries);

// Decode a BIP155-aligned addrv2 payload. Returns true on a fully
// parseable payload; false (and writes *err) on truncation / bad
// CompactSize / length mismatch. Unknown network ids are NOT a parse
// error — they're skipped over (so future BIP155 additions don't
// silently brick the network when partial peers see them).
bool DecodeAddrV2(const std::vector<uint8_t>& payload,
                  std::vector<AddrV2Entry>* out,
                  std::string* err);

}  // namespace dinero::p2p
