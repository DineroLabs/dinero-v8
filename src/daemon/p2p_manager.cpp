#include "p2p_manager.h"
#include "daemon/p2p_message.h"
#include "secure_random.h"
#include "crypto/sha256.h"
#include "common/sha256d.h"  // For Bitcoin-compatible double-SHA256 checksum
#include "consensus/chainparams.h"  // Canonical source of the P2P network magic
#include "network/local_interfaces.h"  // Self-loop filter at dial time
#include "network/types.h"          // Canonical P2P service flag assignments
#include "daemon/node_identity.h"      // NAT traversal Phase 1A: dineroid signing
#include "crypto/dinero_crypto_minimal.h"  // HASH160 3-arg form for node_id derivation
#include <iomanip>                     // std::setw / std::setfill for node_id hex log
#include <iostream>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cstring>
#include <cerrno>
#include <cassert>  // Ring 3 Phase 4c: TS1 invariant assertions
#include <cctype>
#include <ctime>
#include <deque>
#include <optional>  // Ring 3 Phase 4d: TS2 lock-free pattern
#include <random>    // Phase B (v8 peer discovery): mt19937 for addr-relay peer selection
#include <filesystem> // Phase C (v8 peer discovery): atomic peers.dat rename
#include <system_error>
#include <unordered_set>
#include <array>

// Platform-specific includes
#ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #pragma comment(lib, "ws2_32.lib")
    // POSIX compatibility
    #ifndef _SSIZE_T_DEFINED
      #define _SSIZE_T_DEFINED
      typedef ptrdiff_t ssize_t;
    #endif
    #ifndef MSG_NOSIGNAL
      #define MSG_NOSIGNAL 0
    #endif
    #ifndef MSG_DONTWAIT
      #define MSG_DONTWAIT 0
    #endif
    // POSIX shutdown(2) -> Winsock equivalents. The dead-orphan-sweep added
    // shutdown(fd, SHUT_RDWR) on the graceful-stop path; on POSIX SHUT_RDWR
    // is in <sys/socket.h>, on Winsock the equivalent is SD_BOTH.
    #ifndef SHUT_RD
      #define SHUT_RD   SD_RECEIVE
    #endif
    #ifndef SHUT_WR
      #define SHUT_WR   SD_SEND
    #endif
    #ifndef SHUT_RDWR
      #define SHUT_RDWR SD_BOTH
    #endif
#else
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <netinet/tcp.h>
    #include <arpa/inet.h>
    #include <unistd.h>
    #include <fcntl.h>
    #include <netdb.h>
#endif

// P2P Protocol constants
static const uint32_t PROTOCOL_VERSION = 70016;  // Utreexo support

#ifndef DINERO_CLI_GIT_SHA
#define DINERO_CLI_GIT_SHA "unknown"
#endif
static const std::string USER_AGENT = std::string("/dinerod:") + DINERO_CLI_GIT_SHA + "/";

// Network magic bytes — now read from chainparams at call time. The
// canonical per-chain values live in src/consensus/chainparams_impl.cpp;
// SelectParams(chain) at daemon startup makes the right one active and
// dinero::Params().magic returns it. The drift test in
// tests/integration/test_network_magic_sync.sh fails the build if any
// non-canonical literal copies sneak back in.
//
// The NetworkMagic namespace + static MAGIC_BYTES constants that used
// to live here have been removed. The serialize() / parse() paths
// below now call MagicBytes() which forwards to chainparams.
namespace {
inline uint32_t MagicBytes() { return dinero::Params().magic; }
}  // namespace

// Keep service-bit assignments centralized. The NAT traversal flags are shared
// with addrv2 / relay planning, so p2p_manager must not carry a drift-prone copy.
namespace ServiceFlags = dinero::ServiceFlags;

namespace {

constexpr uint32_t kInvMsgTx = 1;
constexpr uint32_t kInvMsgBlock = 2;
constexpr uint32_t kInvMsgUtreexoTx = 0x50000001u;
constexpr uint32_t kInvMsgUtreexoBlock = 0x50000002u;

uint32_t ResolveInventoryType(const std::string& type) {
    if (type == "tx") {
        return kInvMsgTx;
    }
    if (type == "utreexo_tx") {
        return kInvMsgUtreexoTx;
    }
    if (type == "utreexo_block") {
        return kInvMsgUtreexoBlock;
    }
    return kInvMsgBlock;
}

uint32_t ReadLE32(const std::vector<uint8_t>& data, size_t offset) {
    if (offset + 4 > data.size()) {
        return 0;
    }
    return static_cast<uint32_t>(data[offset]) |
           (static_cast<uint32_t>(data[offset + 1]) << 8) |
           (static_cast<uint32_t>(data[offset + 2]) << 16) |
           (static_cast<uint32_t>(data[offset + 3]) << 24);
}

uint64_t ReadLE64(const std::vector<uint8_t>& data, size_t offset) {
    if (offset + 8 > data.size()) {
        return 0;
    }
    uint64_t value = 0;
    for (size_t i = 0; i < 8; ++i) {
        value |= static_cast<uint64_t>(data[offset + i]) << (8 * i);
    }
    return value;
}

std::vector<uint8_t> CreateSendCmpctPayload(bool announce, uint64_t version) {
    std::vector<uint8_t> payload;
    payload.reserve(9);
    payload.push_back(announce ? 1 : 0);
    for (int i = 0; i < 8; ++i) {
        payload.push_back(static_cast<uint8_t>((version >> (8 * i)) & 0xFF));
    }
    return payload;
}

bool ReadVarInt(const std::vector<uint8_t>& data, size_t& offset, uint64_t* out_value) {
    if (!out_value || offset >= data.size()) {
        return false;
    }

    const uint8_t marker = data[offset++];
    if (marker < 0xFD) {
        *out_value = marker;
        return true;
    }

    if (marker == 0xFD) {
        if (offset + 2 > data.size()) {
            return false;
        }
        *out_value = static_cast<uint64_t>(data[offset]) |
                     (static_cast<uint64_t>(data[offset + 1]) << 8);
        offset += 2;
        return true;
    }

    if (marker == 0xFE) {
        if (offset + 4 > data.size()) {
            return false;
        }
        *out_value = static_cast<uint64_t>(ReadLE32(data, offset));
        offset += 4;
        return true;
    }

    if (offset + 8 > data.size()) {
        return false;
    }
    *out_value = ReadLE64(data, offset);
    offset += 8;
    return true;
}

std::string AddressKey(const std::string& address, uint16_t port) {
    return address + ":" + std::to_string(port);
}

bool IsLocalOrWildcardAddress(const std::string& address) {
    return address.empty() ||
           address == "127.0.0.1" ||
           address == "0.0.0.0" ||
           address == "::1" ||
           address == "::";
}

std::string ToLowerAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

bool EndsWith(const std::string& value, const std::string& suffix) {
    return value.size() >= suffix.size() &&
           value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool IsOnionAddress(const std::string& address) {
    const std::string lower = ToLowerAscii(address);
    return EndsWith(lower, ".onion");
}

dinero::p2p::NetworkAddress NetworkAddressForPeer(const std::string& address,
                                                  uint16_t port,
                                                  uint64_t services);

bool IsAdvertisableAddress(const std::string& address, uint16_t port) {
    if (IsLocalOrWildcardAddress(address) || port == 0) {
        return false;
    }
    if (IsOnionAddress(address)) {
        return true;
    }
    auto network_addr = NetworkAddressForPeer(address, port, 0);
    return network_addr.isValid() && network_addr.isRoutable();
}

std::string TrimAscii(std::string value) {
    auto is_space = [](unsigned char c) { return std::isspace(c) != 0; };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(),
                                            [&](unsigned char c) { return !is_space(c); }));
    value.erase(std::find_if(value.rbegin(), value.rend(),
                             [&](unsigned char c) { return !is_space(c); }).base(),
                value.end());
    return value;
}

bool ParseUint16(const std::string& value, uint16_t* out_port) {
    if (!out_port || value.empty()) {
        return false;
    }
    try {
        size_t consumed = 0;
        const unsigned long parsed = std::stoul(value, &consumed);
        if (consumed != value.size() || parsed == 0 || parsed > 65535) {
            return false;
        }
        *out_port = static_cast<uint16_t>(parsed);
        return true;
    } catch (...) {
        return false;
    }
}

bool SplitHostPort(const std::string& target,
                   std::string* out_host,
                   uint16_t* out_port,
                   bool* out_has_port) {
    if (!out_host || !out_port || !out_has_port) {
        return false;
    }

    const std::string trimmed = TrimAscii(target);
    if (trimmed.empty()) {
        return false;
    }

    *out_host = trimmed;
    *out_port = 0;
    *out_has_port = false;

    if (trimmed.front() == '[') {
        const size_t close = trimmed.find(']');
        if (close == std::string::npos || close == 1) {
            return false;
        }
        *out_host = trimmed.substr(1, close - 1);
        if (close + 1 < trimmed.size()) {
            if (trimmed[close + 1] != ':') {
                return false;
            }
            *out_has_port = ParseUint16(trimmed.substr(close + 2), out_port);
            return *out_has_port;
        }
        return true;
    }

    const size_t first_colon = trimmed.find(':');
    if (first_colon != std::string::npos && first_colon == trimmed.rfind(':')) {
        uint16_t parsed_port = 0;
        if (ParseUint16(trimmed.substr(first_colon + 1), &parsed_port)) {
            *out_host = trimmed.substr(0, first_colon);
            *out_port = parsed_port;
            *out_has_port = true;
        }
    }

    return !out_host->empty();
}

bool Ipv4ToHostOrder(const std::string& value, uint32_t* out) {
    if (!out) {
        return false;
    }
    struct in_addr addr {};
    if (inet_pton(AF_INET, value.c_str(), &addr) != 1) {
        return false;
    }
    *out = ntohl(addr.s_addr);
    return true;
}

bool CidrMatches(const std::string& cidr, const std::string& address) {
    const size_t slash = cidr.find('/');
    if (slash == std::string::npos || slash == 0 || slash + 1 >= cidr.size()) {
        return false;
    }

    int prefix = -1;
    try {
        size_t consumed = 0;
        prefix = std::stoi(cidr.substr(slash + 1), &consumed);
        if (consumed != cidr.size() - slash - 1 || prefix < 0 || prefix > 32) {
            return false;
        }
    } catch (...) {
        return false;
    }

    uint32_t base = 0;
    uint32_t candidate = 0;
    if (!Ipv4ToHostOrder(cidr.substr(0, slash), &base) ||
        !Ipv4ToHostOrder(address, &candidate)) {
        return false;
    }

    const uint32_t mask = (prefix == 0) ? 0u : (0xffffffffu << (32 - prefix));
    return (base & mask) == (candidate & mask);
}

bool BanTargetMatches(const std::string& target,
                      const std::string& address,
                      uint16_t port) {
    const std::string normalized = TrimAscii(target);
    if (normalized.empty()) {
        return false;
    }

    if (normalized.find('/') != std::string::npos) {
        return CidrMatches(normalized, address);
    }

    if (port != 0 && normalized == AddressKey(address, port)) {
        return true;
    }

    std::string host;
    uint16_t target_port = 0;
    bool has_port = false;
    if (!SplitHostPort(normalized, &host, &target_port, &has_port)) {
        return false;
    }

    if (has_port) {
        return port != 0 && target_port == port && host == address;
    }

    return host == address;
}

PeerInfo PeerInfoForAddress(const std::string& address, uint16_t port) {
    PeerInfo info;
    info.address = address;
    info.port = port;
    info.is_outbound = false;
    info.is_connected = false;
    info.socket_fd = -1;
    return info;
}

dinero::p2p::NetworkAddress NetworkAddressForPeer(const std::string& address,
                                                  uint16_t port,
                                                  uint64_t services = 0) {
    dinero::p2p::NetworkAddress peer_addr;
    peer_addr.ip = address;
    peer_addr.port = port;
    peer_addr.services = services;
    peer_addr.timestamp = std::chrono::system_clock::now();
    return peer_addr;
}

bool SendAll(int socket_fd, const uint8_t* data, size_t len) {
    size_t sent_total = 0;
    while (sent_total < len) {
#ifdef _WIN32
        const int sent = send(socket_fd,
                              reinterpret_cast<const char*>(data + sent_total),
                              static_cast<int>(len - sent_total),
                              0);
#else
        const ssize_t sent = send(socket_fd, data + sent_total, len - sent_total, MSG_NOSIGNAL);
#endif
        if (sent <= 0) {
            return false;
        }
        sent_total += static_cast<size_t>(sent);
    }
    return true;
}

bool RecvAll(int socket_fd, uint8_t* data, size_t len) {
    size_t recv_total = 0;
    while (recv_total < len) {
#ifdef _WIN32
        const int got = recv(socket_fd,
                             reinterpret_cast<char*>(data + recv_total),
                             static_cast<int>(len - recv_total),
                             0);
#else
        const ssize_t got = recv(socket_fd, data + recv_total, len - recv_total, 0);
#endif
        if (got <= 0) {
            return false;
        }
        recv_total += static_cast<size_t>(got);
    }
    return true;
}

}  // namespace

// Message creation functions
P2PMessage P2PMessage::create_version(uint32_t protocol_version, uint32_t best_height,
                                      uint64_t services,
                                      const std::string& user_agent,
                                      uint64_t explicit_nonce) {
    P2PMessage msg;
    msg.command = "version";

    // Bitcoin wire format version message
    std::vector<uint8_t> payload;

    // Protocol version (4 bytes, little-endian)
    for (int i = 0; i < 4; i++) {
        payload.push_back((protocol_version >> (i * 8)) & 0xFF);
    }

    // Services (8 bytes) - caller provides flags (prune-aware)
    // Default: full node with Utreexo support (bridge bit is opt-in via provider/config).
    // NODE_DINERO_V2 announces post-verack `dineroid` capability — older peers
    // ignore unknown service bits, so this is fully backward-compatible.
    if (services == 0) {
        services = ServiceFlags::NODE_NETWORK
                 | ServiceFlags::NODE_UTREEXO
                 | ServiceFlags::NODE_DINERO_V2;
    }
    for (int i = 0; i < 8; i++) {
        payload.push_back((services >> (i * 8)) & 0xFF);
    }

    // Timestamp (8 bytes)
    uint64_t timestamp = static_cast<uint64_t>(time(nullptr));
    for (int i = 0; i < 8; i++) {
        payload.push_back((timestamp >> (i * 8)) & 0xFF);
    }

    // addrRecv (26 bytes): services(8) + IPv6(16) + port(2)
    for (int i = 0; i < 8; i++) payload.push_back((services >> (i * 8)) & 0xFF);  // services
    for (int i = 0; i < 10; i++) payload.push_back(0);  // IPv6 prefix zeros
    payload.push_back(0xff); payload.push_back(0xff);  // IPv4 marker
    payload.push_back(0); payload.push_back(0); payload.push_back(0); payload.push_back(0);  // IPv4 zeros
    payload.push_back((20999 >> 8) & 0xFF);  // Port (big-endian)
    payload.push_back(20999 & 0xFF);

    // addrFrom (26 bytes): same structure
    for (int i = 0; i < 8; i++) payload.push_back((services >> (i * 8)) & 0xFF);
    for (int i = 0; i < 10; i++) payload.push_back(0);
    payload.push_back(0xff); payload.push_back(0xff);
    payload.push_back(0); payload.push_back(0); payload.push_back(0); payload.push_back(0);
    payload.push_back((20999 >> 8) & 0xFF);
    payload.push_back(20999 & 0xFF);

    // Nonce (8 bytes). Caller may supply an explicit nonce (e.g. so the
    // `dineroid` post-verack identity exchange can sign exactly the value
    // that ends up on the wire). 0 means "auto-generate", preserving prior
    // call sites that don't track nonces.
    uint64_t nonce = explicit_nonce != 0 ? explicit_nonce : SecureRandom::GetUInt64();
    for (int i = 0; i < 8; i++) {
        payload.push_back((nonce >> (i * 8)) & 0xFF);
    }

    // User agent (var_str)
    std::string ua = user_agent.empty() ? USER_AGENT : user_agent;
    payload.push_back(static_cast<uint8_t>(ua.length()));
    for (char c : ua) {
        payload.push_back(static_cast<uint8_t>(c));
    }

    // Start height (4 bytes)
    for (int i = 0; i < 4; i++) {
        payload.push_back((best_height >> (i * 8)) & 0xFF);
    }

    // Relay (1 byte)
    payload.push_back(1);

    msg.payload = payload;
    return msg;
}

P2PMessage P2PMessage::create_verack() {
    P2PMessage msg;
    msg.command = "verack";
    msg.payload.clear();
    return msg;
}

// NAT traversal Phase 1A: post-verack node-identity exchange.
// Wire format of payload:
//   [0..33)  : pubkey (33 bytes, secp256k1 compressed)
//   [33]     : sig_len (1 byte, 1..72 for DER-encoded ECDSA)
//   [34..)   : sig (sig_len bytes)
// `nonce_to_sign` is the REMOTE peer's version nonce (8 bytes, little-endian).
// Signing that value binds this dineroid to the specific handshake; replaying
// it against any other connection fails because the other side's nonce differs.
// ─── NAT Phase C3 slice 1: circuit relay wire format ────────────────
//
// Each builder produces ONLY the payload bytes; the outer P2P frame
// (magic + command + length + checksum) is added by serialize() in
// p2p_message.cpp. All multi-byte integers are little-endian for
// consistency with the rest of Dinero's P2P protocol EXCEPT the port
// fields in RELAY_HINTS, which match BIP155 (BE) so addrv2-derived
// codepaths can share format helpers later.

namespace {

// Local CompactSize writer mirroring dinero::p2p::WriteCompactSize but
// kept private to this TU since the call sites are local. Splitting
// later if a third helper needs it.
void WriteCompactSizeLocal(std::vector<uint8_t>* out, uint64_t v) {
    if (v < 253) {
        out->push_back(static_cast<uint8_t>(v));
    } else if (v <= 0xFFFFu) {
        out->push_back(253);
        out->push_back(static_cast<uint8_t>(v & 0xFF));
        out->push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    } else if (v <= 0xFFFFFFFFu) {
        out->push_back(254);
        for (int i = 0; i < 4; i++) out->push_back(static_cast<uint8_t>((v >> (i * 8)) & 0xFF));
    } else {
        out->push_back(255);
        for (int i = 0; i < 8; i++) out->push_back(static_cast<uint8_t>((v >> (i * 8)) & 0xFF));
    }
}

void WriteLE32Local(std::vector<uint8_t>* out, uint32_t v) {
    for (int i = 0; i < 4; i++) out->push_back(static_cast<uint8_t>((v >> (i * 8)) & 0xFF));
}
void WriteLE64Local(std::vector<uint8_t>* out, uint64_t v) {
    for (int i = 0; i < 8; i++) out->push_back(static_cast<uint8_t>((v >> (i * 8)) & 0xFF));
}
void WriteBE16Local(std::vector<uint8_t>* out, uint16_t v) {
    out->push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    out->push_back(static_cast<uint8_t>(v & 0xFF));
}

}  // namespace

