// Copyright (c) 2026 The Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// STUN client (RFC 5389) — NAT traversal Phase C1.
//
// Discovers the daemon's public IP + port by sending BINDING REQUEST
// datagrams to a small set of STUN servers and parsing the
// XOR-MAPPED-ADDRESS attribute in their responses. Every server's answer
// in a round is kept (NAT classification needs them all); the first one
// is reported as public_addr.
//
// Sockets: by default the client binds its own ephemeral UDP socket. Built
// with a SendFn it owns no socket: requests go out through the caller's
// socket (the P2P port's QuicEndpoint) and the caller hands answers back via
// HandleDatagram, so the reflexive address is the one peers can dial.
//
// Why we DON'T just use Bitcoin Core's externalip flag or rely on
// peers to echo our source address: those work fine when you HAVE
// inbound. Behind a real NAT, the only ways to know your public
// address are (a) ask the router via UPnP/NAT-PMP (covered by
// port_mapper.cpp, fails on hostile routers) or (b) ask an external
// STUN server. (b) succeeds on any NAT that lets outbound UDP work.
//
// Server list: 3 DineroLabs STUN servers (primary, avoids leaking
// every Dinero node's IP to Google), 2 well-known public fallbacks.
// Magic cookie 0x2112A442 is verified on every response per RFC 5389
// §6 — skipping the check is a known spoofability bug class.
//
// Threading: callback fires on the underlying UdpSocket reader
// thread. The internal transaction-id → server map is mutex-guarded.
// Discover() is non-blocking; results arrive asynchronously.

#pragma once

#include "network/udp_socket.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace dinero::network {

struct StunResult {
    UdpAddr public_addr{};            // discovered public IP+port (empty on timeout)
    std::string server_endpoint;      // "stun.example.com:3478" — which server answered
    std::string error_message;        // populated when public_addr.empty()
    // Every answer this round, in arrival order: (server, reflexive address).
    std::vector<std::pair<UdpAddr, UdpAddr>> responses;
};

class StunClient {
public:
    using ResultCallback = std::function<void(const StunResult&)>;
    using SendFn = std::function<bool(const UdpAddr& dest, const uint8_t* data, size_t len)>;

    StunClient();
    // No socket of its own: send with `send`, receive via HandleDatagram().
    explicit StunClient(SendFn send);
    ~StunClient();

    StunClient(const StunClient&) = delete;
    StunClient& operator=(const StunClient&) = delete;

    // Launch a discovery round. Sends a BINDING REQUEST to each
    // configured STUN server; callback fires once, when every server has
    // answered or `timeout` passes, with all answers received (error set
    // only if there were none).
    //
    // Subsequent calls before completion are rejected (returns false).
    bool Discover(std::chrono::seconds timeout, ResultCallback cb);

    // Add a STUN server (e.g. "stun.l.google.com:19302" — host may be a
    // dotted IPv4 literal OR a hostname; hostnames are resolved via
    // getaddrinfo on first send). MUST be called before Discover().
    void AddServer(const std::string& host, uint16_t port);

    // Inject a pre-built server list. Replaces existing list. The
    // default list (5 servers) is wired by P2PService at init time.
    void SetServers(std::vector<std::pair<std::string, uint16_t>> servers);

    // True between Discover() launch and result callback firing.
    bool busy() const { return busy_.load(std::memory_order_acquire); }

    // A datagram the caller's socket received (SendFn mode); non-STUN or
    // unmatched packets are ignored.
    void HandleDatagram(const UdpAddr& src, const uint8_t* data, size_t len);

    // RFC 7983 demux: STUN's first byte is 0x00/0x01 and the magic cookie
    // sits at offset 4. QUIC always sets the 0x40 fixed bit, so a QUIC packet
    // never matches.
    static bool IsStunPacket(const uint8_t* data, size_t len) {
        return len >= 20 && data[0] <= 0x01 && data[4] == 0x21 && data[5] == 0x12 &&
               data[6] == 0xA4 && data[7] == 0x42;
    }
    // XOR-MAPPED-ADDRESS of a Binding success response; nullopt if `data` is
    // not one or is malformed. Does not check the transaction id.
    static std::optional<UdpAddr> ParseBindingResponse(const uint8_t* data, size_t len);

private:
    void OnDatagram(const UdpAddr& src, std::vector<uint8_t> data);
    void DispatchResult(const StunResult& result);
    void TimeoutWatcher(std::chrono::seconds timeout);

    UdpSocket socket_;
    SendFn send_;  // set: external socket, socket_ unused
    std::vector<std::pair<std::string, uint16_t>> servers_;

    mutable std::mutex state_mutex_;
    // Outstanding transactions, keyed on the 12-byte txn id from the
    // request. Value tracks which server we sent to so we can include
    // it in the StunResult.
    struct Outstanding {
        std::array<uint8_t, 12> txn_id;
        std::string server_endpoint;
        UdpAddr server_addr;
        bool fulfilled{false};
    };
    std::vector<Outstanding> outstanding_;
    std::vector<std::pair<UdpAddr, UdpAddr>> responses_;  // this round's answers
    std::string first_server_endpoint_;

    ResultCallback result_cb_;
    std::atomic<bool> busy_{false};
    std::chrono::steady_clock::time_point deadline_;
};

}  // namespace dinero::network
