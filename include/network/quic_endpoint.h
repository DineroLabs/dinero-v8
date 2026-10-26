#pragma once

// One UDP socket on the P2P port, demultiplexed into per-peer QuicSessions.
//
// QuicSession is a pure byte-pipe driver: it neither owns a socket nor sees
// source addresses. Relay circuits feed it through synthetic addresses; direct
// peers need a real socket shared by every session, which is this class.
//
// Routing is by the datagram's source address. A datagram from a known remote
// goes to that remote's session and nowhere else; a datagram from an unknown
// remote can only start a session if it is shaped like a QUIC v1 client
// Initial, passes the per-IP and global rate limits, and the owner's AcceptFn
// agrees. Everything else is dropped and counted.

#include "network/quic_session.h"
#include "network/udp_socket.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

namespace dinero::network {

class QuicEndpoint {
public:
    struct Limits {
        uint32_t initials_per_sec_global = 20;
        uint32_t initials_per_sec_per_ip = 4;
        // Inbound sessions that have not finished their handshake. Forged
        // Initials can only ever hold this many; each expires after 10 s.
        size_t max_pending_inbound = 32;
    };
    struct Counters {
        uint64_t datagrams_routed{0};      // delivered to an existing session
        uint64_t datagrams_dropped{0};     // unknown source and not an acceptable Initial
        uint64_t initials_rate_limited{0};
        uint64_t sessions_accepted{0};
    };

    // Return false to refuse (bans, inbound caps, dedup). Runs on the socket
    // thread; must not block.
    using AcceptFn = std::function<bool(const UdpAddr& remote)>;
    // A new inbound session; the callee shares ownership. Runs on the socket
    // thread; must not block.
    using InboundFn = std::function<void(std::shared_ptr<QuicSession>, const UdpAddr& remote)>;

    QuicEndpoint();
    ~QuicEndpoint();
    QuicEndpoint(const QuicEndpoint&) = delete;
    QuicEndpoint& operator=(const QuicEndpoint&) = delete;

    // Bind `port` (0 = ephemeral) on v4 and, best-effort, v6. False when QUIC
    // is not compiled in or the bind fails.
    bool Start(uint16_t port, QuicSessionOptions server_opts, AcceptFn, InboundFn);
    bool Start(uint16_t port, QuicSessionOptions server_opts, AcceptFn, InboundFn,
               Limits limits);
    // Dial `remote`. Null if a session to `remote` already exists or the
    // endpoint is not running. The handshake completes asynchronously.
    std::shared_ptr<QuicSession> Connect(const UdpAddr& remote, QuicSessionOptions client_opts);
    // Stop routing to `remote` (its owner tore the session down).
    void Forget(const UdpAddr& remote);
    // Close every session (each sends CONNECTION_CLOSE), give them a moment to
    // reach the wire, then stop the socket.
    void Stop();
    // Tests: simulate this host vanishing -- every outgoing datagram is
    // dropped from now on, nothing is closed.
    void SilenceForTest();
    // Tests: deliver `data` as if it arrived from `src` (source spoofing).
    void TestInjectDatagram(const UdpAddr& src, std::vector<uint8_t> data) {
        OnDatagram(src, std::move(data));
    }

    bool Listening() const;
    uint16_t Port() const;
    size_t SessionCount() const;
    Counters Stats() const;

    // Exposed for tests: the Initial shape check used for unknown sources.
    static bool LooksLikeClientInitial(const uint8_t* data, size_t len);

private:
    struct Shared;   // socket + liveness, shared with every session writer
    struct Bucket { double tokens; std::chrono::steady_clock::time_point last; };
    struct Entry {
        std::shared_ptr<QuicSession> session;
        std::chrono::steady_clock::time_point created;
        bool server{false};                 // created from an inbound Initial
        std::vector<uint8_t> client_scid;   // that Initial's source CID (the client's
                                            // own CID: constant across its Initials)
    };

    void OnDatagram(const UdpAddr& src, std::vector<uint8_t> data);
    bool TakeToken(Bucket& b, uint32_t per_sec, std::chrono::steady_clock::time_point now);
    std::shared_ptr<QuicSession> MakeSession(const UdpAddr& remote);
    UdpAddr LocalAddr() const;

    std::shared_ptr<Shared> shared_;
    QuicSessionOptions server_opts_;
    AcceptFn accept_;
    InboundFn inbound_;
    Limits limits_;

    mutable std::mutex mu_;
    std::map<std::pair<std::array<uint8_t, 17>, uint16_t>, Entry> sessions_;
    std::map<std::array<uint8_t, 17>, Bucket> ip_buckets_;
    Bucket global_bucket_{};
    Counters counters_;
};

}  // namespace dinero::network