P2PMessage P2PMessage::create_relay_register(
    const dinero::daemon::NodeIdentity& identity,
    uint64_t nonce_to_sign,
    uint32_t ttl_seconds) {
    P2PMessage msg;
    msg.command = "relayreg";

    const auto node_id = identity.get_node_id_bytes();
    // Build the message that the registrant signs:
    // SHA256(nonce_8LE || node_id_20 || ttl_4LE). The receiving relay
    // re-derives this same byte sequence and verifies against the
    // registrant's proven pubkey.
    std::vector<uint8_t> to_sign;
    to_sign.reserve(8 + 20 + 4);
    WriteLE64Local(&to_sign, nonce_to_sign);
    to_sign.insert(to_sign.end(), node_id.begin(), node_id.end());
    WriteLE32Local(&to_sign, ttl_seconds);

    auto sig = identity.sign_bytes(to_sign.data(), to_sign.size());
    if (sig.empty() || sig.size() > 72) return msg;  // empty payload signals error

    msg.payload.reserve(20 + 4 + 8 + 1 + sig.size());
    msg.payload.insert(msg.payload.end(), node_id.begin(), node_id.end());
    WriteLE32Local(&msg.payload, ttl_seconds);
    WriteLE64Local(&msg.payload, nonce_to_sign);
    msg.payload.push_back(static_cast<uint8_t>(sig.size()));
    msg.payload.insert(msg.payload.end(), sig.begin(), sig.end());
    return msg;
}

P2PMessage P2PMessage::create_relay_connect(
    const std::array<uint8_t, 20>& target_node_id,
    uint64_t request_id) {
    P2PMessage msg;
    msg.command = "relaycon";
    msg.payload.reserve(20 + 8);
    msg.payload.insert(msg.payload.end(), target_node_id.begin(), target_node_id.end());
    WriteLE64Local(&msg.payload, request_id);
    return msg;
}

P2PMessage P2PMessage::create_relay_connect_ack(
    uint64_t request_id,
    uint64_t circuit_id,
    RelayConnectStatus status,
    const std::string& message) {
    P2PMessage msg;
    msg.command = "relayack";
    const auto msg_len = std::min<size_t>(message.size(), 255);
    msg.payload.reserve(8 + 8 + 1 + 1 + msg_len);
    WriteLE64Local(&msg.payload, request_id);
    WriteLE64Local(&msg.payload, circuit_id);
    msg.payload.push_back(static_cast<uint8_t>(status));
    msg.payload.push_back(static_cast<uint8_t>(msg_len));
    msg.payload.insert(msg.payload.end(),
                       message.begin(),
                       message.begin() + static_cast<std::ptrdiff_t>(msg_len));
    return msg;
}

P2PMessage P2PMessage::create_relay_data(
    uint64_t circuit_id,
    RelayDirection direction,
    const std::vector<uint8_t>& payload) {
    P2PMessage msg;
    msg.command = "relaydat";
    msg.payload.reserve(8 + 1 + 9 + payload.size());
    WriteLE64Local(&msg.payload, circuit_id);
    msg.payload.push_back(static_cast<uint8_t>(direction));
    WriteCompactSizeLocal(&msg.payload, payload.size());
    msg.payload.insert(msg.payload.end(), payload.begin(), payload.end());
    return msg;
}

P2PMessage P2PMessage::create_relay_ping(uint64_t circuit_id, uint64_t nonce) {
    P2PMessage msg;
    msg.command = "relaypng";
    msg.payload.reserve(16);
    WriteLE64Local(&msg.payload, circuit_id);
    WriteLE64Local(&msg.payload, nonce);
    return msg;
}

P2PMessage P2PMessage::create_relay_hints(const std::vector<RelayHint>& hints) {
    P2PMessage msg;
    msg.command = "relayhnt";

    // Pre-filter to entries whose addr length matches the per-network
    // expectation (same defensive check as EncodeAddrV2).
    std::vector<const RelayHint*> valid;
    valid.reserve(hints.size());
    for (const auto& h : hints) {
        size_t expected = 0;
        if (!dinero::p2p::NetworkTypeExpectedLength(h.relay_net, &expected)) continue;
        if (h.relay_addr.size() != expected) continue;
        valid.push_back(&h);
    }

    WriteCompactSizeLocal(&msg.payload, valid.size());
    for (const auto* h : valid) {
        msg.payload.insert(msg.payload.end(),
                           h->target_node_id.begin(),
                           h->target_node_id.end());
        msg.payload.push_back(static_cast<uint8_t>(h->relay_net));
        msg.payload.push_back(static_cast<uint8_t>(h->relay_addr.size()));
        msg.payload.insert(msg.payload.end(),
                           h->relay_addr.begin(), h->relay_addr.end());
        WriteBE16Local(&msg.payload, h->relay_port);
    }
    return msg;
}

// NAT traversal Phase C3 slice 2: validate + ingest a RELAY_REGISTER.
//
// Validation rules (each must hold or we log + drop):
//   1. Peer's dineroid identity is proven (we have their pubkey).
//   2. Payload is well-formed: 20 + 4 + 8 + 1 + sig_len bytes.
//   3. Claimed node_id matches the dineroid-proven node_id (you can
//      only register YOUR OWN identity — not someone else's).
//   4. Nonce in the payload equals peer->our_nonce — the nonce WE
//      sent in OUR version message to THIS peer. Binds the
//      registration to this specific connection so an attacker who
//      captures the relayreg on the wire can't replay it elsewhere.
//   5. TTL clamped to RelayRegistry::kMaxTtlSeconds (2h).
//   6. SHA256(nonce_LE || node_id || ttl_LE) verifies against the
//      proven pubkey using NodeIdentity::verify_bytes.
//   7. Registry has capacity (refuses new entries when full, but
//      always lets existing registrants refresh).
void P2PManager::handle_relay_register(const std::string& peer_address,
                                       const P2PMessage& message) {
    // (2) Wire-format length check.
    constexpr size_t kFixedPrefix = 20 + 4 + 8 + 1;
    if (message.payload.size() < kFixedPrefix) {
        std::cout << "[P2P] relayreg: short payload (" << message.payload.size()
                  << " bytes) from " << peer_address << std::endl;
        return;
    }
    const uint8_t sig_len = message.payload[20 + 4 + 8];
    if (sig_len == 0 || sig_len > 72 ||
        message.payload.size() != kFixedPrefix + sig_len) {
        std::cout << "[P2P] relayreg: bad sig_len " << static_cast<int>(sig_len)
                  << " from " << peer_address << std::endl;
        return;
    }

    std::array<uint8_t, 20> claimed_node_id{};
    std::copy_n(message.payload.begin(), 20, claimed_node_id.begin());

    const uint32_t ttl = ReadLE32(message.payload, 20);
    const uint64_t payload_nonce = ReadLE64(message.payload, 20 + 4);
    const uint8_t* sig_ptr = message.payload.data() + kFixedPrefix;

    // (1) Look up the peer; require proven identity.
    std::shared_ptr<PeerInfo> peer;
    {
        std::lock_guard<std::mutex> lock(peers_mutex_);
        auto it = connected_peers_.find(peer_address);
        if (it != connected_peers_.end()) peer = it->second;
    }
    if (!peer) {
        std::cout << "[P2P] relayreg: unknown peer " << peer_address << std::endl;
        return;
    }
    if (!peer->identity_proven) {
        std::cout << "[P2P] relayreg: rejected from " << peer_address
                  << " — dineroid identity not proven on this connection" << std::endl;
        return;
    }

    // (3) Claimed node_id MUST match the proven one.
    if (claimed_node_id != peer->their_node_id) {
        std::cout << "[P2P] relayreg: claimed node_id doesn't match proven identity from "
                  << peer_address << " — refused" << std::endl;
        return;
    }

    // (4) Nonce binding to THIS connection.
    if (payload_nonce != peer->our_nonce) {
        std::cout << "[P2P] relayreg: nonce mismatch from " << peer_address
                  << " (replay attempt or stale message)" << std::endl;
        return;
    }

    // (5) Clamp TTL.
    const uint32_t effective_ttl =
        std::min<uint32_t>(ttl, dinero::network::RelayRegistry::kMaxTtlSeconds);
    if (effective_ttl == 0) {
        std::cout << "[P2P] relayreg: zero TTL from " << peer_address
                  << " — refused" << std::endl;
        return;
    }

    // (6) Reconstruct the signed message and verify.
    std::vector<uint8_t> signed_msg;
    signed_msg.reserve(8 + 20 + 4);
    for (int i = 0; i < 8; i++) {
        signed_msg.push_back(static_cast<uint8_t>((payload_nonce >> (i * 8)) & 0xFF));
    }
    signed_msg.insert(signed_msg.end(), claimed_node_id.begin(), claimed_node_id.end());
    for (int i = 0; i < 4; i++) {
        signed_msg.push_back(static_cast<uint8_t>((ttl >> (i * 8)) & 0xFF));
    }
    if (!dinero::daemon::NodeIdentity::verify_bytes(
            signed_msg.data(), signed_msg.size(),
            sig_ptr, sig_len, peer->their_pubkey.data())) {
        std::cout << "[P2P] relayreg: signature verification FAILED from "
                  << peer_address << std::endl;
        return;
    }

    // (7) Register.
    dinero::network::RelayRegistration reg;
    reg.node_id = claimed_node_id;
    reg.pubkey = peer->their_pubkey;
    reg.peer_address = peer_address;
    reg.expires_at = std::chrono::steady_clock::now() +
                     std::chrono::seconds(effective_ttl);
    const bool inserted = relay_registry_.Register(reg);
    if (!inserted) {
        std::cout << "[P2P] relayreg: refused from " << peer_address
                  << " — registry at capacity (" << relay_registry_.size() << ")"
                  << std::endl;
        return;
    }

    std::ostringstream id_hex;
    id_hex << std::hex << std::setfill('0');
    for (auto b : claimed_node_id) id_hex << std::setw(2) << static_cast<unsigned int>(b);
    std::cout << "[P2P] relayreg: registered " << id_hex.str() << " from "
              << peer_address << " for " << effective_ttl
              << "s (registry size now " << relay_registry_.size() << ")"
              << std::endl;
}

// NAT traversal Phase 1A.2 / BIP155: sendaddrv2 has empty payload — its
// mere presence is the negotiation that both peers speak addrv2.
P2PMessage P2PMessage::create_sendaddrv2() {
    P2PMessage msg;
    msg.command = "sendaddrv2";
    msg.payload.clear();
    return msg;
}

// NAT traversal Phase 1A.2 / BIP155: typed addr-v2 message body. Delegates
// the wire format to dinero::p2p::EncodeAddrV2 so the encoder is shared
// with any unit tests that might need it independently of the P2P frame.
P2PMessage P2PMessage::create_addrv2(const std::vector<dinero::p2p::AddrV2Entry>& entries) {
    P2PMessage msg;
    msg.command = "addrv2";
    msg.payload = dinero::p2p::EncodeAddrV2(entries);
    return msg;
}

P2PMessage P2PMessage::create_dineroid(const dinero::daemon::NodeIdentity& identity,
                                       uint64_t nonce_to_sign) {
    P2PMessage msg;
    msg.command = "dineroid";

    const auto& pubkey = identity.get_pubkey_bytes();
    // The identity is uninitialized if get_pubkey_bytes returns all zeros —
    // the daemon would have failed earlier on missing keys, but stay defensive.
    bool pubkey_is_zero = true;
    for (auto b : pubkey) { if (b != 0) { pubkey_is_zero = false; break; } }
    if (pubkey_is_zero) {
        return msg;  // empty payload; caller must check and skip
    }

    uint8_t nonce_le[8];
    for (int i = 0; i < 8; i++) {
        nonce_le[i] = static_cast<uint8_t>((nonce_to_sign >> (i * 8)) & 0xFF);
    }
    auto sig = identity.sign_bytes(nonce_le, 8);
    if (sig.empty() || sig.size() > 72) {
        return msg;  // signing failure; caller must check and skip
    }

    msg.payload.reserve(33 + 1 + sig.size());
    msg.payload.insert(msg.payload.end(), pubkey.begin(), pubkey.end());
    msg.payload.push_back(static_cast<uint8_t>(sig.size()));
    msg.payload.insert(msg.payload.end(), sig.begin(), sig.end());
    return msg;
}

P2PMessage P2PMessage::create_ping(uint64_t nonce) {
    P2PMessage msg;
    msg.command = "ping";
    
    // 8-byte nonce
    for (int i = 0; i < 8; i++) {
        msg.payload.push_back((nonce >> (i * 8)) & 0xFF);
    }
    
    return msg;
}

P2PMessage P2PMessage::create_pong(uint64_t nonce) {
    P2PMessage msg;
    msg.command = "pong";
    
    // 8-byte nonce
    for (int i = 0; i < 8; i++) {
        msg.payload.push_back((nonce >> (i * 8)) & 0xFF);
    }
    
    return msg;
}

P2PMessage P2PMessage::create_getaddr() {
    P2PMessage msg;
    msg.command = "getaddr";
    msg.payload.clear();
    return msg;
}

P2PMessage P2PMessage::create_addr(const std::vector<PeerInfo>& peers) {
    P2PMessage msg;
    msg.command = "addr";
    
    // Count (1 byte for simplicity)
    msg.payload.push_back(static_cast<uint8_t>(std::min(peers.size(), size_t(255))));
    
    // Peer addresses
    for (size_t i = 0; i < std::min(peers.size(), size_t(255)); i++) {
        const auto& peer = peers[i];
        
        // IP address (simplified - just store as string length + string)
        msg.payload.push_back(static_cast<uint8_t>(peer.address.length()));
        for (char c : peer.address) {
            msg.payload.push_back(static_cast<uint8_t>(c));
        }
        
        // Port (2 bytes)
        msg.payload.push_back(peer.port & 0xFF);
        msg.payload.push_back((peer.port >> 8) & 0xFF);
    }
    
    return msg;
}

P2PMessage P2PMessage::create_inv(const std::vector<std::string>& hashes, const std::string& type) {
    P2PMessage msg;
    msg.command = "inv";

    // Binary format: count(1) + [type(4) + hash(32)]*count.
    // Supports: tx, block, utreexo_tx, utreexo_block.
    uint32_t inv_type = ResolveInventoryType(type);

    msg.payload.reserve(1 + hashes.size() * 36);
    msg.payload.push_back(static_cast<uint8_t>(hashes.size()));

    for (const auto& hex_hash : hashes) {
        // Append type (4 bytes, little-endian)
        msg.payload.push_back(inv_type & 0xFF);
        msg.payload.push_back((inv_type >> 8) & 0xFF);
        msg.payload.push_back((inv_type >> 16) & 0xFF);
        msg.payload.push_back((inv_type >> 24) & 0xFF);

        // Convert hex to binary (32 bytes)
        for (size_t i = 0; i < 64 && i < hex_hash.size(); i += 2) {
            uint8_t byte = 0;
            char c1 = hex_hash[i];
            char c2 = (i + 1 < hex_hash.size()) ? hex_hash[i + 1] : '0';
            byte = ((c1 >= 'a' ? c1 - 'a' + 10 : c1 >= 'A' ? c1 - 'A' + 10 : c1 - '0') << 4) |
                   (c2 >= 'a' ? c2 - 'a' + 10 : c2 >= 'A' ? c2 - 'A' + 10 : c2 - '0');
            msg.payload.push_back(byte);
        }
        // Pad if hash was shorter than 64 hex chars
        while (msg.payload.size() < 1 + (static_cast<size_t>(&hex_hash - &hashes[0]) + 1) * 36) {
            msg.payload.push_back(0);
        }
    }

    return msg;
}

P2PMessage P2PMessage::create_getdata(const std::vector<std::string>& hashes, const std::string& type) {
    P2PMessage msg;
    msg.command = "getdata";

    // Binary format: count(1) + [type(4) + hash(32)]*count.
    // Supports: tx, block, utreexo_tx, utreexo_block.
    uint32_t inv_type = ResolveInventoryType(type);

    msg.payload.reserve(1 + hashes.size() * 36);
    msg.payload.push_back(static_cast<uint8_t>(hashes.size()));

    for (const auto& hex_hash : hashes) {
        // Append type (4 bytes, little-endian)
        msg.payload.push_back(inv_type & 0xFF);
        msg.payload.push_back((inv_type >> 8) & 0xFF);
        msg.payload.push_back((inv_type >> 16) & 0xFF);
        msg.payload.push_back((inv_type >> 24) & 0xFF);

        // Convert hex to binary (32 bytes)
        for (size_t i = 0; i < 64 && i < hex_hash.size(); i += 2) {
            uint8_t byte = 0;
            char c1 = hex_hash[i];
            char c2 = (i + 1 < hex_hash.size()) ? hex_hash[i + 1] : '0';
            byte = ((c1 >= 'a' ? c1 - 'a' + 10 : c1 >= 'A' ? c1 - 'A' + 10 : c1 - '0') << 4) |
                   (c2 >= 'a' ? c2 - 'a' + 10 : c2 >= 'A' ? c2 - 'A' + 10 : c2 - '0');
            msg.payload.push_back(byte);
        }
        // Pad if hash was shorter than 64 hex chars
        while (msg.payload.size() < 1 + (static_cast<size_t>(&hex_hash - &hashes[0]) + 1) * 36) {
            msg.payload.push_back(0);
        }
    }

    return msg;
}

// Binary INV: takes raw hash bytes directly (no hex conversion)
P2PMessage P2PMessage::create_inv_binary(const uint8_t* hash, size_t hash_len, uint32_t inv_type) {
    P2PMessage msg;
    msg.command = "inv";

    // Binary format: count(1) + type(4) + hash(32)
    msg.payload.reserve(37);
    msg.payload.push_back(1);  // count = 1

    // Type (4 bytes, little-endian)
    msg.payload.push_back(inv_type & 0xFF);
    msg.payload.push_back((inv_type >> 8) & 0xFF);
    msg.payload.push_back((inv_type >> 16) & 0xFF);
    msg.payload.push_back((inv_type >> 24) & 0xFF);

    // Hash (32 bytes, raw)
    for (size_t i = 0; i < 32 && i < hash_len; ++i) {
        msg.payload.push_back(hash[i]);
    }
    // Pad if needed
    for (size_t i = hash_len; i < 32; ++i) {
        msg.payload.push_back(0);
    }

    return msg;
}

// Binary GETDATA: takes raw hash bytes directly
P2PMessage P2PMessage::create_getdata_binary(const uint8_t* hash, size_t hash_len, uint32_t inv_type) {
    P2PMessage msg;
    msg.command = "getdata";

    // Binary format: count(1) + type(4) + hash(32)
    msg.payload.reserve(37);
    msg.payload.push_back(1);  // count = 1

    // Type (4 bytes, little-endian)
    msg.payload.push_back(inv_type & 0xFF);
    msg.payload.push_back((inv_type >> 8) & 0xFF);
    msg.payload.push_back((inv_type >> 16) & 0xFF);
    msg.payload.push_back((inv_type >> 24) & 0xFF);

    // Hash (32 bytes, raw)
    for (size_t i = 0; i < 32 && i < hash_len; ++i) {
        msg.payload.push_back(hash[i]);
    }
    // Pad if needed
    for (size_t i = hash_len; i < 32; ++i) {
        msg.payload.push_back(0);
    }

    return msg;
}

// Phase C.2: Block transmission
P2PMessage P2PMessage::create_block(const std::string& block_hex) {
    P2PMessage msg;
    msg.command = "block";

    // Convert hex string to bytes
    msg.payload.reserve(block_hex.size() / 2);
    for (size_t i = 0; i < block_hex.size(); i += 2) {
        if (i + 1 < block_hex.size()) {
            std::string byte_str = block_hex.substr(i, 2);
            try {
                // Use strtol for safer conversion
                char* end_ptr = nullptr;
                long val = std::strtol(byte_str.c_str(), &end_ptr, 16);
                if (end_ptr != byte_str.c_str() + 2) {
                    std::cerr << "[P2P] Invalid hex byte in block: '" << byte_str << "' at position " << i << std::endl;
                    continue;
                }
                msg.payload.push_back(static_cast<uint8_t>(val));
            } catch (const std::exception& e) {
                std::cerr << "[P2P] Hex conversion error at position " << i << ": " << e.what() << std::endl;
            }
        }
    }

    return msg;
}

