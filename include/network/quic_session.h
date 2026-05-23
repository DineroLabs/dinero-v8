// Copyright (c) 2026 The Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

#include "network/udp_socket.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <string>
#include <vector>

namespace dinero::network {

struct QuicSessionOptions {
    std::string alpn{"dinero-relay/1"};
    std::string server_name{"localhost"};
    std::string certificate_pem;
    std::string private_key_pem;
    bool verify_peer{false};
    size_t max_datagram_size{1500};
};

struct QuicSessionStats {
    bool active{false};
    bool handshake_completed{false};
    bool handshake_confirmed{false};
    bool stream_closed{false};
    std::string tls_cipher;
    std::string selected_alpn;
};

class QuicSession {
public:
    // Wire-bytes-out callback. Invoked from the owning session thread.
    // Implementations MUST be non-blocking or at most briefly blocking
    // (they hand the bytes to the relay socket writer). If they block,
    // they stall only this session — that's correct semantics.
    using OutboundWriter = std::function<void(std::vector<uint8_t>)>;

    enum class Role {
        Client,
        Server,
    };

    // Construction starts the owning thread, which blocks on the inbox
    // until Start{Client,Server} is called. `writer` must be valid for the
    // lifetime of the session.
    explicit QuicSession(OutboundWriter writer);

    // Destructor signals the owning thread to stop, drains pending events,
    // and joins. Safe to call at any time.
    ~QuicSession();

    QuicSession(const QuicSession&) = delete;
    QuicSession& operator=(const QuicSession&) = delete;

    // Initiate handshake as client. Schedules work onto the owning thread
    // and returns. After this, the session thread will drive ngtcp2,
    // emit outgoing bytes via `writer`, and resolve the handshake future
    // when complete.
    bool StartClient(const UdpAddr& local,
                     const UdpAddr& remote,
                     const QuicSessionOptions& options = QuicSessionOptions{});

    // Prime the session for server role. Subsequent EnqueueIncomingPacket
    // calls will cause StartServerFromInitial to be invoked on the first
    // packet, then ReceivePacket on it and all subsequent packets.
    bool StartServer(const UdpAddr& local,
                     const UdpAddr& remote,
                     const QuicSessionOptions& options);

    // Deliver an inbound QUIC packet. Called by the relay listen thread
    // on every RELAY_DATA frame. Non-blocking; pushes to the inbox and
    // returns immediately.
    void EnqueueIncomingPacket(std::vector<uint8_t> packet);

    // Enqueue application-layer stream bytes to send. Called by the
    // peer-handler thread (via send_relay_data_to_virtual_peer).
    // Non-blocking.
    void QueueOutgoingStream(std::vector<uint8_t> payload, bool fin = false);

    // Handshake-completion future. Resolves true when both sides have
    // completed the QUIC handshake; resolves false on session failure,
    // close, or destruction. Safe to call from any thread; multiple
    // concurrent waiters are allowed (shared_future).
    std::shared_future<bool> WaitHandshakeReady();

    // Pop any decrypted application-layer bytes that the session thread
    // has emitted since the last call. Blocks up to `timeout` if nothing
    // is available. Returns empty vector on timeout or session close.
    std::vector<uint8_t> ReadDecryptedStream(std::chrono::milliseconds timeout);

    // Snapshot of state. Safe from any thread; reads atomic snapshot
    // fields the session thread publishes.
    QuicSessionStats Stats() const;

    bool active() const;
    bool handshake_ready() const;
    std::string last_error() const;

    // Initiate graceful close. The session thread will drain the inbox,
    // emit any pending CONNECTION_CLOSE, and exit.
    void Close();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace dinero::network
