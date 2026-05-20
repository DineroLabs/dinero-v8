// Copyright (c) 2026 The Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

#include "network/udp_socket.h"

#include <chrono>
#include <cstdint>
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
    enum class Role {
        Client,
        Server,
    };

    QuicSession();
    ~QuicSession();

    QuicSession(const QuicSession&) = delete;
    QuicSession& operator=(const QuicSession&) = delete;

    bool StartClient(const UdpAddr& local,
                     const UdpAddr& remote,
                     const QuicSessionOptions& options = QuicSessionOptions{});

    bool StartServerFromInitial(const UdpAddr& local,
                                const UdpAddr& remote,
                                const std::vector<uint8_t>& first_packet,
                                const QuicSessionOptions& options);

    bool ReceivePacket(const UdpAddr& local,
                       const UdpAddr& remote,
                       const std::vector<uint8_t>& packet);
    bool HandleExpiry();

    bool QueueStreamData(const std::vector<uint8_t>& payload, bool fin = true);
    bool DrainOutgoing(std::vector<std::vector<uint8_t>>* packets);

    std::vector<uint8_t> TakeReceivedStreamData();
    QuicSessionStats Stats() const;

    bool active() const;
    bool handshake_ready() const;
    std::string last_error() const;

private:
    struct Impl;
    Impl* impl_;
};

}  // namespace dinero::network