// Phase C.3: Headers-first sync
P2PMessage P2PMessage::create_getheaders(const std::vector<std::string>& locator) {
    P2PMessage msg;
    msg.command = "getheaders";

    // Bitcoin wire format: version(4) + varint(count) + hashes(32*count) + stophash(32)

    // Helper to encode varint
    auto encode_varint = [](std::vector<uint8_t>& out, uint64_t value) {
        if (value < 0xFD) {
            out.push_back(static_cast<uint8_t>(value));
        } else if (value <= 0xFFFF) {
            out.push_back(0xFD);
            out.push_back(static_cast<uint8_t>(value & 0xFF));
            out.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
        } else {
            out.push_back(0xFE);
            for (int i = 0; i < 4; i++) {
                out.push_back(static_cast<uint8_t>((value >> (i * 8)) & 0xFF));
            }
        }
    };

    // Helper to convert hex string to bytes
    auto hex_to_bytes = [](const std::string& hex) -> std::vector<uint8_t> {
        std::vector<uint8_t> bytes;
        for (size_t i = 0; i + 1 < hex.size(); i += 2) {
            uint8_t byte = 0;
            char c1 = hex[i], c2 = hex[i + 1];
            if (c1 >= '0' && c1 <= '9') byte = (c1 - '0') << 4;
            else if (c1 >= 'a' && c1 <= 'f') byte = (c1 - 'a' + 10) << 4;
            else if (c1 >= 'A' && c1 <= 'F') byte = (c1 - 'A' + 10) << 4;
            if (c2 >= '0' && c2 <= '9') byte |= (c2 - '0');
            else if (c2 >= 'a' && c2 <= 'f') byte |= (c2 - 'a' + 10);
            else if (c2 >= 'A' && c2 <= 'F') byte |= (c2 - 'A' + 10);
            bytes.push_back(byte);
        }
        return bytes;
    };

    // Version (4 bytes, little-endian) - use protocol version 70016
    uint32_t version = 70016;
    for (int i = 0; i < 4; i++) {
        msg.payload.push_back(static_cast<uint8_t>((version >> (i * 8)) & 0xFF));
    }

    // Hash count (varint)
    encode_varint(msg.payload, locator.size());

    // Locator hashes (32 bytes each, in wire order)
    for (const auto& hash_hex : locator) {
        auto hash_bytes = hex_to_bytes(hash_hex);
        if (hash_bytes.size() == 32) {
            msg.payload.insert(msg.payload.end(), hash_bytes.begin(), hash_bytes.end());
        } else {
            // Pad with zeros if hash is wrong size
            msg.payload.insert(msg.payload.end(), 32, 0);
        }
    }

    // Stop hash (32 bytes of zeros = get all headers)
    msg.payload.insert(msg.payload.end(), 32, 0);

    return msg;
}

P2PMessage P2PMessage::create_headers(const std::vector<std::string>& header_hexes) {
    P2PMessage msg;
    msg.command = "headers";

    // Bitcoin wire format: varint(count) + (header_bytes + varint(0))*N
    // Each header is followed by a tx_count varint (always 0 for headers message)
    // Max 2000 headers per message (Bitcoin standard)

    size_t count = std::min(header_hexes.size(), size_t(2000));

    // Helper to encode varint
    auto encode_varint = [](std::vector<uint8_t>& out, uint64_t value) {
        if (value < 0xFD) {
            out.push_back(static_cast<uint8_t>(value));
        } else if (value <= 0xFFFF) {
            out.push_back(0xFD);
            out.push_back(static_cast<uint8_t>(value & 0xFF));
            out.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
        } else if (value <= 0xFFFFFFFF) {
            out.push_back(0xFE);
            for (int i = 0; i < 4; i++) {
                out.push_back(static_cast<uint8_t>((value >> (i * 8)) & 0xFF));
            }
        } else {
            out.push_back(0xFF);
            for (int i = 0; i < 8; i++) {
                out.push_back(static_cast<uint8_t>((value >> (i * 8)) & 0xFF));
            }
        }
    };

    // Helper to decode hex string to bytes
    auto hex_to_bytes = [](const std::string& hex) -> std::vector<uint8_t> {
        std::vector<uint8_t> bytes;
        for (size_t i = 0; i + 1 < hex.size(); i += 2) {
            uint8_t byte = 0;
            for (int j = 0; j < 2; j++) {
                char c = hex[i + j];
                byte <<= 4;
                if (c >= '0' && c <= '9') byte |= (c - '0');
                else if (c >= 'a' && c <= 'f') byte |= (c - 'a' + 10);
                else if (c >= 'A' && c <= 'F') byte |= (c - 'A' + 10);
            }
            bytes.push_back(byte);
        }
        return bytes;
    };

    // Build payload
    std::vector<uint8_t> payload;

    // Header count (varint)
    encode_varint(payload, count);

    // Each header followed by tx_count = 0
    for (size_t i = 0; i < count; i++) {
        std::vector<uint8_t> header_bytes = hex_to_bytes(header_hexes[i]);
        payload.insert(payload.end(), header_bytes.begin(), header_bytes.end());

        // tx_count = 0 (varint)
        payload.push_back(0x00);
    }

    msg.payload = payload;
    return msg;
}

std::vector<uint8_t> P2PMessage::serialize() const {
    std::vector<uint8_t> result;

    // Magic bytes (4 bytes) — read from chainparams via MagicBytes() so
    // the wire format always matches what consensus / SelectParams set.
    const uint32_t magic = MagicBytes();
    for (int i = 0; i < 4; i++) {
        result.push_back((magic >> (i * 8)) & 0xFF);
    }
    
    // Command (12 bytes, null-padded)
    for (size_t i = 0; i < 12; i++) {
        if (i < command.length()) {
            result.push_back(static_cast<uint8_t>(command[i]));
        } else {
            result.push_back(0);
        }
    }
    
    // Payload length (4 bytes)
    uint32_t payload_len = static_cast<uint32_t>(payload.size());
    for (int i = 0; i < 4; i++) {
        result.push_back((payload_len >> (i * 8)) & 0xFF);
    }
    
    // Checksum (4 bytes) - first 4 bytes of double-SHA256 (Bitcoin protocol)
    std::vector<uint8_t> hash = Dinero::Common::double_sha256_raw(payload.data(), payload.size());
    // Bitcoin checksum is first 4 bytes of double-SHA256, little-endian
    for (int i = 0; i < 4; i++) {
        result.push_back(hash[i]);
    }
    
    // Payload
    result.insert(result.end(), payload.begin(), payload.end());
    
    return result;
}

std::unique_ptr<P2PMessage> P2PMessage::deserialize(const std::vector<uint8_t>& data) {
    if (data.size() < 24) return nullptr; // Minimum header size
    
    // Check magic bytes
    uint32_t magic = 0;
    for (int i = 0; i < 4; i++) {
        magic |= (static_cast<uint32_t>(data[i]) << (i * 8));
    }
    if (magic != MagicBytes()) return nullptr;
    
    auto msg = std::make_unique<P2PMessage>();
    
    // Extract command (12 bytes)
    msg->command.clear();
    for (size_t i = 4; i < 16; i++) {
        if (data[i] != 0) {
            msg->command += static_cast<char>(data[i]);
        }
    }
    
    // Extract payload length
    uint32_t payload_len = 0;
    for (int i = 0; i < 4; i++) {
        payload_len |= (static_cast<uint32_t>(data[16 + i]) << (i * 8));
    }
    
    // Extract checksum
    uint32_t checksum = 0;
    for (int i = 0; i < 4; i++) {
        checksum |= (static_cast<uint32_t>(data[20 + i]) << (i * 8));
    }
    msg->checksum = checksum;
    
    // Extract payload
    if (data.size() < 24 + payload_len) return nullptr;
    
    msg->payload.assign(data.begin() + 24, data.begin() + 24 + payload_len);
    
    // Verify checksum (first 4 bytes of double-SHA256)
    std::vector<uint8_t> hash = Dinero::Common::double_sha256_raw(msg->payload.data(), msg->payload.size());
    uint32_t calculated_checksum = hash[0] | (static_cast<uint32_t>(hash[1]) << 8) |
                                   (static_cast<uint32_t>(hash[2]) << 16) | (static_cast<uint32_t>(hash[3]) << 24);
    if (calculated_checksum != checksum) return nullptr;
    
    return msg;
}

P2PManager::P2PManager(uint16_t listen_port, const std::string& external_ip)
    : listen_port_(listen_port), user_agent_(USER_AGENT), external_ip_(external_ip), protocol_version_(PROTOCOL_VERSION) {

    if (!external_ip_.empty()) {
        std::cout << "[P2P] External IP configured: " << external_ip_ << " (will reject self-connections)" << std::endl;
    }

#ifdef _WIN32
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);
#endif
}

P2PManager::~P2PManager() {
    stop();
    
#ifdef _WIN32
    WSACleanup();
#endif
}

void P2PManager::set_user_agent(const std::string& user_agent) {
    user_agent_ = user_agent.empty() ? USER_AGENT : user_agent;
}

void P2PManager::set_address_manager(dinero::p2p::AddressManager* address_manager) {
    address_manager_ = address_manager;
    std::cout << "[P2P] Address manager bridge "
              << (address_manager_ ? "enabled" : "disabled") << std::endl;
}

// NAT traversal Phase 1A: symmetric post-version, pre-verack identity
// exchange. Caller MUST have:
//   - already exchanged version messages (so peer->their_nonce and our own
//     peer->our_nonce are populated)
//   - verified both sides advertise NODE_DINERO_V2
//   - confirmed node_identity_ is non-null
// Both sides reach this function simultaneously (after sending their own
// version + parsing the remote one), so the send-then-receive ordering is
// deadlock-free: each side's ~107-byte dineroid fits the kernel send buffer.
//
// Failure modes are all non-fatal: send fails / receive times out /
// payload malformed / signature invalid → log and return. The peer stays
// connected (identity_proven=false), the relay subsystem in later phases
// just refuses to advertise that peer's reachability.
void P2PManager::ExchangeDineroId(PeerInfo* peer) {
    if (!peer || !node_identity_) return;

    // Send our dineroid first. Sign over peer->their_nonce so the receiver
    // can verify against the nonce they themselves embedded in their version.
    auto msg = P2PMessage::create_dineroid(*node_identity_, peer->their_nonce);
    if (msg.payload.empty()) {
        std::cout << "[Handshake] dineroid: failed to construct outgoing message for "
                  << peer->to_string() << "; continuing legacy" << std::endl;
        return;
    }
    if (!send_message(peer->socket_fd, msg)) {
        std::cout << "[Handshake] dineroid: send failed to " << peer->to_string()
                  << "; continuing legacy" << std::endl;
        return;
    }

    // Receive remote dineroid. receive_message blocks with its existing
    // socket-level timeout; if the remote peer is on rc8 or older they
    // simply won't send and we'll time out → log and continue.
    auto remote = receive_message(peer->socket_fd);
    if (!remote) {
        std::cout << "[Handshake] dineroid: no response from " << peer->to_string()
                  << " (likely older peer); continuing legacy" << std::endl;
        return;
    }
    if (remote->command != "dineroid") {
        std::cout << "[Handshake] dineroid: expected 'dineroid' but got '"
                  << remote->command << "' from " << peer->to_string()
                  << "; continuing legacy" << std::endl;
        return;
    }
    // Wire format: pubkey(33) | sig_len(1) | sig(sig_len)
    if (remote->payload.size() < 34) {
        std::cout << "[Handshake] dineroid: short payload (" << remote->payload.size()
                  << " bytes) from " << peer->to_string() << std::endl;
        return;
    }
    const uint8_t sig_len = remote->payload[33];
    if (sig_len == 0 || sig_len > 72 ||
        remote->payload.size() != 34u + sig_len) {
        std::cout << "[Handshake] dineroid: invalid sig_len " << static_cast<int>(sig_len)
                  << " from " << peer->to_string() << std::endl;
        return;
    }

    // Verify the signature is over OUR own version nonce.
    uint8_t our_nonce_le[8];
    for (int i = 0; i < 8; i++) {
        our_nonce_le[i] = static_cast<uint8_t>((peer->our_nonce >> (i * 8)) & 0xFF);
    }
    const uint8_t* pubkey = remote->payload.data();
    const uint8_t* sig = remote->payload.data() + 34;
    if (!dinero::daemon::NodeIdentity::verify_bytes(our_nonce_le, 8,
                                                    sig, sig_len, pubkey)) {
        std::cout << "[Handshake] dineroid: signature verification FAILED for "
                  << peer->to_string() << "; identity not proven" << std::endl;
        return;
    }

    // Success — derive node_id = HASH160(pubkey) and persist on the peer.
    std::copy(pubkey, pubkey + 33, peer->their_pubkey.begin());
    HASH160(pubkey, 33, peer->their_node_id.data());
    peer->identity_proven = true;

    std::ostringstream id_hex;
    id_hex << std::hex << std::setfill('0');
    for (auto b : peer->their_node_id) {
        id_hex << std::setw(2) << static_cast<unsigned int>(b);
    }
    std::cout << "[Handshake] dineroid: identity proven for " << peer->to_string()
              << " (node_id=" << id_hex.str() << ")" << std::endl;
}

// NAT traversal Phase 1A.2 / BIP155: symmetric sendaddrv2 negotiation.
// Called from perform_handshake right after ExchangeDineroId so it inherits
// the same NODE_DINERO_V2 gate. Both peers send their (empty-payload)
// sendaddrv2 in parallel — like dineroid the send-then-receive ordering is
// deadlock-free at this fragment size.
//
// Failure modes are all non-fatal: rc7- peers won't send sendaddrv2, the
// receive will time out (or the message will be a different command if a
// post-verack message races us); either way the peer flag stays false and
// legacy `addr` continues to flow. Only effect of failure is "we'll send
// legacy addr to this peer instead of addrv2."
void P2PManager::ExchangeSendAddrV2(PeerInfo* peer) {
    if (!peer) return;

    auto out = P2PMessage::create_sendaddrv2();
    if (!send_message(peer->socket_fd, out)) {
        std::cout << "[Handshake] sendaddrv2: send failed to " << peer->to_string()
                  << "; legacy addr still works" << std::endl;
        return;
    }

    auto remote = receive_message(peer->socket_fd);
    if (!remote) {
        std::cout << "[Handshake] sendaddrv2: no response from " << peer->to_string()
                  << " (older peer?); falling back to legacy addr" << std::endl;
        return;
    }
    if (remote->command != "sendaddrv2") {
        std::cout << "[Handshake] sendaddrv2: expected 'sendaddrv2' but got '"
                  << remote->command << "' from " << peer->to_string()
                  << "; falling back to legacy addr" << std::endl;
        return;
    }
    peer->peer_wants_addrv2 = true;
    std::cout << "[Handshake] sendaddrv2: peer " << peer->to_string()
              << " supports addrv2 (BIP155 typed addr gossip enabled)" << std::endl;
}

// BIP155 addrv2 ingestion path. Decodes via dinero::p2p::DecodeAddrV2,
// then funnels IPV4 / IPV6 entries into the same remember_peer_address()
// pipe that legacy handle_addr uses. TORV3 / I2P entries are parsed but
// dropped: ingesting them requires (a) onion-string codec (TORv3 checksum
// needs SHA3) and (b) addrman storage that remembers the network type.
// Both land in a follow-up commit; until then we explicitly count and log
// the dropped entries so operators can see we're seeing them.
void P2PManager::handle_addrv2(const std::string& peer_address, const P2PMessage& message) {
    std::vector<dinero::p2p::AddrV2Entry> entries;
    std::string err;
    if (!dinero::p2p::DecodeAddrV2(message.payload, &entries, &err)) {
        std::cout << "[P2P] handle_addrv2: malformed payload from " << peer_address
                  << ": " << err << std::endl;
        return;
    }

    int ipv4_added = 0;
    int ipv6_seen = 0;
    int torv3_skipped = 0;
    int i2p_skipped = 0;
    std::vector<std::pair<std::string, uint16_t>> new_for_relay;

    for (const auto& e : entries) {
        switch (e.net) {
            case dinero::p2p::NetworkType::IPV4: {
                // addr is exactly 4 bytes per network type validation.
                char buf[16];
                std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u",
                              e.addr[0], e.addr[1], e.addr[2], e.addr[3]);
                std::string addr(buf);
                if (e.port == 0) continue;
                if (addr == "127.0.0.1" || addr == "0.0.0.0") continue;
                if (!external_ip_.empty() && addr == external_ip_ && e.port == listen_port_) continue;
                // Same defensive ephemeral-port filter as handle_addr —
                // only allow our own listen_port_, since that's the
                // canonical Dinero P2P port.
                if (e.port != listen_port_) continue;
                if (remember_peer_address(addr, e.port, peer_address)) {
                    ipv4_added++;
                    new_for_relay.emplace_back(addr, e.port);
                }
                break;
            }
            case dinero::p2p::NetworkType::IPV6:
                ipv6_seen++;
                // TODO Phase A2 follow-up: format the 16 bytes as a
                // bracketed IPv6 string and pass through the same
                // remember_peer_address path. Existing addrman is
                // string-keyed so the format must be canonical.
                break;
            case dinero::p2p::NetworkType::TORV3:
                torv3_skipped++;
                // TODO Phase A2 follow-up: encode raw 32-byte pubkey
                // back to "xxx.onion" using the TORv3 base32 + SHA3
                // checksum scheme. Once that lands the existing
                // SOCKS5 dial path can reach these peers.
                break;
            case dinero::p2p::NetworkType::I2P:
                i2p_skipped++;
                break;
            case dinero::p2p::NetworkType::Unknown:
                break;
        }
    }

    if (ipv4_added > 0 || ipv6_seen > 0 || torv3_skipped > 0 || i2p_skipped > 0) {
        std::cout << "[P2P] addrv2 from " << peer_address
                  << ": ipv4_added=" << ipv4_added
                  << " ipv6_seen=" << ipv6_seen
                  << " torv3_skipped=" << torv3_skipped
                  << " i2p_skipped=" << i2p_skipped
                  << " (TORv3/I2P/IPv6 ingestion is a Phase A2 follow-up)"
                  << std::endl;
        if (ipv4_added > 0 && !peers_file_path_.empty()) {
            save_peers_with_seeds(peers_file_path_);
        }
        if (!new_for_relay.empty()) {
            relay_addresses_to_peers(peer_address, new_for_relay);
        }
    }
}

void P2PManager::set_node_identity(std::shared_ptr<dinero::daemon::NodeIdentity> identity) {
    node_identity_ = std::move(identity);
    if (node_identity_) {
        std::cout << "[P2P] Node identity wired (node_id=" << node_identity_->get_node_id()
                  << ") — `dineroid` handshake enabled with NODE_DINERO_V2 peers" << std::endl;
    } else {
        std::cout << "[P2P] Node identity cleared — `dineroid` handshake disabled" << std::endl;
    }
}

void P2PManager::set_onion_proxy(const std::string& proxy_host, uint16_t proxy_port, bool log_change) {
    std::lock_guard<std::mutex> lock(peers_mutex_);
    onion_proxy_host_ = proxy_host;
    onion_proxy_port_ = proxy_port;
    if (!log_change) {
        return;
    }
    if (!onion_proxy_host_.empty() && onion_proxy_port_ != 0) {
        std::cout << "[P2P] Onion transport enabled via SOCKS5 proxy "
                  << onion_proxy_host_ << ":" << onion_proxy_port_ << std::endl;
    } else {
        std::cout << "[P2P] Onion transport disabled" << std::endl;
    }
}

