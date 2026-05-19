// Copyright (c) 2026 The Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// Cross-platform UDP socket facility — NAT traversal Phase B1.
//
// First piece of the additive UDP transport stack. Currently does pure
// best-effort datagrams (no reliability, no encryption); QUIC reliable
// streams via ngtcp2 (Phase B2) ride on top of this, as does the STUN
// client (Phase C1). Existing TCP P2P is untouched.
//
// Why one facility for both IPv4 and IPv6: each address family needs
// its own socket (you can't bind a v4 socket to receive v6 datagrams
// portably), but the upstream callers — STUN, hole-punch, QUIC — want
// a single send/receive abstraction. UdpSocket binds both, demuxes on
// receive, dispatches on send via the destination's address family.
//
// Threading: a single internal reader thread does blocking select() on
// both sockets with a 1s timeout (matches the existing P2PManager
// listen-loop pattern at p2p_manager.cpp:1475). Receive callback is
// invoked on that thread; callers must keep the callback short and
// thread-safe. Sends are caller-thread; per-socket sendto is atomic.

#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace dinero::network {

// UDP address — family + 16-byte IP storage (first 4 bytes used for
// IPv4 in network order) + host-order port. Mirrors getaddrinfo/struct
// sockaddr_storage closely enough that internal conversions stay
// trivial without exposing platform sockaddr types in the public API.
struct UdpAddr {
    enum class Family : uint8_t { V4 = 1, V6 = 2 };

    Family family{Family::V4};
    std::array<uint8_t, 16> ip{};  // first 4 bytes for V4, all 16 for V6
    uint16_t port{0};

    static UdpAddr FromIPv4(const uint8_t ip4[4], uint16_t port);
    static UdpAddr FromIPv6(const uint8_t ip6[16], uint16_t port);

    // "1.2.3.4:8080" or "[::1]:8080". Empty on malformed.
    std::string to_string() const;
    bool empty() const { return port == 0; }
};

class UdpSocket {
public:
    // Called from the reader thread for every received datagram.
    // Callback must be thread-safe and SHOULD return quickly (heavy
    // work should be dispatched to a worker thread by the caller).
    using ReceiveCallback =
        std::function<void(const UdpAddr& src, std::vector<uint8_t> data)>;

    UdpSocket();
    ~UdpSocket();

    UdpSocket(const UdpSocket&) = delete;
    UdpSocket& operator=(const UdpSocket&) = delete;

    // Binds an IPv4 + IPv6 socket pair to `port` on all interfaces
    // (INADDR_ANY / in6addr_any) and starts the reader thread.
    // If `port == 0`, the OS picks; query bound_port() afterwards.
    // Returns false if either bind fails or if already active.
    bool Bind(uint16_t port);

    // Stops the reader thread, closes both sockets, and (defensively)
    // clears the receive callback to break any retain cycle. Idempotent.
    void Stop();

    // Best-effort send. Blocks until the kernel buffer accepts (typically
    // microseconds for sub-MTU datagrams). Returns false on socket error
    // or unsupported address family.
    bool SendTo(const UdpAddr& dest, const uint8_t* data, size_t len);

    // Must be called before Bind() (or guaranteed to be set before any
    // datagram could arrive). Setting to nullptr disables dispatch.
    void OnReceive(ReceiveCallback cb);

    bool active() const { return active_.load(std::memory_order_acquire); }

    // After successful Bind, the actual OS-assigned port (== requested
    // port unless the caller passed 0).
    uint16_t bound_port() const { return bound_port_; }

private:
    void ReaderLoop();
    void DispatchOne(int family_id);

    // Held in the .cpp to keep platform socket types out of the header.
    struct Impl;
    std::unique_ptr<Impl> impl_;

    std::atomic<bool> active_{false};
    std::atomic<bool> stop_{false};
    uint16_t bound_port_{0};

    std::thread reader_thread_;
    std::mutex cb_mutex_;
    ReceiveCallback cb_;
};

}  // namespace dinero::network
