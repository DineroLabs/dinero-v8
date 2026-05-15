// Minimal Dinero P2P wire-format helpers for the seeder.
//
// The seeder doesn't need the full P2PManager — just enough to:
//   1) frame outbound messages (magic + command + length + checksum + payload)
//   2) parse inbound frames
//   3) build a `version` message
//   4) build a `getaddr` message
//   5) parse an `addr` payload into IP:port pairs
//
// The P2P wire magic comes from network_constants_generated.h, which is
// regenerated from src/consensus/chainparams_impl.cpp by
// tools/sync_network_constants_headers.py. This keeps the seeder
// binary independent of the daemon's heavyweight build graph while
// still guaranteeing the seeder uses the same magic as the daemon.
// The drift test in tests/integration/test_network_magic_sync.sh
// fails the build if regen would change the file. Protocol version
// and default port constants are still hand-mirrored — convert them
// the same way (chainparams_impl.cpp source of truth + generated
// header) when they next need to move.

#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "dinero/seeder/network_constants_generated.h"

namespace dinero {
namespace seeder {

// Default magic used by frame builders/parsers. Re-exported from the
// generated header so existing call sites keep working. Multi-network
// seeders should call MagicForNetwork() at startup and pass the
// returned value into build_frame / parse_frame explicitly.
inline constexpr uint32_t kMagicBytes = kMagicMainnet;

// Mirrors src/daemon/p2p_manager.cpp:PROTOCOL_VERSION (Utreexo support).
inline constexpr uint32_t kProtocolVersion = 70016u;

// Mirrors chainparams_impl.cpp default mainnet port.
inline constexpr uint16_t kDefaultMainnetPort = 20999;

// Service flags. Bit 0 = NODE_NETWORK (full block service); we set this
// so peers don't reject us for the handshake.
inline constexpr uint64_t kServiceNodeNetwork = 1ull << 0;

// A parsed inbound frame.
struct Frame {
    std::string command;            // null-stripped, e.g. "addr"
    std::vector<uint8_t> payload;   // raw payload bytes
};

// Build a complete wire frame (header + payload) ready to write() to a
// socket. `command` is automatically null-padded to 12 bytes.
std::vector<uint8_t> build_frame(const std::string& command,
                                  const std::vector<uint8_t>& payload);

// Try to parse a single frame from the start of `buf`. On success
// returns true, fills `out`, and `consumed` is set to the number of
// bytes used (header + payload). On short buffer returns false and
// `consumed` is unchanged. On bad magic / oversize length / bad checksum
// returns false with `consumed` set to 1 (resync by 1 byte; caller can
// keep trying).
bool parse_frame(const uint8_t* buf, size_t len, Frame& out, size_t& consumed);

// Build a `version` payload. Caller picks the nonce (any unique 64-bit
// value; the seeder uses a random per-connection nonce so peers can
// detect self-connections).
std::vector<uint8_t> build_version_payload(uint64_t nonce,
                                            const std::string& user_agent,
                                            uint32_t best_height);

// Build a `getaddr` payload (empty).
std::vector<uint8_t> build_getaddr_payload();

// Parse an `addr` payload into a list of (IP-string, port) pairs.
// IPv4 only (matches Dinero's current wire format — see
// src/daemon/p2p_manager.cpp:handle_addr).
std::vector<std::pair<std::string, uint16_t>>
parse_addr_payload(const std::vector<uint8_t>& payload);

}  // namespace seeder
}  // namespace dinero