bool P2PManager::onion_proxy_enabled() const {
    std::lock_guard<std::mutex> lock(peers_mutex_);
    return !onion_proxy_host_.empty() && onion_proxy_port_ != 0;
}

std::string P2PManager::onion_proxy_endpoint() const {
    std::lock_guard<std::mutex> lock(peers_mutex_);
    if (onion_proxy_host_.empty() || onion_proxy_port_ == 0) {
        return "";
    }
    return onion_proxy_host_ + ":" + std::to_string(onion_proxy_port_);
}

bool P2PManager::probe_onion_proxy(std::string* message) {
    std::string proxy_host;
    uint16_t proxy_port = 0;
    {
        std::lock_guard<std::mutex> lock(peers_mutex_);
        proxy_host = onion_proxy_host_;
        proxy_port = onion_proxy_port_;
    }

    if (proxy_host.empty() || proxy_port == 0) {
        if (message) {
            *message = "onion proxy not configured";
        }
        return false;
    }
    if (IsOnionAddress(proxy_host)) {
        if (message) {
            *message = "onion proxy endpoint cannot itself be an onion address";
        }
        return false;
    }

    const int socket_fd = create_client_socket(proxy_host, proxy_port);
    if (socket_fd < 0) {
        if (message) {
            *message = "SOCKS5 proxy not reachable at " + proxy_host + ":" +
                       std::to_string(proxy_port);
        }
        return false;
    }

    const uint8_t greeting[] = {0x05, 0x01, 0x00};
    uint8_t greeting_reply[2] = {};
    const bool ok = SendAll(socket_fd, greeting, sizeof(greeting)) &&
                    RecvAll(socket_fd, greeting_reply, sizeof(greeting_reply)) &&
                    greeting_reply[0] == 0x05 && greeting_reply[1] == 0x00;
    close_socket(socket_fd);

    if (message) {
        *message = ok
            ? "SOCKS5 proxy reachable at " + proxy_host + ":" + std::to_string(proxy_port)
            : "SOCKS5 proxy reachable but rejected no-auth handshake at " +
              proxy_host + ":" + std::to_string(proxy_port);
    }
    return ok;
}

void P2PManager::add_advertised_address(const std::string& address, uint16_t port) {
    if (IsLocalOrWildcardAddress(address) || port == 0) {
        std::cout << "[P2P] Skipping unroutable advertised address: "
                  << address << ":" << port << std::endl;
        return;
    }

    if (!IsAdvertisableAddress(address, port)) {
        std::cout << "[P2P] Skipping non-routable advertised address: "
                  << address << ":" << port << std::endl;
        return;
    }

    std::lock_guard<std::mutex> lock(peers_mutex_);
    const std::string key = AddressKey(address, port);
    for (const auto& existing : advertised_addresses_) {
        if (AddressKey(existing.first, existing.second) == key) {
            return;
        }
    }
    advertised_addresses_.emplace_back(address, port);
    std::cout << "[P2P] Advertising reachable address: " << key << std::endl;
}

std::vector<std::pair<std::string, uint16_t>> P2PManager::get_advertised_addresses() const {
    std::lock_guard<std::mutex> lock(peers_mutex_);
    return advertised_addresses_;
}

bool P2PManager::start() {
    if (running_) {
        std::cout << "P2P manager already running" << std::endl;
        return true;
    }
    
    shutdown_requested_ = false;
    network_active_.store(true, std::memory_order_release);
    
    // Start listen thread
    listen_thread_ = std::make_unique<std::thread>(&P2PManager::listen_loop, this);

    // Start connection manager thread
    connection_manager_thread_ = std::make_unique<std::thread>(&P2PManager::connection_manager_loop, this);

    // Start async outbox thread
    outbox_thread_ = std::make_unique<std::thread>(&P2PManager::outbox_loop, this);

    // Phase C: Start adaptive keepalive thread
    keepalive_thread_ = std::make_unique<std::thread>(&P2PManager::keepalive_loop, this);

    running_ = true;

    std::cout << "P2P manager started on port " << listen_port_ << " (async outbox + keepalive enabled)" << std::endl;
    return true;
}

// Ring 3 Phase 4c: TS1-compliant stop()
// ======================================
// TS1 Property: Join-before-erase
// All peer threads MUST be joined before erasing peers from map.
// This prevents use-after-free by ensuring threads have exited before destruction.
//
// THREAD-SAFE: Multiple concurrent calls to stop() are safe (idempotent)
void P2PManager::stop() {
    // Atomically check and transition to shutdown state
    bool expected = true;
    if (!running_.compare_exchange_strong(expected, false)) {
        // Already stopped or stopping
        return;
    }

    // Phase C (v8 peer discovery): final save BEFORE threads shut down.
    // connected_peers_ and seed_nodes_ are still populated here; doing
    // this after cleanup would persist an empty set. Captures any
    // last_seen_unix / new-address state accumulated since the last
    // periodic keepalive_loop save.
    if (!peers_file_path_.empty()) {
        save_peers_with_seeds(peers_file_path_);
    }

    shutdown_requested_ = true;

    // Ring 3 Phase 4e: TS3 - Wake up all waiting threads immediately
    outbox_cv_.notify_all();              // Wake up outbox thread
    keepalive_cv_.notify_all();           // Wake up keepalive thread
    connection_manager_cv_.notify_all();  // Wake up connection manager thread

    // Interrupt all peer sockets BEFORE joining threads.
    // Peer threads block on select(1s) — closing sockets makes select() return
    // immediately with an error, so threads exit without waiting for the timeout.
    {
        std::lock_guard<std::mutex> lock(peers_mutex_);
        for (auto& pair : connected_peers_) {
            if (pair.second->socket_fd >= 0) {
                ::shutdown(pair.second->socket_fd, SHUT_RDWR);
            }
        }
    }

    // Wait for management threads to finish
    if (listen_thread_ && listen_thread_->joinable()) {
        listen_thread_->join();
    }

    if (connection_manager_thread_ && connection_manager_thread_->joinable()) {
        connection_manager_thread_->join();
    }

    if (outbox_thread_ && outbox_thread_->joinable()) {
        outbox_thread_->join();
    }

    // Phase C: Stop keepalive thread
    if (keepalive_thread_ && keepalive_thread_->joinable()) {
        keepalive_thread_->join();
    }

    // TS1 CRITICAL SECTION: Join all peer threads BEFORE erasing peers
    // =================================================================
    // This is the core TS1 invariant: join-before-erase

    // Step 1: Join all peer threads (blocks until all peer_handler_loop exit)
    for (auto& thread : peer_threads_) {
        if (thread && thread->joinable()) {
            thread->join();
        }
    }
    peer_threads_.clear();

    // Step 2: Now that all threads are joined, transition peers to JOINED state
    {
        std::lock_guard<std::mutex> lock(peers_mutex_);
        for (auto& pair : connected_peers_) {
            // TS1 ASSERTION A3: Before transitioning to JOINED, peer must be in STOPPING state
            // (or RUNNING if thread exited without reaching cleanup)
            auto state = pair.second->lifetime_state.load();
            assert(state == PeerLifetimeState::RUNNING ||
                   state == PeerLifetimeState::STOPPING ||
                   state == PeerLifetimeState::ALLOCATED);
            (void)state;

            // TS1 Invariant L3: Transition to JOINED state after thread join
            pair.second->lifetime_state.store(PeerLifetimeState::JOINED);

            // Close socket (may already be closed by cleanup_peer, idempotent)
            close_socket(pair.second->socket_fd);
        }
    }

    // Step 3: Only NOW is it safe to erase peers (all threads exited)
    {
        std::lock_guard<std::mutex> lock(peers_mutex_);
        connected_peers_.clear();  // shared_ptr destroyed → DESTROYED state
    }
    {
        std::lock_guard<std::mutex> lock(socket_send_mutexes_guard_);
        socket_send_mutexes_.clear();
    }

    // NOTE: running_ was already set to false atomically at the beginning
    std::cout << "P2P manager stopped" << std::endl;
}

void P2PManager::add_seed_node(const std::string& address, uint16_t port) {
    bool inserted = false;
    {
        std::lock_guard<std::mutex> lock(peers_mutex_);
        // Deduplicate — same seed can come from CLI, hardcoded list, and peers.dat
        for (const auto& s : seed_nodes_) {
            if (s.first == address && s.second == port) return;
        }
        seed_nodes_.emplace_back(address, port);
        inserted = true;
    }

    if (inserted && address_manager_) {
        auto network_addr = NetworkAddressForPeer(address, port);
        if (network_addr.isValid() && network_addr.isRoutable()) {
            address_manager_->addAddress(network_addr, "seed");
        }
    }

    std::cout << "Added seed node: " << address << ":" << port << std::endl;
}

bool P2PManager::remove_seed_node(const std::string& address, uint16_t port) {
    std::lock_guard<std::mutex> lock(peers_mutex_);
    const auto before = seed_nodes_.size();
    seed_nodes_.erase(
        std::remove_if(seed_nodes_.begin(), seed_nodes_.end(),
                       [&](const auto& seed) { return seed.first == address && seed.second == port; }),
        seed_nodes_.end());
    return seed_nodes_.size() != before;
}

std::vector<std::pair<std::string, uint16_t>> P2PManager::get_seed_nodes() const {
    std::lock_guard<std::mutex> lock(peers_mutex_);
    return seed_nodes_;
}

bool P2PManager::remember_peer_address(const std::string& address,
                                       uint16_t port,
                                       const std::string& source_peer) {
    if (IsLocalOrWildcardAddress(address) || port == 0) {
        return false;
    }
    if (!external_ip_.empty() && address == external_ip_ && port == listen_port_) {
        return false;
    }
    if (port != listen_port_) {
        return false;
    }
    if (is_peer_banned(address, port)) {
        return false;
    }

    if (!IsAdvertisableAddress(address, port)) {
        return false;
    }

    add_seed_node(address, port);

    if (address_manager_ && !IsOnionAddress(address)) {
        auto network_addr = NetworkAddressForPeer(address, port);
        address_manager_->addAddress(network_addr, source_peer);
    }
    return true;
}

void P2PManager::mark_peer_address_attempt(const std::string& address,
                                           uint16_t port,
                                           bool success) {
    if (!address_manager_ || IsLocalOrWildcardAddress(address) || port == 0) {
        return;
    }
    if (!IsAdvertisableAddress(address, port)) {
        return;
    }

    if (IsOnionAddress(address)) {
        return;
    }

    auto network_addr = NetworkAddressForPeer(address, port);
    address_manager_->addAddress(network_addr, success ? "connect-success" : "connect-failure");
    address_manager_->markAttempt(network_addr, success);
    if (success) {
        address_manager_->markGood(network_addr);
    }
}

std::vector<std::pair<std::string, uint16_t>> P2PManager::get_local_advertised_addresses() const {
    std::lock_guard<std::mutex> lock(peers_mutex_);
    return advertised_addresses_;
}

std::vector<std::pair<std::string, uint16_t>> P2PManager::collect_advertisable_addresses(
    size_t max_count) const {
    std::vector<std::pair<std::string, uint16_t>> result;
    std::unordered_set<std::string> seen;

    auto add_address = [&](const std::string& address, uint16_t port) {
        if (result.size() >= max_count || IsLocalOrWildcardAddress(address) || port == 0) {
            return;
        }
        if (!IsAdvertisableAddress(address, port)) {
            return;
        }
        const std::string key = AddressKey(address, port);
        if (seen.insert(key).second) {
            result.emplace_back(address, port);
        }
    };

    {
        std::lock_guard<std::mutex> lock(peers_mutex_);
        for (const auto& advertised : advertised_addresses_) {
            add_address(advertised.first, advertised.second);
        }
        for (const auto& pair : connected_peers_) {
            const auto& peer = pair.second;
            if (!peer->is_connected || !peer->is_outbound) {
                continue;
            }
            add_address(peer->address, peer->port);
        }
    }

    if (address_manager_ && result.size() < max_count) {
        const auto addrman_addresses = address_manager_->getAdvertisableAddresses(max_count - result.size());
        for (const auto& addr : addrman_addresses) {
            add_address(addr.ip, addr.port);
        }
    }

    return result;
}

bool P2PManager::send_addr_list_to_socket(
    int socket_fd,
    const std::vector<std::pair<std::string, uint16_t>>& addresses) {
    if (addresses.empty()) {
        return true;
    }

    std::vector<PeerInfo> peer_infos;
    peer_infos.reserve(addresses.size());
    for (const auto& [address, port] : addresses) {
        peer_infos.push_back(PeerInfoForAddress(address, port));
    }
    auto addr_msg = P2PMessage::create_addr(peer_infos);
    return send_message(socket_fd, addr_msg);
}

bool P2PManager::connect_to_peer(const std::string& address, uint16_t port) {
    if (!network_active_.load(std::memory_order_acquire)) {
        return false;
    }

    // 🚫 SELF-LOOP PREVENTION: Reject connections to self
    // NOTE: Localhost check relaxed to allow SSH tunnel testing (regtest)
    if (port == listen_port_) {
        // Check localhost addresses - WARN but allow for tunnel testing
        if (address == "127.0.0.1" || address == "localhost" ||
            address == "::1" || address == "0.0.0.0" || address == "::") {
            std::cout << "[P2P] ⚠️  Warning: localhost connection to own port (tunnel mode): " << address << ":" << port << std::endl;
            // Allow for SSH tunnel testing - true self-connections will fail at TCP level anyway
        }
        // Check configured external IP (still reject - this is a true self-connection)
        if (!external_ip_.empty() && address == external_ip_) {
            std::cout << "[P2P] ⚠️  Rejected self-connection to external IP: " << address << ":" << port << std::endl;
            return false;
        }
        // Check every IP bound to a local interface. This catches the case where
        // `external_ip_` was not configured but the dial target is one of our own
        // NICs (the v5-reset bug — see include/network/local_interfaces.h).
        // Without this guard, MAINNET_SEED_IPS / addrman / peers.dat replay all
        // dial the local node which then poisons the BlockDownloadScheduler with
        // a peer slot frozen at our own chain tip.
        if (dinero::network::IsLocalInterfaceIp(address)) {
            std::cout << "[P2P] ⚠️  Rejected self-connection to local interface IP: "
                      << address << ":" << port << std::endl;
            return false;
        }
    }

    // Resolve DNS to IP for dedup check (prevents duplicate connections to same IP
    // when seed list contains both raw IP and DNS name for the same server)
    std::string resolved_ip = address;
    if (!IsOnionAddress(address)) {
        struct sockaddr_in sa;
        if (inet_pton(AF_INET, address.c_str(), &sa.sin_addr) <= 0) {
            struct addrinfo hints{}, *res = nullptr;
            hints.ai_family = AF_INET;
            hints.ai_socktype = SOCK_STREAM;
            if (getaddrinfo(address.c_str(), nullptr, &hints, &res) == 0 && res) {
                char buf[INET_ADDRSTRLEN];
                inet_ntop(AF_INET,
                          &((struct sockaddr_in*)res->ai_addr)->sin_addr,
                          buf, sizeof(buf));
                resolved_ip = buf;
                freeaddrinfo(res);
            }
        }
    }

    if (is_peer_banned(address, port) || is_peer_banned(resolved_ip, port)) {
        std::cout << "[P2P] Skipping banned peer: " << address << ":" << port
                  << " (resolved " << resolved_ip << ")" << std::endl;
        return false;
    }

    // Check if already connected or connection in progress
    std::string peer_key = address + ":" + std::to_string(port);
    {
        std::lock_guard<std::mutex> lock(peers_mutex_);
        auto it = connected_peers_.find(peer_key);
        if (it != connected_peers_.end() && it->second->is_connected) {
            return true; // Already connected
        }
        // Prevent duplicate connection attempts (race between loop iterations)
        if (connecting_peers_.count(peer_key) > 0) {
            return true; // Connection already in progress
        }
        // Check if already connected to the same resolved IP (different seed entry)
        for (const auto& [key, peer] : connected_peers_) {
            if (peer->is_connected && peer->address == resolved_ip && peer->port == port) {
                std::cout << "[P2P] Skipping " << peer_key << " — already connected to "
                          << resolved_ip << ":" << port << std::endl;
                return true;
            }
        }
        connecting_peers_.insert(peer_key);
    }

    // Create outbound connection
    int socket_fd = create_client_socket(address, port);
    if (socket_fd < 0) {
        std::cerr << "Failed to connect to " << peer_key << std::endl;
        mark_peer_address_attempt(resolved_ip, port, false);
        // Remove from connecting set on failure
        std::lock_guard<std::mutex> lock(peers_mutex_);
        connecting_peers_.erase(peer_key);
        return false;
    }

    // Create peer info
    // Ring 3 Phase 4c: Use shared_ptr for TS1 compliance
    auto peer = std::make_shared<PeerInfo>();
    peer->address = resolved_ip;  // Store resolved IP for dedup (not DNS name)
    peer->port = port;
    peer->is_outbound = true;
    peer->is_connected = false;
    peer->socket_fd = socket_fd;
    peer->connected_since = std::chrono::system_clock::now();
    peer->last_message_at = peer->connected_since;
    peer->last_seen = std::chrono::system_clock::now();

    // Set send timeout as safety measure
    set_socket_send_timeout(socket_fd, SEND_TIMEOUT_SEC);

    std::cout << "Connected to peer: " << peer_key << " (send timeout: " << SEND_TIMEOUT_SEC << "s)" << std::endl;

    // Start peer handler thread
    // Ring 3 Phase 4c: Pass shared_ptr (copied, not moved) for TS1 compliance
    peer_threads_.emplace_back(
        std::make_unique<std::thread>(&P2PManager::peer_handler_loop, this, peer)
    );

    return true;
}

void P2PManager::listen_loop() {
    int listen_socket = create_listen_socket();
    if (listen_socket < 0) {
        std::cerr << "Failed to create listen socket" << std::endl;
        return;
    }
    
    while (!shutdown_requested_) {
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(listen_socket, &read_fds);
        
        struct timeval timeout;
        timeout.tv_sec = 1;
        timeout.tv_usec = 0;
        
        int activity = select(listen_socket + 1, &read_fds, nullptr, nullptr, &timeout);
        
        if (activity < 0 && !shutdown_requested_) {
            std::cerr << "Select error in P2P listen loop" << std::endl;
            break;
        }
        
        if (activity > 0 && FD_ISSET(listen_socket, &read_fds)) {
            struct sockaddr_in client_addr;
            socklen_t client_len = sizeof(client_addr);
            
            int client_socket = accept(listen_socket, (struct sockaddr*)&client_addr, &client_len);
            if (client_socket >= 0) {
                if (!network_active_.load(std::memory_order_acquire)) {
                    close_socket(client_socket);
                    continue;
                }
                std::string client_address = inet_ntoa(client_addr.sin_addr);
                handle_incoming_connection(client_socket, client_address);
            }
        }
    }

    // Clear listening state on shutdown
    socket_listening_.store(false, std::memory_order_release);

    close_socket(listen_socket);
}

