// Copyright (c) 2026 The Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

#include "network/udp_socket.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace dinero::network {

struct QuicTransportInfo {
    bool ngtcp2_available{false};
    bool crypto_available{false};
    bool mainnet_relay_ready{false};
    std::string ngtcp2_version;
    std::string openssl_version;
    std::string crypto_backend{"none"};
    std::string disabled_reason;
};

struct QuicDatagram {
    UdpAddr source;
    std::vector<uint8_t> payload;
};

class QuicTransport {
public:
    struct Options {
        uint16_t listen_port{0};
        size_t max_pending_datagrams{256};
    };

    QuicTransport();
    ~QuicTransport();

    QuicTransport(const QuicTransport&) = delete;
    QuicTransport& operator=(const QuicTransport&) = delete;

    static QuicTransportInfo CompileInfo();
    static bool InitializeCrypto();

    // Mainnet relay stays gated until a complete encrypted stream/session
    // transport is wired into P2PManager. This helper makes that explicit for
    // callers and tests instead of letting dependency presence imply readiness.
    static bool MainnetRelayReady();

    bool Start(const Options& options = Options{});
    void Stop();

    bool SendDatagram(const UdpAddr& destination, const std::vector<uint8_t>& payload);
    bool ReceiveDatagram(QuicDatagram* out, std::chrono::milliseconds timeout);

    bool active() const;
    uint16_t bound_port() const;
    std::string last_error() const;

private:
    void OnDatagram(const UdpAddr& source, std::vector<uint8_t> payload);
    void SetLastError(std::string message);

    UdpSocket udp_;
    size_t max_pending_datagrams_{256};

    mutable std::mutex mutex_;
    std::condition_variable datagram_cv_;
    std::deque<QuicDatagram> pending_datagrams_;
    std::string last_error_;
};

}  // namespace dinero::network