void P2PManager::connection_manager_loop() {
    while (!shutdown_requested_) {
        if (!network_active_.load(std::memory_order_acquire)) {
            std::unique_lock<std::mutex> lock(connection_manager_mutex_);
            connection_manager_cv_.wait_for(lock, std::chrono::seconds(1), [this] {
                return shutdown_requested_.load() || network_active_.load(std::memory_order_acquire);
            });
            continue;
        }

        // Try to connect to bootstrap/discovered peers. Seed nodes are the
        // bootstrap surface; addrman is the Bitcoin-style rolling address book
        // fed by addr/getaddr relay.
        // TS2 COMPLIANT: Collect connection targets inside lock, connect outside lock
        std::vector<std::pair<std::string, uint16_t>> seeds_to_connect;
        std::vector<dinero::p2p::NetworkAddress> addrman_candidates;
        if (address_manager_) {
            addrman_candidates = address_manager_->getAddresses(MAX_OUTBOUND_CONNECTIONS);
        }
        {
            std::lock_guard<std::mutex> lock(peers_mutex_);

            size_t active_peer_count = 0;
            // Collect resolved IPs of already-connected peers for dedup
            std::unordered_set<std::string> connected_ips;
            for (const auto& pair : connected_peers_) {
                if (pair.second->is_connected) {
                    active_peer_count++;
                    connected_ips.insert(pair.second->address);
                }
            }

            auto consider_candidate = [&](const std::string& address, uint16_t port) {
                if (active_peer_count + seeds_to_connect.size() >= MAX_OUTBOUND_CONNECTIONS) {
                    return;
                }
                if (address.empty() || port == 0) {
                    return;
                }

                std::string peer_key = address + ":" + std::to_string(port);

                // Skip if already connected (by peer key)
                auto it = connected_peers_.find(peer_key);
                if (it != connected_peers_.end() && it->second->is_connected) {
                    return;
                }

                // Skip if connection already in progress (prevents duplicate handlers)
                if (connecting_peers_.count(peer_key) > 0) {
                    return;
                }

                // Resolve DNS seeds to IP and skip if already connected to that IP
                // (prevents duplicate connections when both "seed1.dinero-coin.com" and
                // its raw IP "172.93.160.131" appear in the seed list)
                std::string resolved_ip = address;
                struct sockaddr_in sa;
                if (inet_pton(AF_INET, address.c_str(), &sa.sin_addr) <= 0) {
                    // Not a raw IP — resolve DNS
                    struct addrinfo hints{}, *res = nullptr;
                    hints.ai_family = AF_INET;
                    hints.ai_socktype = SOCK_STREAM;
                    if (getaddrinfo(address.c_str(), nullptr, &hints, &res) == 0 && res) {
                        char buf[INET_ADDRSTRLEN];
                        inet_ntop(AF_INET,
                                  &((struct sockaddr_in*)res->ai_addr)->sin_addr,
                                  buf, sizeof(buf));
                        resolved_ip = buf;
                        freeaddrinfo(res);
                    }
                }
                if (connected_ips.count(resolved_ip) > 0) {
                    return;  // Already connected to this IP via another seed entry
                }

                seeds_to_connect.emplace_back(address, port);
            };

            for (const auto& seed : seed_nodes_) {
                consider_candidate(seed.first, seed.second);
            }
            for (const auto& candidate : addrman_candidates) {
                consider_candidate(candidate.ip, candidate.port);
            }
        }

        // TS2 COMPLIANT: Perform blocking connect operations outside lock
        for (const auto& seed : seeds_to_connect) {
            connect_to_peer(seed.first, seed.second);
        }

        // ✅ LIGHTWEIGHT KEEPALIVE: Send PING every 30s to prevent NAT timeout
        {
            std::vector<std::string> peers_to_ping;
            auto now = std::chrono::steady_clock::now();

            // Collect peers that need pings (avoiding nested locks)
            {
                std::lock_guard<std::mutex> lock(peers_mutex_);
                for (auto& [peer_key, peer] : connected_peers_) {
                    if (!peer->is_connected) continue;

                    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - peer->last_ping_sent);
                    if (elapsed.count() >= 30) {
                        peers_to_ping.push_back(peer_key);
                        peer->last_ping_sent = now;
                    }
                }
            }

            // Send pings outside the lock to avoid deadlock
            for (const auto& peer_key : peers_to_ping) {
                uint64_t nonce = SecureRandom::GetUInt64();
                auto ping_msg = P2PMessage::create_ping(nonce);
                send_to_peer(peer_key, ping_msg);
            }
        }

        // Ring 3 Phase 4e: TS3 Fix - Interruptible wait instead of sleep_for
        // Allows immediate wakeup on shutdown, achieving full TS3.4 compliance
        {
            std::unique_lock<std::mutex> lock(connection_manager_mutex_);
            connection_manager_cv_.wait_for(lock, std::chrono::seconds(10),
                [this]{ return shutdown_requested_.load(); });
        }
    }
}

void P2PManager::handle_incoming_connection(int client_socket, const std::string& client_address) {
    if (!network_active_.load(std::memory_order_acquire)) {
        close_socket(client_socket);
        return;
    }

    // Enforce inbound connection caps to prevent resource exhaustion
    {
        std::lock_guard<std::mutex> lock(peers_mutex_);
        size_t inbound_count = 0;
        size_t same_ip_count = 0;
        for (const auto& [key, peer] : connected_peers_) {
            if (!peer->is_outbound && peer->is_connected) {
                ++inbound_count;
                if (peer->address == client_address) {
                    ++same_ip_count;
                }
            }
        }
        if (inbound_count >= MAX_INBOUND_CONNECTIONS) {
            std::cout << "[P2P] Inbound connection limit reached (" << MAX_INBOUND_CONNECTIONS
                      << "), rejecting " << client_address << std::endl;
            close_socket(client_socket);
            return;
        }
        if (same_ip_count >= MAX_INBOUND_PER_IP) {
            std::cout << "[P2P] Per-IP inbound limit reached (" << MAX_INBOUND_PER_IP
                      << "), rejecting " << client_address << std::endl;
            close_socket(client_socket);
            return;
        }
    }

    // SELF-LOOP PREVENTION: Only reject true self-connections
    // Note: For incoming connections, we cannot know the peer's listening port
    // (only their ephemeral source port). Self-connection prevention is primarily
    // handled in the outbound connect_to_peer() logic which checks both IP and port.
    //
    // For incoming: Only reject if from our configured external IP (true self-connection)
    if (!external_ip_.empty() && client_address == external_ip_) {
        std::cout << "[P2P] 🚫 Rejected incoming self-connection from external IP: " << client_address << std::endl;
        close_socket(client_socket);
        return;
    }
    // Localhost connections are allowed (enables multi-node local testing)

    // Set send timeout as safety measure
    set_socket_send_timeout(client_socket, SEND_TIMEOUT_SEC);

    // Extract actual source port from socket (fixes peer key collision bug)
    struct sockaddr_in peer_addr;
    socklen_t addr_len = sizeof(peer_addr);
    uint16_t source_port = 0;

    if (getpeername(client_socket, (struct sockaddr*)&peer_addr, &addr_len) == 0) {
        source_port = ntohs(peer_addr.sin_port);
    } else {
        std::cerr << "[P2P] Warning: Failed to get peer source port: " << strerror(errno) << std::endl;
        // Fallback: Use socket FD as unique identifier
        source_port = static_cast<uint16_t>(client_socket & 0xFFFF);
    }

    if (is_peer_banned(client_address, source_port) || is_peer_banned(client_address, 0)) {
        std::cout << "[P2P] Rejected banned inbound peer: "
                  << client_address << ":" << source_port << std::endl;
        close_socket(client_socket);
        return;
    }

    // Ring 3 Phase 4c: Create peer with shared_ptr for TS1 compliance
    auto peer = std::make_shared<PeerInfo>();
    peer->address = client_address;
    peer->port = source_port;  // Use actual source port for unique peer key
    peer->is_outbound = false;
    peer->is_connected = false;
    peer->socket_fd = client_socket;
    peer->connected_since = std::chrono::system_clock::now();
    peer->last_message_at = peer->connected_since;
    peer->last_seen = std::chrono::system_clock::now();
    // lifetime_state initialized to ALLOCATED by default

    std::cout << "Incoming connection from: " << client_address << ":" << source_port
              << " (send timeout: " << SEND_TIMEOUT_SEC << "s)" << std::endl;

    // Start peer handler thread
    peer_threads_.emplace_back(
        std::make_unique<std::thread>(&P2PManager::peer_handler_loop, this, peer)
    );
}

// Ring 3 Phase 4c: TS1-compliant peer_handler_loop
// ==================================================
// CRITICAL FIX: No raw pointers across lock boundaries
// Uses weak_ptr to prevent use-after-free when peer is erased by another thread
void P2PManager::peer_handler_loop(std::shared_ptr<PeerInfo> peer) {
    std::string peer_key = peer->to_string();

    // TS1 Invariant L1: Transition to RUNNING state
    peer->lifetime_state.store(PeerLifetimeState::RUNNING);

    // Add to connected peers (manager takes ownership via shared_ptr)
    std::weak_ptr<PeerInfo> peer_weak;  // TS1 Invariant O1: Worker holds weak_ptr only
    {
        std::lock_guard<std::mutex> lock(peers_mutex_);
        connected_peers_[peer_key] = peer;  // shared_ptr copied (not moved)
        peer_weak = peer;  // Create weak_ptr for thread-safe access
    }

    // Perform handshake
    // TS1 CRITICAL: Lock weak_ptr before access, proving peer is still alive
    auto peer_locked = peer_weak.lock();
    if (!peer_locked || !perform_handshake(peer_locked.get())) {
        std::cerr << "Handshake failed with " << peer_key << std::endl;
        // Clear connecting guard on handshake failure
        {
            std::lock_guard<std::mutex> lock(peers_mutex_);
            connecting_peers_.erase(peer_key);
        }
        cleanup_peer(peer_key);
        return;
    }

    // Handshake succeeded — clear connecting guard
    {
        std::lock_guard<std::mutex> lock(peers_mutex_);
        connecting_peers_.erase(peer_key);
    }

    // TS1 ASSERTION A1: Peer accessed only in valid states (RUNNING or STOPPING)
    auto state = peer_locked->lifetime_state.load();
    assert(state == PeerLifetimeState::RUNNING || state == PeerLifetimeState::STOPPING);
    (void)state;  // Suppress unused variable warning in release builds

    // Notify connection
    if (peer_connected_handler_) {
        peer_connected_handler_(peer_key);
    }

    // Message loop
    // TS1 CRITICAL: Lock weak_ptr on EACH iteration (prevents use-after-free)
    while (!shutdown_requested_) {
        // TS1 CHECK: Lock weak_ptr → proves peer not destroyed
        auto peer_locked = peer_weak.lock();
        if (!peer_locked || !peer_locked->is_connected) {
            // Peer was destroyed by another thread, or disconnected
            std::cout << "Connection lost with " << peer_key << std::endl;
            break;
        }

        // TS1 ASSERTION A2: Peer accessed only in valid states during message loop
        auto state = peer_locked->lifetime_state.load();
        assert(state == PeerLifetimeState::RUNNING || state == PeerLifetimeState::STOPPING);
        (void)state;

        // TS1 SAFE: peer_locked holds shared_ptr, peer cannot be destroyed
        int socket_fd = peer_locked->socket_fd;
        // Release lock before blocking I/O (prevent holding lock during recv)
        peer_locked.reset();

        auto message = receive_message(socket_fd);
        if (!message) {
            std::cout << "Connection lost with " << peer_key << std::endl;
            break;
        }

        // Update last_seen (lock weak_ptr again)
        peer_locked = peer_weak.lock();
        if (peer_locked) {
            peer_locked->last_message_at = std::chrono::system_clock::now();
            peer_locked->last_seen = std::chrono::system_clock::now();
        }

        process_message(peer_key, *message);
    }

    // TS1 Invariant L2: Transition to STOPPING state
    {
        auto peer_locked = peer_weak.lock();
        if (peer_locked) {
            peer_locked->lifetime_state.store(PeerLifetimeState::STOPPING);
        }
    }

    // Notify disconnection
    if (peer_disconnected_handler_) {
        peer_disconnected_handler_(peer_key);
    }

    // TS1 Note: cleanup_peer will enforce join-before-erase in Phase 4c
    cleanup_peer(peer_key);

    // TS1 Invariant L3: Thread exits, ready for JOINED state
    // (cleanup_peer will transition to JOINED after this thread is joined)
}

// P2P sync fix: Helper to parse version message and extract peer info
// Bitcoin version message format:
//   version:     4 bytes  (offset 0)
//   services:    8 bytes  (offset 4)
//   timestamp:   8 bytes  (offset 12)
//   addr_recv:  26 bytes  (offset 20)
//   addr_from:  26 bytes  (offset 46)
//   nonce:       8 bytes  (offset 72)
//   user_agent:  var_str  (offset 80) - varint length + string
//   start_height: 4 bytes (after user_agent)
//   relay:       1 byte   (optional, after start_height)
static void parse_version_payload(const std::vector<uint8_t>& payload, PeerInfo* peer) {
    if (!peer || payload.size() < 12) {
        return;
    }

    // Parse protocol version + advertised service flags.
    peer->protocol_version = ReadLE32(payload, 0);
    peer->service_flags = ReadLE64(payload, 4);

    // NAT traversal Phase 1A: capture the remote nonce so we can sign it
    // back in our `dineroid` message. Layout is fixed by the Bitcoin version
    // message: services(8) + timestamp(8) + addrRecv(26) + addrFrom(26) +
    // nonce(8), so nonce begins at offset 72.
    if (payload.size() >= 80) {
        peer->their_nonce = ReadLE64(payload, 72);
    }

    // User agent and start_height are optional for malformed/minimal payloads.
    if (payload.size() <= 80) {
        return;
    }

    // Parse user_agent at offset 80 (variable length varint + bytes).
    size_t offset = 80;
    uint64_t ua_len = 0;
    if (!ReadVarInt(payload, offset, &ua_len)) {
        return;
    }

    if (ua_len > payload.size() - offset) {
        return;
    }
    // Cap user-agent to 256 bytes to prevent memory abuse
    size_t safe_ua_len = std::min(static_cast<size_t>(ua_len), size_t{256});
    peer->user_agent.assign(payload.begin() + offset, payload.begin() + offset + safe_ua_len);
    offset += static_cast<size_t>(ua_len);

    // Parse start_height (4 bytes, little-endian) after user_agent.
    if (payload.size() >= offset + 4) {
        const uint32_t start_height = ReadLE32(payload, offset);
        peer->start_height = start_height;
        peer->best_known_height = start_height;
        peer->best_height = start_height;  // Transitional mirror for legacy callers.
    }
}

static uint32_t advertised_peer_height(const PeerInfo& peer) {
    return std::max(peer.best_known_height, peer.start_height);
}

static void seed_peer_sync_telemetry(PeerInfo* peer, uint32_t local_height) {
    if (!peer || local_height == 0) {
        return;
    }

    const uint32_t advertised_height = advertised_peer_height(*peer);
    if (advertised_height == 0) {
        return;
    }

    // Snapshot/bootstrap restores can legitimately advance our validated tip
    // before we exchange fresh headers or blocks with a peer. Clamp telemetry
    // to the lower of our local height and the peer's advertised best height.
    const uint32_t effective_height = std::min(local_height, advertised_height);
    peer->synced_headers = std::max(peer->synced_headers, effective_height);
    peer->synced_blocks = std::max(peer->synced_blocks, effective_height);
}

bool P2PManager::perform_handshake(PeerInfo* peer) {
    // P2P sync fix: Get actual chain height instead of hardcoded 0
    uint32_t our_height = height_provider_ ? height_provider_() : 0;

    // Get service flags (prune-aware: NODE_NETWORK_LIMITED if pruned/snapshot)
    uint64_t our_services = service_flags_provider_ ? service_flags_provider_() : 0;

    // NAT traversal Phase 1A: pre-generate the version nonce so it can be
    // tracked on PeerInfo and later compared against the signature in the
    // remote peer's `dineroid` message. SecureRandom guarantees 0 is never
    // returned for any practical run, but the explicit_nonce=0 sentinel
    // means "auto-gen", so re-roll defensively.
    peer->our_nonce = SecureRandom::GetUInt64();
    if (peer->our_nonce == 0) peer->our_nonce = 1;

    std::cout << "[Handshake DEBUG] Starting handshake with " << peer->to_string()
              << " (outbound=" << peer->is_outbound << ")" << std::endl;

    if (peer->is_outbound) {
        // Send version message with actual chain height and service flags
        auto version_msg = P2PMessage::create_version(protocol_version_, our_height, our_services,
                                                     user_agent_, peer->our_nonce);
        if (!send_message(peer->socket_fd, version_msg)) {
            return false;
        }

        // Wait for version response
        auto response = receive_message(peer->socket_fd);
        if (!response || response->command != "version") {
            return false;
        }

        // P2P sync fix: Parse peer's version info (including their chain height)
        parse_version_payload(response->payload, peer);
        seed_peer_sync_telemetry(peer, our_height);

        // NAT traversal Phase 1A: optional post-version, pre-verack identity
        // exchange. Both sides must advertise NODE_DINERO_V2 AND we must have
        // a local identity wired. Either side missing => skip silently and
        // continue legacy verack flow (backward compatibility).
        const bool both_v2 = (peer->service_flags & ServiceFlags::NODE_DINERO_V2) &&
                             (our_services & ServiceFlags::NODE_DINERO_V2);
        if (both_v2 && node_identity_) {
            ExchangeDineroId(peer);
        }
        // NAT traversal Phase 1A.2: BIP155 sendaddrv2 negotiation. Gated on
        // the same NODE_DINERO_V2 capability (no separate flag — addrv2 is
        // tied to v2 protocol). Identity is NOT a prereq here — addrv2
        // works fine without proven identity.
        if (both_v2) {
            ExchangeSendAddrV2(peer);
        }

        // Send verack
        auto verack_msg = P2PMessage::create_verack();
        if (!send_message(peer->socket_fd, verack_msg)) {
            return false;
        }

        // Wait for verack
        response = receive_message(peer->socket_fd);
        if (!response || response->command != "verack") {
            return false;
        }

    } else {
        // Wait for version message
        std::cout << "[Handshake DEBUG] Waiting for version message from inbound peer..." << std::endl;
        auto version_msg = receive_message(peer->socket_fd);
        if (!version_msg) {
            std::cout << "[Handshake DEBUG] receive_message returned nullptr (timeout/disconnect/parse error)" << std::endl;
            return false;
        }
        if (version_msg->command != "version") {
            std::cout << "[Handshake DEBUG] Expected 'version' but got '" << version_msg->command << "'" << std::endl;
            return false;
        }
        std::cout << "[Handshake DEBUG] Got version message, command='" << version_msg->command << "'" << std::endl;

        // P2P sync fix: Parse peer's version info (including their chain height)
        parse_version_payload(version_msg->payload, peer);
        seed_peer_sync_telemetry(peer, our_height);

        // Send version response with actual chain height and service flags
        auto response = P2PMessage::create_version(protocol_version_, our_height, our_services,
                                                  user_agent_, peer->our_nonce);
        if (!send_message(peer->socket_fd, response)) {
            return false;
        }

        // NAT traversal Phase 1A: see outbound branch above. Same gating
        // logic; this is symmetric so both sides reach this point with the
        // same view of NODE_DINERO_V2 + identity availability.
        const bool both_v2 = (peer->service_flags & ServiceFlags::NODE_DINERO_V2) &&
                             (our_services & ServiceFlags::NODE_DINERO_V2);
        if (both_v2 && node_identity_) {
            ExchangeDineroId(peer);
        }
        // NAT traversal Phase 1A.2: BIP155 sendaddrv2 negotiation. Gated on
        // the same NODE_DINERO_V2 capability (no separate flag — addrv2 is
        // tied to v2 protocol). Identity is NOT a prereq here — addrv2
        // works fine without proven identity.
        if (both_v2) {
            ExchangeSendAddrV2(peer);
        }

        // Wait for verack
        auto verack = receive_message(peer->socket_fd);
        if (!verack || verack->command != "verack") {
            return false;
        }

        // Send verack
        auto verack_response = P2PMessage::create_verack();
        if (!send_message(peer->socket_fd, verack_response)) {
            return false;
        }
    }

    P2PMessage sendcmpct_msg;
    sendcmpct_msg.command = "sendcmpct";
    sendcmpct_msg.payload = CreateSendCmpctPayload(true, 1);
    if (!send_message(peer->socket_fd, sendcmpct_msg)) {
        return false;
    }

    peer->is_connected = true;
    std::cout << "Handshake completed with " << peer->to_string()
              << " (our_height=" << our_height << ", peer_height=" << peer->best_height << ")" << std::endl;

    if (peer->is_outbound) {
        mark_peer_address_attempt(peer->address, peer->port, true);
    }

    // Request peer's known addresses for network discovery
    // Only on outbound connections to avoid addr storms
    if (peer->is_outbound) {
        auto getaddr_msg = P2PMessage::create_getaddr();
        send_message(peer->socket_fd, getaddr_msg);
        std::cout << "[P2P] Sent getaddr to " << peer->to_string() << std::endl;
        // Phase B (v8 peer discovery): record initial send so keepalive_loop
        // re-sends getaddr after the configured interval. Without this
        // timestamp, every keepalive tick would re-send and the network
        // would carry pointless addr-storm traffic.
        peer->last_getaddr_sent = std::chrono::steady_clock::now();
    }

    // Bitcoin-shaped self-advertisement: if this node knows a reachable public
    // address (manual externalip or successful UPnP), announce it after
    // handshake so servers and other peers can add it to addrman and relay it.
    const auto local_addresses = get_local_advertised_addresses();
    if (!local_addresses.empty() && send_addr_list_to_socket(peer->socket_fd, local_addresses)) {
        std::cout << "[P2P] Advertised " << local_addresses.size()
                  << " reachable address(es) to " << peer->to_string() << std::endl;
    }

    return true;
}

void P2PManager::disconnect_peer(const std::string& peer_address) {
    std::cout << "Disconnecting peer: " << peer_address << std::endl;
    cleanup_peer(peer_address);
    
    // Trigger disconnected handler
    if (peer_disconnected_handler_) {
        peer_disconnected_handler_(peer_address);
    }
}

bool P2PManager::ban_peer(const std::string& target, std::chrono::seconds duration) {
    const std::string normalized = TrimAscii(target);
    if (normalized.empty() || duration.count() <= 0) {
        return false;
    }

    const auto now = std::chrono::system_clock::now();
    const auto created = std::chrono::system_clock::to_time_t(now);
    const auto banned_until = std::chrono::system_clock::to_time_t(now + duration);

    {
        std::lock_guard<std::mutex> lock(bans_mutex_);
        banned_peers_[normalized] = BanEntry{
            normalized,
            static_cast<int64_t>(created),
            static_cast<int64_t>(banned_until),
        };
    }

    std::vector<std::string> peers_to_disconnect;
    {
        std::lock_guard<std::mutex> lock(peers_mutex_);
        for (const auto& [peer_key, peer] : connected_peers_) {
            if (peer && peer->is_connected &&
                BanTargetMatches(normalized, peer->address, peer->port)) {
                peers_to_disconnect.push_back(peer_key);
            }
        }
    }

    for (const auto& peer_key : peers_to_disconnect) {
        disconnect_peer(peer_key);
    }

    std::cout << "[P2P] Banned peer target " << normalized
              << " for " << duration.count() << " seconds"
              << " (disconnected " << peers_to_disconnect.size() << " peer(s))" << std::endl;
    return true;
}

bool P2PManager::unban_peer(const std::string& target) {
    const std::string normalized = TrimAscii(target);
    if (normalized.empty()) {
        return false;
    }

    std::lock_guard<std::mutex> lock(bans_mutex_);
    return banned_peers_.erase(normalized) > 0;
}

void P2PManager::clear_banned_peers() {
    std::lock_guard<std::mutex> lock(bans_mutex_);
    banned_peers_.clear();
}

std::vector<P2PManager::BanEntry> P2PManager::list_banned_peers() const {
    const auto now = static_cast<int64_t>(std::time(nullptr));
    std::vector<BanEntry> entries;

    std::lock_guard<std::mutex> lock(bans_mutex_);
    entries.reserve(banned_peers_.size());
    for (const auto& [target, entry] : banned_peers_) {
        if (entry.banned_until > now) {
            entries.push_back(entry);
        }
    }
    std::sort(entries.begin(), entries.end(), [](const BanEntry& a, const BanEntry& b) {
        return a.target < b.target;
    });
    return entries;
}

bool P2PManager::is_peer_banned(const std::string& address, uint16_t port) const {
    if (address.empty()) {
        return false;
    }

    const auto now = static_cast<int64_t>(std::time(nullptr));
    std::lock_guard<std::mutex> lock(bans_mutex_);
    for (const auto& [target, entry] : banned_peers_) {
        if (entry.banned_until <= now) {
            continue;
        }
        if (BanTargetMatches(target, address, port)) {
            return true;
        }
    }
    return false;
}

void P2PManager::set_network_active(bool active) {
    const bool previous = network_active_.exchange(active, std::memory_order_acq_rel);
    if (previous == active) {
        return;
    }

    if (!active) {
        std::vector<std::string> peers_to_disconnect;
        {
            std::lock_guard<std::mutex> lock(peers_mutex_);
            peers_to_disconnect.reserve(connected_peers_.size());
            for (const auto& [peer_address, peer] : connected_peers_) {
                if (peer->is_connected) {
                    peers_to_disconnect.push_back(peer_address);
                }
            }
        }
        for (const auto& peer_address : peers_to_disconnect) {
            disconnect_peer(peer_address);
        }
    }

    keepalive_cv_.notify_all();
    connection_manager_cv_.notify_all();
}

// Ring 3 Phase 4c: TS1-compliant cleanup_peer
// =============================================
// CRITICAL: This function is called FROM WITHIN peer_handler_loop,
// so it CANNOT join the thread (a thread can't join itself).
//
// TS1 Solution: cleanup_peer only marks peer as disconnected and closes socket.
// The actual erase-from-map happens in stop() AFTER all threads are joined.
void P2PManager::cleanup_peer(const std::string& peer_address) {
    int fd_to_clean = -1;
    {
        std::lock_guard<std::mutex> lock(peers_mutex_);
        connecting_peers_.erase(peer_address);  // Clear connecting guard
        auto it = connected_peers_.find(peer_address);
        if (it != connected_peers_.end()) {
            fd_to_clean = it->second->socket_fd;
            // Close socket to unblock any pending I/O
            close_socket(it->second->socket_fd);

            // Mark peer as disconnected (but keep in map until threads are joined)
            it->second->is_connected = false;

            // TS1 Note: We do NOT erase here because the peer thread may still
            // be running. The erase will happen in stop() after join.
        }
    }
    // Clean up per-socket send mutex (outside peers_mutex_ to avoid nesting)
    if (fd_to_clean >= 0) {
        erase_socket_send_mutex(fd_to_clean);
    }
}

std::vector<PeerInfo> P2PManager::get_connected_peers() const {
    const uint32_t local_height = height_provider_ ? height_provider_() : 0;

    std::lock_guard<std::mutex> lock(peers_mutex_);
    std::vector<PeerInfo> result;
    result.reserve(connected_peers_.size());

    for (const auto& pair : connected_peers_) {
        if (pair.second->is_connected) {
            // Ring 3 Phase 4c: Manual construction because lifetime_state is atomic (non-copyable/non-movable)
            // Note: We skip copying lifetime_state - it's an internal implementation detail
            PeerInfo info;
            info.address = pair.second->address;
            info.port = pair.second->port;
            info.user_agent = pair.second->user_agent;
            info.protocol_version = pair.second->protocol_version;
            info.service_flags = pair.second->service_flags;
            info.start_height = pair.second->start_height;
            info.best_known_height = pair.second->best_known_height;
            info.synced_headers = pair.second->synced_headers;
            info.best_height = pair.second->best_height;
            info.synced_blocks = pair.second->synced_blocks;
            info.compact_blocks_enabled = pair.second->compact_blocks_enabled;
            info.compact_blocks_announce = pair.second->compact_blocks_announce;
            info.compact_blocks_version = pair.second->compact_blocks_version;
            info.bytes_recv = pair.second->bytes_recv;
            info.bytes_sent = pair.second->bytes_sent;
            info.connected_since = pair.second->connected_since;
            info.last_message_at = pair.second->last_message_at;
            info.last_seen = pair.second->last_seen;
            info.last_ping_sent = pair.second->last_ping_sent;
            info.is_outbound = pair.second->is_outbound;
            info.is_connected = pair.second->is_connected;
            info.socket_fd = pair.second->socket_fd;
            info.last_seen_unix = pair.second->last_seen_unix;
            info.avg_latency_ms = pair.second->avg_latency_ms;
            // lifetime_state remains at default ALLOCATED (internal detail, not exposed in API)
            seed_peer_sync_telemetry(&info, local_height);

            result.emplace_back(std::move(info));
        }
    }
    return result;
}

size_t P2PManager::get_peer_count() const {
    std::lock_guard<std::mutex> lock(peers_mutex_);
    size_t connected = 0;
    for (const auto& pair : connected_peers_) {
        if (pair.second->is_connected) {
            ++connected;
        }
    }
    return connected;
}

void P2PManager::update_peer_height(const std::string& peer_address, uint32_t height) {
    std::lock_guard<std::mutex> lock(peers_mutex_);
    auto it = connected_peers_.find(peer_address);
    if (it != connected_peers_.end()) {
        // best_known_height tracks the remote peer's advertised best height.
        // Never let local sync bookkeeping push it backwards.
        if (height > it->second->best_known_height) {
            it->second->best_known_height = height;
            it->second->best_height = height;
        }
    }
}

void P2PManager::update_peer_synced_headers(const std::string& peer_address, uint32_t height) {
    std::lock_guard<std::mutex> lock(peers_mutex_);
    auto it = connected_peers_.find(peer_address);
    if (it != connected_peers_.end()) {
        if (height > it->second->best_known_height) {
            it->second->best_known_height = height;
            it->second->best_height = height;
        }
        it->second->synced_headers = std::max(it->second->synced_headers, height);
    }
}

void P2PManager::update_peer_synced_blocks(const std::string& peer_address, uint32_t height) {
    std::lock_guard<std::mutex> lock(peers_mutex_);
    auto it = connected_peers_.find(peer_address);
    if (it != connected_peers_.end()) {
        if (height > it->second->best_known_height) {
            it->second->best_known_height = height;
            it->second->best_height = height;
        }
        it->second->synced_blocks = std::max(it->second->synced_blocks, height);
        it->second->synced_headers = std::max(it->second->synced_headers, height);
    }
}

PeerInfo* P2PManager::get_peer_info(const std::string& peer_address) {
    std::lock_guard<std::mutex> lock(peers_mutex_);
    auto it = connected_peers_.find(peer_address);
    if (it != connected_peers_.end()) {
        return it->second.get();
    }
    return nullptr;
}

bool P2PManager::peer_has_service_flags(const std::string& peer_address, uint64_t required_flags) const {
    std::lock_guard<std::mutex> lock(peers_mutex_);
    auto it = connected_peers_.find(peer_address);
    if (it == connected_peers_.end()) {
        return false;
    }
    return (it->second->service_flags & required_flags) == required_flags;
}

bool P2PManager::peer_prefers_compact_blocks(const std::string& peer_address) const {
    std::lock_guard<std::mutex> lock(peers_mutex_);
    auto it = connected_peers_.find(peer_address);
    if (it == connected_peers_.end()) {
        return false;
    }
    return it->second->compact_blocks_enabled &&
           it->second->compact_blocks_announce &&
           it->second->compact_blocks_version >= 1;
}

std::shared_ptr<std::mutex> P2PManager::get_socket_send_mutex(int socket_fd) {
    std::lock_guard<std::mutex> guard(socket_send_mutexes_guard_);
    auto it = socket_send_mutexes_.find(socket_fd);
    if (it != socket_send_mutexes_.end()) {
        return it->second;
    }
    auto mtx = std::make_shared<std::mutex>();
    socket_send_mutexes_[socket_fd] = mtx;
    return mtx;
}

void P2PManager::erase_socket_send_mutex(int socket_fd) {
    std::lock_guard<std::mutex> guard(socket_send_mutexes_guard_);
    socket_send_mutexes_.erase(socket_fd);
}

bool P2PManager::send_message(int socket_fd, const P2PMessage& message) {
    auto smtx = get_socket_send_mutex(socket_fd);
    std::lock_guard<std::mutex> send_lock(*smtx);
    auto data = message.serialize();
    size_t total_sent = 0;

    while (total_sent < data.size()) {
        int sent = send(socket_fd,
                        reinterpret_cast<const char*>(data.data() + total_sent),
                        data.size() - total_sent,
                        MSG_NOSIGNAL);

        if (sent < 0) {
            if (errno == EINTR) {
                continue;
            }

            if (errno == EWOULDBLOCK || errno == EAGAIN) {
                // Socket may be non-blocking (e.g., async outbox path). Wait briefly
                // for writability and continue instead of failing the whole message.
                fd_set write_fds;
                FD_ZERO(&write_fds);
                FD_SET(socket_fd, &write_fds);

                struct timeval timeout;
                timeout.tv_sec = SEND_TIMEOUT_SEC;
                timeout.tv_usec = 0;

                int ready = select(socket_fd + 1, nullptr, &write_fds, nullptr, &timeout);
                if (ready > 0) {
                    continue;
                }
            }

            return false;
        }

        if (sent == 0) {
            return false;
        }

        total_sent += sent;
    }

    // Update bytes_sent for this peer
    std::lock_guard<std::mutex> lock(peers_mutex_);
    for (auto& pair : connected_peers_) {
        if (pair.second->socket_fd == socket_fd) {
            pair.second->bytes_sent += data.size();
            break;
        }
    }
    
    return true;
}

std::unique_ptr<P2PMessage> P2PManager::receive_message(int socket_fd) {
    // Ring 3 Phase 4e: TS3 Fix - Use select() with timeout to make recv() interruptible
    // This fixes TS3.1, TS3.3, and TS3.4 violations in peer_handler_loop

    // Read header first (24 bytes)
    std::vector<uint8_t> header(24);
    size_t total_received = 0;
    size_t total_bytes_received = 0;  // Track total for peer stats

    while (total_received < 24) {
        // TS3 FIX: Check for data availability with timeout before blocking recv
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(socket_fd, &read_fds);

        struct timeval timeout;
        timeout.tv_sec = 1;   // 1 second timeout (TS3.3 compliance)
        timeout.tv_usec = 0;

        int activity = select(socket_fd + 1, &read_fds, nullptr, nullptr, &timeout);

        if (activity < 0) {
            // Select error
            return nullptr;
        }

        if (activity == 0) {
            // Timeout - check shutdown signal (TS3.1 compliance)
            if (shutdown_requested_) {
                return nullptr;
            }
            continue;  // Retry select
        }

        // Data available, safe to recv without indefinite blocking
        int received = recv(socket_fd,
                           reinterpret_cast<char*>(header.data() + total_received),
                           24 - total_received, 0);

        if (received <= 0) {
            return nullptr;
        }

        total_received += received;
    }

    // Extract payload length
    uint32_t payload_len = 0;
    for (int i = 0; i < 4; i++) {
        payload_len |= (static_cast<uint32_t>(header[16 + i]) << (i * 8));
    }

    // Sanity check. The previous cap here was a hardcoded 1 MB, which is
    // correct for a pre-SegWit chain but *too small* for Dinero: a full
    // witness block can reach MAX_BLOCK_WEIGHT = 4 MB (see
    // include/consensus/limits.h), and with ring-covenant ZK proofs
    // (~6.9 KB measured per input, consensus cap 25 per block) plus CT
    // rangeproofs, real blocks on this chain would be silently dropped.
    //
    // 4 MB is the right value, derived from three aligned sources:
    //   1. consensus MAX_BLOCK_WEIGHT = 4 MB (include/consensus/limits.h),
    //      the hard ceiling for any legitimate serialized block.
    //   2. include/daemon/p2p_message.h: MAX_MESSAGE_SIZE = 4 MB, the
    //      canonical constant used by peer_connection.cpp in this same stack.
    //   3. primitives/block.cpp:312 enforces the same cap at deserialize.
    //
    // ZK headroom check: 25 ring-covenant inputs × ~6.9 KB proof ≈ 172 KB
    // measured; 25 × 50 KB (pessimistic fee-reserve budget) = 1.25 MB. Even
    // the pessimistic case leaves 2.75 MB for CT rangeproofs + base tx data,
    // so 4 MB has ~20× headroom on today's measured proof sizes and ~3× on
    // the pessimistic budget.
    //
    // If MAX_RING_COVENANT_INPUTS_PER_BLOCK is ever raised, or if per-input
    // proof size grows beyond ~120 KB, revisit this cap *together* with
    // MAX_BLOCK_WEIGHT — they need to stay consistent.
    //
    // Not using dinero::MAX_MESSAGE_SIZE from daemon/p2p_message.h directly
    // because including that header here broke the transitive
    // <netdb.h>/<fcntl.h> include chain. The value below must stay in sync
    // with include/daemon/p2p_message.h.
    static constexpr size_t kMaxP2PMessageSize = 4ULL * 1024 * 1024;  // 4 MB
    if (payload_len > kMaxP2PMessageSize) {
        return nullptr;
    }

    // Read payload
    std::vector<uint8_t> full_message = header;
    if (payload_len > 0) {
        std::vector<uint8_t> payload(payload_len);
        total_received = 0;
        auto payload_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);

        while (total_received < payload_len) {
            // TS3 FIX: Check for data availability with timeout (same pattern as header)
            fd_set read_fds;
            FD_ZERO(&read_fds);
            FD_SET(socket_fd, &read_fds);

            struct timeval timeout;
            timeout.tv_sec = 1;
            timeout.tv_usec = 0;

            int activity = select(socket_fd + 1, &read_fds, nullptr, nullptr, &timeout);

            if (activity < 0) {
                return nullptr;
            }

            if (activity == 0) {
                // Timeout - check shutdown signal
                if (shutdown_requested_) {
                    return nullptr;
                }
                if (std::chrono::steady_clock::now() >= payload_deadline) {
                    std::cerr << "[P2P] Payload receive timeout (" << payload_len
                              << " bytes, received " << total_received
                              << ") on socket " << socket_fd << std::endl;
                    return nullptr;
                }
                continue;  // Retry select
            }

            int received = recv(socket_fd,
                               reinterpret_cast<char*>(payload.data() + total_received),
                               payload_len - total_received, 0);

            if (received <= 0) {
                return nullptr;
            }

            total_received += received;
            // Any progress extends the deadline.
            payload_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        }

        full_message.insert(full_message.end(), payload.begin(), payload.end());
    }

    // Update bytes_recv for this peer
    total_bytes_received = full_message.size();
    {
        std::lock_guard<std::mutex> lock(peers_mutex_);
        for (auto& pair : connected_peers_) {
            if (pair.second->socket_fd == socket_fd) {
                pair.second->bytes_recv += total_bytes_received;
                break;
            }
        }
    }  // Release peers_mutex_ BEFORE deserialize to avoid blocking all peer threads

    auto msg = P2PMessage::deserialize(full_message);
    if (!msg) {
        std::cout << "[receive_message DEBUG] deserialize returned nullptr for "
                  << full_message.size() << " bytes" << std::endl;
        // Print first 24 bytes (header) for debugging
        std::cout << "[receive_message DEBUG] Header bytes: ";
        for (size_t i = 0; i < std::min<size_t>(24, full_message.size()); i++) {
            printf("%02x ", full_message[i]);
        }
        std::cout << std::endl;
    }
    return msg;
}

void P2PManager::process_message(const std::string& peer_address, const P2PMessage& message) {
    // Handle built-in messages
    if (message.command == "ping") {
        handle_ping(peer_address, message);
    } else if (message.command == "pong") {
        handle_pong(peer_address, message);
    } else if (message.command == "sendcmpct") {
        handle_sendcmpct(peer_address, message);
    } else if (message.command == "getaddr") {
        handle_getaddr(peer_address, message);
    } else if (message.command == "addr") {
        handle_addr(peer_address, message);
    } else if (message.command == "sendaddrv2") {
        // BIP155 allows late `sendaddrv2` too; pick up the flag whenever it
        // arrives. (Handshake path also sets it; this just keeps state in
        // sync when the message comes outside the negotiation window.)
        std::lock_guard<std::mutex> lock(peers_mutex_);
        auto it = connected_peers_.find(peer_address);
        if (it != connected_peers_.end() && it->second) {
            it->second->peer_wants_addrv2 = true;
        }
    } else if (message.command == "addrv2") {
        handle_addrv2(peer_address, message);
    } else if (message.command == "relayreg") {
        // NAT Phase C3 slice 2: real handler — validates sig, registers.
        handle_relay_register(peer_address, message);
    } else if (message.command == "relaycon" ||
               message.command == "relayack" || message.command == "relaydat" ||
               message.command == "relaypng" || message.command == "relayhnt") {
        // NAT Phase C3 slices 3+: still log-only. The splice (slice 3) and
        // client-side outbound (slice 4) land in follow-up commits; until
        // then we accept the messages cleanly so any forward-deployed
        // implementation can probe whether a peer speaks the protocol.
        std::cout << "[P2P] relay-protocol message '" << message.command
                  << "' (" << message.payload.size() << " bytes) from "
                  << peer_address << " — handler is a Phase C3 follow-up; dropped"
                  << std::endl;
    } else {
        // Forward to application handler
        if (message_handler_) {
            message_handler_(peer_address, message);
        }
    }
}

void P2PManager::handle_ping(const std::string& peer_address, const P2PMessage& message) {
    // Extract nonce and send pong
    if (message.payload.size() >= 8) {
        uint64_t nonce = 0;
        for (int i = 0; i < 8; i++) {
            nonce |= (static_cast<uint64_t>(message.payload[i]) << (i * 8));
        }
        
        auto pong = P2PMessage::create_pong(nonce);
        send_to_peer(peer_address, pong);
    }
}

void P2PManager::handle_pong(const std::string& peer_address, const P2PMessage& message) {
    (void)peer_address;
    (void)message;
    // Keepalive acknowledgement handled implicitly by connection liveness.
}

void P2PManager::handle_sendcmpct(const std::string& peer_address, const P2PMessage& message) {
    if (message.payload.size() < 9) {
        std::cerr << "[P2P] Invalid sendcmpct from " << peer_address
                  << " (payload=" << message.payload.size() << " bytes)" << std::endl;
        return;
    }

    const bool announce = message.payload[0] != 0;
    const uint64_t version = ReadLE64(message.payload, 1);

    std::lock_guard<std::mutex> lock(peers_mutex_);
    auto it = connected_peers_.find(peer_address);
    if (it == connected_peers_.end()) {
        return;
    }

    it->second->compact_blocks_enabled = true;
    it->second->compact_blocks_announce = announce;
    it->second->compact_blocks_version = version;

    std::cout << "[P2P] Received sendcmpct from " << peer_address
              << " (announce=" << (announce ? "1" : "0")
              << ", version=" << version << ")" << std::endl;
}

void P2PManager::handle_getaddr(const std::string& peer_address, const P2PMessage& message) {
    (void)message;

    // Reply from the address book, not merely the current socket list. This is
    // what lets a reachable community node propagate beyond the bootstrap
    // servers: one node learns it, addrman stores it, getaddr shares it.
    const auto relayable = collect_advertisable_addresses(1000);
    if (!relayable.empty()) {
        std::vector<PeerInfo> peer_infos;
        peer_infos.reserve(relayable.size());
        for (const auto& [address, port] : relayable) {
            peer_infos.push_back(PeerInfoForAddress(address, port));
        }
        auto addr_msg = P2PMessage::create_addr(peer_infos);
        send_to_peer(peer_address, addr_msg);
    }
}

void P2PManager::handle_addr(const std::string& peer_address, const P2PMessage& message) {
    const auto& payload = message.payload;
    if (payload.empty()) return;

    uint8_t count = payload[0];
    size_t offset = 1;
    int added = 0;

    // Phase B (v8 peer discovery): collect the validated addresses so we
    // can relay them to other peers after the loop. Without relay, addrs
    // learned by one node stay local — community-hosted nodes can't
    // propagate through the network even when they're reachable.
    std::vector<std::pair<std::string, uint16_t>> new_for_relay;

    for (uint8_t i = 0; i < count && offset < payload.size(); i++) {
        // Read address string length
        if (offset >= payload.size()) break;
        uint8_t addr_len = payload[offset++];
        if (offset + addr_len + 2 > payload.size()) break;

        // Read address string
        std::string addr(payload.begin() + offset, payload.begin() + offset + addr_len);
        offset += addr_len;

        // Read port (2 bytes, little-endian)
        uint16_t port = payload[offset] | (payload[offset + 1] << 8);
        offset += 2;

        // Validate: skip empty, localhost, self, or ephemeral ports
        if (addr.empty() || port == 0) continue;
        if (addr == "127.0.0.1" || addr == "0.0.0.0" || addr == "::1") continue;
        if (!external_ip_.empty() && addr == external_ip_ && port == listen_port_) continue;
        // Reject addresses with non-standard ports — these are almost certainly
        // ephemeral source ports from inbound connections, not real listening ports.
        if (port != listen_port_) continue;

        // Add as a connect candidate and feed addrman. Inbound socket source
        // ports are still rejected above; only explicit listening addresses
        // survive into the address book.
        if (remember_peer_address(addr, port, peer_address)) {
            added++;
            new_for_relay.emplace_back(addr, port);
        }
    }

    if (added > 0) {
        std::cout << "[P2P] Received " << added << " new peer address(es) from "
                  << peer_address << std::endl;
        // Persist discovered peers so they survive restart
        if (!peers_file_path_.empty()) {
            save_peers_with_seeds(peers_file_path_);
        }

        // Phase B (v8 peer discovery): relay to 2 random outbound peers
        // (excluding the sender). Forwarding is what makes the network
        // grow — without it, a new peer's address only reaches its
        // immediate neighbor. Bitcoin Core relays to 2 randomly-selected
        // peers per inbound addr message; same shape here.
        relay_addresses_to_peers(peer_address, new_for_relay);
    }
}

void P2PManager::relay_addresses_to_peers(
    const std::string& source_peer,
    const std::vector<std::pair<std::string, uint16_t>>& addresses) {
    if (addresses.empty()) return;

    // Collect candidate outbound peers (excluding the sender).
    std::vector<std::string> candidates;
    {
        std::lock_guard<std::mutex> lock(peers_mutex_);
        for (const auto& pair : connected_peers_) {
            const auto& peer = pair.second;
            if (!peer->is_connected) continue;
            if (!peer->is_outbound) continue;          // only outbound (known listening port)
            if (pair.first == source_peer) continue;   // don't echo back to sender
            candidates.push_back(pair.first);
        }
    }
    if (candidates.empty()) return;

    // Shuffle and pick the first 2 (Bitcoin Core's relay fanout). Using
    // mt19937 seeded from steady_clock — sufficient for fanout selection;
    // not security-critical (the addresses themselves are public).
    std::mt19937 rng{static_cast<uint32_t>(
        std::chrono::steady_clock::now().time_since_epoch().count())};
    std::shuffle(candidates.begin(), candidates.end(), rng);
    const size_t fanout = std::min<size_t>(2, candidates.size());

    // Build a minimal PeerInfo vector for create_addr. We only need
    // address + port — create_addr extracts those two fields.
    std::vector<PeerInfo> as_peer_infos;
    as_peer_infos.reserve(addresses.size());
    for (const auto& [addr, port] : addresses) {
        as_peer_infos.push_back(PeerInfoForAddress(addr, port));
    }
    auto addr_msg = P2PMessage::create_addr(as_peer_infos);

    for (size_t i = 0; i < fanout; ++i) {
        send_to_peer(candidates[i], addr_msg);
    }
    std::cout << "[P2P] Relayed " << addresses.size()
              << " address(es) to " << fanout << " peer(s)" << std::endl;
}

bool P2PManager::send_to_peer(const std::string& peer_address, const P2PMessage& message) {
    if (!network_active_.load(std::memory_order_acquire)) {
        return false;
    }

    // Get socket FD while holding lock, then release before sending
    int socket_fd = -1;
    {
        std::lock_guard<std::mutex> lock(peers_mutex_);
        auto it = connected_peers_.find(peer_address);
        if (it != connected_peers_.end() && it->second->is_connected) {
            socket_fd = it->second->socket_fd;
        }
    }

    // Send outside peers_mutex_ to avoid blocking other threads.
    // send_message() acquires per-socket mutex internally to prevent
    // interleaved writes without blocking sends to OTHER peers.
    if (socket_fd >= 0) {
        return send_message(socket_fd, message);
    }
    return false;
}

void P2PManager::broadcast_message(const P2PMessage& message) {
    // DEPRECATED: Use broadcast_message_async() instead to avoid blocking
    std::cout << "[WARN] Using deprecated synchronous broadcast_message(). Use broadcast_message_async() instead." << std::endl;
    broadcast_message_async(message);
}

void P2PManager::broadcast_message_async(const P2PMessage& message) {
    if (!network_active_.load(std::memory_order_acquire)) {
        return;
    }

    // Serialize once for all peers
    auto data = std::make_shared<std::vector<uint8_t>>(message.serialize());
    
    std::lock_guard<std::mutex> lock(outbox_mutex_);
    
    // Check queue size limit
    if (outbox_queue_.size() >= MAX_OUTBOX_SIZE) {
        // WARNING: This drops block announcements silently — can cause sync stalls.
        // Log to both stderr and stdout so it's visible in log files.
        std::cerr << "[P2P] ERROR: Outbox queue full (" << MAX_OUTBOX_SIZE
                  << ", current=" << outbox_queue_.size() << "), dropping broadcast!" << std::endl;
        std::cout << "[P2P] ERROR: Outbox queue full (" << MAX_OUTBOX_SIZE
                  << ", current=" << outbox_queue_.size() << "), dropping broadcast!" << std::endl;
        return;
    }
    
    // Queue message for each connected peer
    std::lock_guard<std::mutex> peers_lock(peers_mutex_);
    for (const auto& pair : connected_peers_) {
        if (pair.second->is_connected) {
            OutMsg msg;
            msg.peer_id = pair.first;
            msg.data = data;
            msg.offset = 0;
            msg.tries = 0;
            msg.queued_at = std::chrono::steady_clock::now();
            outbox_queue_.push_back(std::move(msg));
        }
    }
    
    // Wake up outbox thread
    outbox_cv_.notify_one();
}

int P2PManager::create_listen_socket() {
    int socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (socket_fd < 0) {
        std::cerr << "socket() failed: " << strerror(errno) << std::endl;
        return -1;
    }
    
    // Enable socket reuse
    int opt = 1;
    if (setsockopt(socket_fd, SOL_SOCKET, SO_REUSEADDR, (char*)&opt, sizeof(opt)) < 0) {
        std::cerr << "setsockopt(SO_REUSEADDR) failed: " << strerror(errno) << std::endl;
    }

#ifdef SO_REUSEPORT
    // Enable port reuse on systems that support it
    if (setsockopt(socket_fd, SOL_SOCKET, SO_REUSEPORT, (char*)&opt, sizeof(opt)) < 0) {
        // Not critical - some systems don't support SO_REUSEPORT
    }
#endif

    // ✅ TCP KEEPALIVE: Keep sockets alive through NAT/firewalls
    int keepalive = 1;
    if (setsockopt(socket_fd, SOL_SOCKET, SO_KEEPALIVE, (char*)&keepalive, sizeof(keepalive)) < 0) {
        std::cerr << "setsockopt(SO_KEEPALIVE) failed: " << strerror(errno) << std::endl;
    }
#ifdef __linux__
    // Linux-specific: Send keepalive probes after 60s idle, every 30s, 3 probes max
    int keepidle = 60;
    int keepintvl = 30;
    int keepcnt = 3;
    setsockopt(socket_fd, IPPROTO_TCP, TCP_KEEPIDLE, &keepidle, sizeof(keepidle));
    setsockopt(socket_fd, IPPROTO_TCP, TCP_KEEPINTVL, &keepintvl, sizeof(keepintvl));
    setsockopt(socket_fd, IPPROTO_TCP, TCP_KEEPCNT, &keepcnt, sizeof(keepcnt));
#endif
    
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(listen_port_);
    
    // Try to bind to requested port
    if (bind(socket_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        std::cerr << "bind() failed on port " << listen_port_ << ": " << strerror(errno) << std::endl;
        
        // If requested port is not 0, try ephemeral port as fallback
        if (listen_port_ != 0) {
            std::cout << "Falling back to ephemeral port..." << std::endl;
            server_addr.sin_port = htons(0); // Let OS choose
            
            if (bind(socket_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
                std::cerr << "bind() failed for ephemeral port: " << strerror(errno) << std::endl;
                close_socket(socket_fd);
                return -1;
            }
        } else {
            close_socket(socket_fd);
            return -1;
        }
    }
    
    // Get the actual bound port (important for ephemeral ports)
    socklen_t addr_len = sizeof(server_addr);
    if (getsockname(socket_fd, (struct sockaddr*)&server_addr, &addr_len) == 0) {
        uint16_t actual_port = ntohs(server_addr.sin_port);
        if (actual_port != listen_port_) {
            std::cout << "P2P server bound to port " << actual_port << " (requested: " << listen_port_ << ")" << std::endl;
            listen_port_ = actual_port; // Update our stored port
        }
    }
    
    if (listen(socket_fd, 128) < 0) { // Increased backlog
        std::cerr << "listen() failed: " << strerror(errno) << std::endl;
        close_socket(socket_fd);
        return -1;
    }

    // Signal socket is ready (deterministic, no races)
    socket_listening_.store(true, std::memory_order_release);
    ready_cv_.notify_all();

    std::cout << "P2P server successfully listening on *:" << listen_port_ << std::endl;
    return socket_fd;
}

int P2PManager::create_client_socket(const std::string& address, uint16_t port) {
    if (IsOnionAddress(address)) {
        std::string proxy_host;
        uint16_t proxy_port = 0;
        {
            std::lock_guard<std::mutex> lock(peers_mutex_);
            proxy_host = onion_proxy_host_;
            proxy_port = onion_proxy_port_;
        }
        if (proxy_host.empty() || proxy_port == 0) {
            std::cerr << "[P2P] Refusing to dial onion peer without SOCKS5 proxy: "
                      << address << ":" << port << std::endl;
            return -1;
        }
        return create_socks5_client_socket(proxy_host, proxy_port, address, port);
    }

    int socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (socket_fd < 0) {
        return -1;
    }

    // ✅ TCP KEEPALIVE: Keep sockets alive through NAT/firewalls
    int keepalive = 1;
    if (setsockopt(socket_fd, SOL_SOCKET, SO_KEEPALIVE, (char*)&keepalive, sizeof(keepalive)) < 0) {
        std::cerr << "setsockopt(SO_KEEPALIVE) failed: " << strerror(errno) << std::endl;
    }
#ifdef __linux__
    // Linux-specific: Send keepalive probes after 60s idle, every 30s, 3 probes max
    int keepidle = 60;
    int keepintvl = 30;
    int keepcnt = 3;
    setsockopt(socket_fd, IPPROTO_TCP, TCP_KEEPIDLE, &keepidle, sizeof(keepidle));
    setsockopt(socket_fd, IPPROTO_TCP, TCP_KEEPINTVL, &keepintvl, sizeof(keepintvl));
    setsockopt(socket_fd, IPPROTO_TCP, TCP_KEEPCNT, &keepcnt, sizeof(keepcnt));
#endif

    // Set socket to non-blocking for timeout control
    #ifdef _WIN32
        unsigned long mode = 1;
        ioctlsocket(socket_fd, FIONBIO, &mode);
    #else
        int flags = fcntl(socket_fd, F_GETFL, 0);
        fcntl(socket_fd, F_SETFL, flags | O_NONBLOCK);
    #endif
    
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    
    if (inet_pton(AF_INET, address.c_str(), &server_addr.sin_addr) <= 0) {
        // Not a raw IP address — try DNS resolution (supports seed1.dinero-coin.com etc.)
        struct addrinfo hints{}, *result = nullptr;
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        int err = getaddrinfo(address.c_str(), nullptr, &hints, &result);
        if (err != 0 || !result) {
            std::cerr << "[P2P] DNS resolution failed for " << address << ": "
                      << gai_strerror(err) << std::endl;
            close_socket(socket_fd);
            return -1;
        }
        server_addr.sin_addr = ((struct sockaddr_in*)result->ai_addr)->sin_addr;
        freeaddrinfo(result);
        std::cout << "[P2P] Resolved " << address << " -> "
                  << inet_ntoa(server_addr.sin_addr) << std::endl;
    }
    
    // Non-blocking connect
    int result = connect(socket_fd, (struct sockaddr*)&server_addr, sizeof(server_addr));
    if (result < 0) {
        #ifdef _WIN32
            if (WSAGetLastError() != WSAEWOULDBLOCK) {
                close_socket(socket_fd);
                return -1;
            }
        #else
            if (errno != EINPROGRESS) {
                close_socket(socket_fd);
                return -1;
            }
        #endif
        
        // Wait for connection with 5 second timeout
        fd_set write_fds;
        FD_ZERO(&write_fds);
        FD_SET(socket_fd, &write_fds);
        
        struct timeval timeout;
        timeout.tv_sec = 5;
        timeout.tv_usec = 0;
        
        result = select(socket_fd + 1, NULL, &write_fds, NULL, &timeout);
        if (result <= 0) {
            std::cerr << "Connection timeout to " << address << ":" << port << std::endl;
            close_socket(socket_fd);
            return -1;
        }
        
        // Check if connection succeeded
        int error = 0;
        socklen_t len = sizeof(error);
        if (getsockopt(socket_fd, SOL_SOCKET, SO_ERROR, (char*)&error, &len) < 0 || error != 0) {
            close_socket(socket_fd);
            return -1;
        }
    }
    
    // Set socket back to blocking mode
    #ifdef _WIN32
        unsigned long blocking_mode = 0;
        ioctlsocket(socket_fd, FIONBIO, &blocking_mode);
    #else
        flags = fcntl(socket_fd, F_GETFL, 0);
        fcntl(socket_fd, F_SETFL, flags & ~O_NONBLOCK);
    #endif
    
    return socket_fd;
}

int P2PManager::create_socks5_client_socket(const std::string& proxy_host,
                                            uint16_t proxy_port,
                                            const std::string& target_host,
                                            uint16_t target_port) {
    if (target_host.empty() || target_host.size() > 255 || target_port == 0) {
        return -1;
    }
    if (IsOnionAddress(proxy_host)) {
        std::cerr << "[P2P] Onion proxy itself cannot be an onion address: "
                  << proxy_host << ":" << proxy_port << std::endl;
        return -1;
    }

    const int socket_fd = create_client_socket(proxy_host, proxy_port);
    if (socket_fd < 0) {
        std::cerr << "[P2P] SOCKS5 proxy connection failed: "
                  << proxy_host << ":" << proxy_port << std::endl;
        return -1;
    }

    const uint8_t greeting[] = {0x05, 0x01, 0x00};  // SOCKS5, one method, no auth.
    uint8_t greeting_reply[2] = {};
    if (!SendAll(socket_fd, greeting, sizeof(greeting)) ||
        !RecvAll(socket_fd, greeting_reply, sizeof(greeting_reply)) ||
        greeting_reply[0] != 0x05 || greeting_reply[1] != 0x00) {
        std::cerr << "[P2P] SOCKS5 proxy rejected no-auth handshake for "
                  << target_host << ":" << target_port << std::endl;
        close_socket(socket_fd);
        return -1;
    }

    std::vector<uint8_t> request;
    request.reserve(7 + target_host.size());
    request.push_back(0x05);  // version
    request.push_back(0x01);  // CONNECT
    request.push_back(0x00);  // reserved
    request.push_back(0x03);  // domain name; lets Tor resolve .onion internally.
    request.push_back(static_cast<uint8_t>(target_host.size()));
    request.insert(request.end(), target_host.begin(), target_host.end());
    request.push_back(static_cast<uint8_t>((target_port >> 8) & 0xff));
    request.push_back(static_cast<uint8_t>(target_port & 0xff));

    uint8_t reply_header[4] = {};
    if (!SendAll(socket_fd, request.data(), request.size()) ||
        !RecvAll(socket_fd, reply_header, sizeof(reply_header)) ||
        reply_header[0] != 0x05 || reply_header[1] != 0x00) {
        std::cerr << "[P2P] SOCKS5 CONNECT failed for "
                  << target_host << ":" << target_port;
        if (reply_header[0] == 0x05) {
            std::cerr << " (reply=" << static_cast<int>(reply_header[1]) << ")";
        }
        std::cerr << std::endl;
        close_socket(socket_fd);
        return -1;
    }

    size_t bind_len = 0;
    if (reply_header[3] == 0x01) {
        bind_len = 4;       // IPv4
    } else if (reply_header[3] == 0x03) {
        uint8_t domain_len = 0;
        if (!RecvAll(socket_fd, &domain_len, 1)) {
            close_socket(socket_fd);
            return -1;
        }
        bind_len = domain_len;
    } else if (reply_header[3] == 0x04) {
        bind_len = 16;      // IPv6
    } else {
        close_socket(socket_fd);
        return -1;
    }

    std::array<uint8_t, 256> discard {};
    if (bind_len > discard.size() ||
        !RecvAll(socket_fd, discard.data(), bind_len) ||
        !RecvAll(socket_fd, discard.data(), 2)) {
        close_socket(socket_fd);
        return -1;
    }

    std::cout << "[P2P] Connected to onion peer via SOCKS5 proxy: "
              << target_host << ":" << target_port << std::endl;
    return socket_fd;
}

void P2PManager::close_socket(int socket_fd) {
#ifdef _WIN32
    closesocket(socket_fd);
#else
    close(socket_fd);
#endif
}

void P2PManager::set_socket_nonblocking(int socket_fd) {
#ifdef _WIN32
    unsigned long mode = 1;
    ioctlsocket(socket_fd, FIONBIO, &mode);
#else
    int flags = fcntl(socket_fd, F_GETFL, 0);
    if (flags >= 0) {
        fcntl(socket_fd, F_SETFL, flags | O_NONBLOCK);
    }
#endif
}

void P2PManager::set_socket_send_timeout(int socket_fd, int seconds) {
#ifdef _WIN32
    DWORD timeout = seconds * 1000;
    setsockopt(socket_fd, SOL_SOCKET, SO_SNDTIMEO, (const char*)&timeout, sizeof(timeout));
#else
    struct timeval timeout;
    timeout.tv_sec = seconds;
    timeout.tv_usec = 0;
    setsockopt(socket_fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
#endif
}

void P2PManager::outbox_loop() {
    std::cout << "[P2P] Async outbox thread started" << std::endl;
    
    while (!shutdown_requested_) {
        OutMsg msg;
        
        // Wait for messages
        {
            std::unique_lock<std::mutex> lock(outbox_mutex_);
            outbox_cv_.wait_for(lock, std::chrono::milliseconds(100), 
                [this]{ return shutdown_requested_ || !outbox_queue_.empty(); });
            
            if (shutdown_requested_) break;
            
            if (outbox_queue_.empty()) continue;
            
            msg = std::move(outbox_queue_.front());
            outbox_queue_.pop_front();
        }
        
        // Find peer socket
        int socket_fd = -1;
        bool peer_connected = false;
        {
            std::lock_guard<std::mutex> lock(peers_mutex_);
            auto it = connected_peers_.find(msg.peer_id);
            if (it != connected_peers_.end() && it->second->is_connected) {
                socket_fd = it->second->socket_fd;
                peer_connected = true;
            }
        }
        
        if (!peer_connected || socket_fd < 0) {
            // Peer disconnected, drop message
            continue;
        }
        
        // Try non-blocking send
        int send_flags = MSG_NOSIGNAL;
#ifdef MSG_DONTWAIT
        send_flags |= MSG_DONTWAIT;
#endif
        ssize_t sent = 0;
        {
            auto smtx = get_socket_send_mutex(socket_fd);
            std::lock_guard<std::mutex> send_lock(*smtx);
            sent = ::send(socket_fd,
                          reinterpret_cast<const char*>(msg.data->data() + msg.offset),
                          msg.data->size() - msg.offset,
                          send_flags);
        }

        if (sent > 0) {
            msg.offset += static_cast<size_t>(sent);
            
            // Update peer stats
            {
                std::lock_guard<std::mutex> lock(peers_mutex_);
                auto it = connected_peers_.find(msg.peer_id);
                if (it != connected_peers_.end()) {
                    it->second->bytes_sent += sent;
                }
            }
            
            if (msg.offset < msg.data->size()) {
                // Partial write - requeue with priority
                std::lock_guard<std::mutex> lock(outbox_mutex_);
                outbox_queue_.push_front(std::move(msg));
            }
        } else if (sent < 0 && (errno == EWOULDBLOCK || errno == EAGAIN)) {
            // Socket buffer full - backpressure
            msg.tries++;
            
            if (msg.tries < MAX_SEND_TRIES) {
                // Exponential backoff
                std::this_thread::sleep_for(std::chrono::milliseconds(10 * msg.tries));
                
                std::lock_guard<std::mutex> lock(outbox_mutex_);
                outbox_queue_.push_back(std::move(msg));
            } else {
                // Peer is stalled - disconnect
                std::cerr << "[P2P] Peer " << msg.peer_id << " stalled on send (dropping after " 
                         << msg.tries << " tries)" << std::endl;
                cleanup_peer(msg.peer_id);
            }
        } else if (sent < 0 && errno == EINTR) {
            // Interrupted syscall - retry quickly without penalizing peer.
            std::lock_guard<std::mutex> lock(outbox_mutex_);
            outbox_queue_.push_front(std::move(msg));
        } else {
            // Send error - disconnect peer
            std::cerr << "[P2P] Send error to peer " << msg.peer_id << ": " << strerror(errno) << std::endl;
            cleanup_peer(msg.peer_id);
        }
    }

    std::cout << "[P2P] Async outbox thread stopped" << std::endl;
}

// ============================================================================
// PHASE C: Persistent Peer Database & Adaptive Keepalive
// ============================================================================

// Phase C (v8 peer discovery): file format constants.
// Header line lets future readers detect/migrate format changes
// without crashing on unknown trailing fields. Loaders skip any line
// starting with '#'.
namespace {
constexpr const char* PEERS_FILE_HEADER = "# DINERO_PEERS_V1";
constexpr size_t PEERS_FILE_MAX_ENTRIES = 5000;  // Bitcoin Core uses ~5000 (new+tried buckets)
}  // namespace

void P2PManager::write_peers_file_atomic(const std::string& peers_file_path,
                                          const std::string& content) {
    // Atomic-write pattern: write to peers.dat.tmp, then rename in place.
    // Rename is atomic at the filesystem layer on POSIX (rename(2)) and
    // semi-atomic on Windows (MoveFileEx with MOVEFILE_REPLACE_EXISTING).
    // Either way, mid-write crashes leave the prior peers.dat intact —
    // the half-written .tmp is collateral but never observable as
    // peers.dat.
    //
    // On POSIX we additionally fsync() the temp file before rename for
    // power-loss durability (the data is guaranteed on disk before the
    // rename commits). On Windows this fsync step is best-effort via
    // FlushFileBuffers and skipped here; atomicity at the FS layer is
    // sufficient for the integrity invariant.
    const std::string tmp_path = peers_file_path + ".tmp";

    {
        std::ofstream tmp(tmp_path, std::ios::binary | std::ios::trunc);
        if (!tmp.is_open()) {
            std::cerr << "[P2P] Failed to open " << tmp_path
                      << " for atomic peers save" << std::endl;
            return;
        }
        tmp.write(content.data(), static_cast<std::streamsize>(content.size()));
        if (!tmp) {
            std::cerr << "[P2P] write() failed for " << tmp_path << std::endl;
            tmp.close();
            std::error_code ec;
            std::filesystem::remove(tmp_path, ec);
            return;
        }
        tmp.flush();
        // ofstream destructor closes the fd.
    }

#ifndef _WIN32
    // POSIX best-effort fsync. Open the path read-only just to call
    // fsync on the fd; ofstream's underlying fd isn't reachable from
    // the std API. This second open is cheap (path is in dentry cache).
    int fd = ::open(tmp_path.c_str(), O_RDONLY);
    if (fd >= 0) {
        if (::fsync(fd) < 0) {
            std::cerr << "[P2P] fsync() warning for " << tmp_path << ": "
                      << strerror(errno) << std::endl;
        }
        ::close(fd);
    }
#endif

    std::error_code ec;
    std::filesystem::rename(tmp_path, peers_file_path, ec);
    if (ec) {
        std::cerr << "[P2P] rename() failed: " << tmp_path << " -> "
                  << peers_file_path << ": " << ec.message() << std::endl;
        std::filesystem::remove(tmp_path, ec);
    }
}

void P2PManager::load_peers(const std::string& peers_file_path) {
    peers_file_path_ = peers_file_path;

    std::ifstream file(peers_file_path);
    if (!file.is_open()) {
        std::cout << "[P2P] No peers.dat found - starting fresh" << std::endl;
        return;
    }

    std::string line;
    int loaded = 0;
    int skipped = 0;
    while (std::getline(file, line)) {
        // Skip blank lines and comment/header lines (the PEERS_FILE_HEADER
        // sentinel lives in this comment space — older builds without
        // header support silently skipped # lines too, so the format is
        // forward-compatible by construction).
        if (line.empty() || line[0] == '#') continue;

        std::istringstream iss(line);
        std::string addr;
        uint16_t port = 0;
        int64_t last_seen = 0;

        if (iss >> addr >> port >> last_seen) {
            // Skip stale entries with ephemeral ports (not real listening ports)
            if (port != listen_port_) {
                skipped++;
                continue;
            }
            add_seed_node(addr, port);
            loaded++;
        } else {
            skipped++;
        }
    }

    std::cout << "[P2P] Loaded " << loaded << " peers from " << peers_file_path
              << (skipped > 0 ? " (" + std::to_string(skipped) + " entries skipped)" : "")
              << std::endl;
}

void P2PManager::save_peers(const std::string& peers_file_path) {
    std::ostringstream buf;
    buf << PEERS_FILE_HEADER << "\n";

    size_t total = 0;
    {
        std::lock_guard<std::mutex> lock(peers_mutex_);
        for (const auto& pair : connected_peers_) {
            if (total >= PEERS_FILE_MAX_ENTRIES) break;
            const auto& peer = pair.second;
            buf << peer->address << " " << peer->port << " "
                << peer->last_seen_unix << "\n";
            ++total;
        }
    }

    write_peers_file_atomic(peers_file_path, buf.str());
    std::cout << "[P2P] Saved " << total << " peers to " << peers_file_path << std::endl;
}

void P2PManager::save_peers_with_seeds(const std::string& peers_file_path) {
    std::ostringstream buf;
    buf << PEERS_FILE_HEADER << "\n";

    auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::unordered_set<std::string> written;
    size_t total = 0;

    {
        std::lock_guard<std::mutex> lock(peers_mutex_);

        // Save only outbound peers (we know their real listening port).
        // Inbound peers have ephemeral source ports that can't be connected to.
        for (const auto& pair : connected_peers_) {
            if (total >= PEERS_FILE_MAX_ENTRIES) break;
            const auto& peer = pair.second;
            if (!peer->is_outbound) continue;
            std::string key = peer->address + ":" + std::to_string(peer->port);
            buf << peer->address << " " << peer->port << " "
                << (peer->last_seen_unix > 0 ? peer->last_seen_unix : now) << "\n";
            written.insert(key);
            ++total;
        }

        // Save seed_nodes_ that aren't already written (includes addr-discovered peers)
        // Resolve DNS names to IPs for dedup against already-written resolved-IP entries.
        for (const auto& seed : seed_nodes_) {
            if (total >= PEERS_FILE_MAX_ENTRIES) break;
            std::string ip = seed.first;
            struct sockaddr_in sa;
            if (inet_pton(AF_INET, seed.first.c_str(), &sa.sin_addr) <= 0) {
                struct addrinfo hints{}, *res = nullptr;
                hints.ai_family = AF_INET;
                hints.ai_socktype = SOCK_STREAM;
                if (getaddrinfo(seed.first.c_str(), nullptr, &hints, &res) == 0 && res) {
                    char buf2[INET_ADDRSTRLEN];
                    inet_ntop(AF_INET,
                              &((struct sockaddr_in*)res->ai_addr)->sin_addr,
                              buf2, sizeof(buf2));
                    ip = buf2;
                    freeaddrinfo(res);
                }
            }
            std::string key = ip + ":" + std::to_string(seed.second);
            if (written.count(key) == 0) {
                buf << ip << " " << seed.second << " " << now << "\n";
                written.insert(key);
                ++total;
            }
        }
    }

    write_peers_file_atomic(peers_file_path, buf.str());
    std::cout << "[P2P] Saved " << total << " peers (connected + discovered) to "
              << peers_file_path << std::endl;
}

void P2PManager::mark_peer_seen(const std::string& peer_address) {
    std::lock_guard<std::mutex> lock(peers_mutex_);
    auto it = connected_peers_.find(peer_address);
    if (it != connected_peers_.end()) {
        it->second->last_seen_unix = std::chrono::system_clock::to_time_t(
            std::chrono::system_clock::now());
    }
}

void P2PManager::keepalive_loop() {
    std::cout << "[P2P] Keepalive thread started (30s PING interval)" << std::endl;

    // Phase C (v8 peer discovery): periodic peers.dat save cadence.
    // Without periodic save, last_seen_unix updates accumulated during
    // an uptime are lost on ungraceful exit. 10 ticks × 30s = 5 minutes
    // is the same order as Bitcoin Core's DUMP_PEERS_INTERVAL (15 min);
    // shorter here because Dinero's peer churn is higher per fewer
    // operators in the early network.
    constexpr int save_every_n_ticks = 10;
    int ticks_since_save = 0;

    while (!shutdown_requested_) {
        // Ring 3 Phase 4e: TS3 Fix - Interruptible wait instead of sleep_for
        // Allows immediate wakeup on shutdown, fixing TS3.1 and TS3.4 violations
        {
            std::unique_lock<std::mutex> lock(keepalive_mutex_);
            keepalive_cv_.wait_for(lock, std::chrono::seconds(30),
                [this]{ return shutdown_requested_.load(); });
        }

        if (shutdown_requested_) break;
        if (!network_active_.load(std::memory_order_acquire)) {
            continue;
        }

        // Send PING to all connected peers
        std::vector<std::string> peer_addresses;
        {
            std::lock_guard<std::mutex> lock(peers_mutex_);
            for (const auto& pair : connected_peers_) {
                if (pair.second->is_connected) {
                    peer_addresses.push_back(pair.first);
                }
            }
        }

        for (const auto& peer_addr : peer_addresses) {
            uint64_t nonce = static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
            auto ping_msg = P2PMessage::create_ping(nonce);
            send_to_peer(peer_addr, ping_msg);

            // Update last_ping_sent timestamp
            {
                std::lock_guard<std::mutex> lock(peers_mutex_);
                auto it = connected_peers_.find(peer_addr);
                if (it != connected_peers_.end()) {
                    it->second->last_ping_sent = std::chrono::steady_clock::now();
                }
            }
        }

        // Phase B (v8 peer discovery): periodic getaddr cadence. Bitcoin
        // Core sends getaddr at handshake and re-requests every 24 hours
        // (see node/net_processing.cpp). Same shape here: refresh peer
        // knowledge so the addrman / seed_nodes_ store doesn't stagnate
        // at whatever the original handshake learned. Only sent to
        // outbound peers (inbound peers have ephemeral source ports that
        // are useless for relay; getaddr replies would be wasted).
        constexpr auto getaddr_interval = std::chrono::hours(24);
        const auto now = std::chrono::steady_clock::now();
        std::vector<std::string> due_for_getaddr;
        {
            std::lock_guard<std::mutex> lock(peers_mutex_);
            for (const auto& pair : connected_peers_) {
                const auto& peer = pair.second;
                if (!peer->is_connected) continue;
                if (!peer->is_outbound) continue;
                // last_getaddr_sent == time_point{} means we never sent
                // (peer connected before this code shipped, or initial
                // send at complete_handshake failed). Either way, send
                // now and start the clock.
                const auto since = now - peer->last_getaddr_sent;
                if (peer->last_getaddr_sent == std::chrono::steady_clock::time_point{} ||
                    since >= getaddr_interval) {
                    due_for_getaddr.push_back(pair.first);
                }
            }
        }
        if (!due_for_getaddr.empty()) {
            auto getaddr_msg = P2PMessage::create_getaddr();
            for (const auto& peer_addr : due_for_getaddr) {
                send_to_peer(peer_addr, getaddr_msg);
                std::lock_guard<std::mutex> lock(peers_mutex_);
                auto it = connected_peers_.find(peer_addr);
                if (it != connected_peers_.end()) {
                    it->second->last_getaddr_sent = now;
                }
            }
            std::cout << "[P2P] Sent periodic getaddr to "
                      << due_for_getaddr.size() << " peer(s)" << std::endl;
        }

        // Phase C (v8 peer discovery): periodic peers.dat save. Captures
        // last_seen_unix updates and any seed_nodes_ additions that
        // happened outside the handle_addr save trigger (e.g., DNS-seed
        // resolution, manual addnode RPC).
        if (++ticks_since_save >= save_every_n_ticks && !peers_file_path_.empty()) {
            save_peers_with_seeds(peers_file_path_);
            ticks_since_save = 0;
        }
    }

    std::cout << "[P2P] Keepalive thread stopped" << std::endl;
}
