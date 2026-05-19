// Copyright (c) 2026 The Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "network/udp_socket.h"

#include <cstdio>
#include <cstring>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
using socket_t = SOCKET;
static constexpr socket_t kInvalidSocket = INVALID_SOCKET;
static int close_socket(socket_t s) { return ::closesocket(s); }
static int last_socket_error() { return WSAGetLastError(); }
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#include <errno.h>
using socket_t = int;
static constexpr socket_t kInvalidSocket = -1;
static int close_socket(socket_t s) { return ::close(s); }
static int last_socket_error() { return errno; }
#endif

namespace dinero::network {

struct UdpSocket::Impl {
    socket_t v4{kInvalidSocket};
    socket_t v6{kInvalidSocket};
};

UdpAddr UdpAddr::FromIPv4(const uint8_t ip4[4], uint16_t port) {
    UdpAddr a;
    a.family = Family::V4;
    std::memcpy(a.ip.data(), ip4, 4);
    a.port = port;
    return a;
}

UdpAddr UdpAddr::FromIPv6(const uint8_t ip6[16], uint16_t port) {
    UdpAddr a;
    a.family = Family::V6;
    std::memcpy(a.ip.data(), ip6, 16);
    a.port = port;
    return a;
}

std::string UdpAddr::to_string() const {
    char buf[64];
    if (family == Family::V4) {
        std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u:%u",
                      ip[0], ip[1], ip[2], ip[3], port);
        return buf;
    }
    // IPv6 — use inet_ntop for canonical formatting.
    char addr_buf[INET6_ADDRSTRLEN] = {0};
    in6_addr in6;
    std::memcpy(&in6, ip.data(), 16);
    if (inet_ntop(AF_INET6, &in6, addr_buf, sizeof(addr_buf)) == nullptr) {
        return {};
    }
    std::snprintf(buf, sizeof(buf), "[%s]:%u", addr_buf, port);
    return buf;
}

UdpSocket::UdpSocket() : impl_(std::make_unique<Impl>()) {
#ifdef _WIN32
    // WSAStartup may already be called by the rest of the daemon
    // (P2PManager calls it in start()). We call again defensively —
    // refcounted on Windows, so a matching WSACleanup pairs up.
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
}

UdpSocket::~UdpSocket() {
    Stop();
#ifdef _WIN32
    WSACleanup();
#endif
}

void UdpSocket::OnReceive(ReceiveCallback cb) {
    std::lock_guard<std::mutex> lock(cb_mutex_);
    cb_ = std::move(cb);
}

bool UdpSocket::Bind(uint16_t port) {
    if (active_.load(std::memory_order_acquire)) {
        return false;
    }

    // IPv4 socket
    impl_->v4 = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (impl_->v4 == kInvalidSocket) {
        std::fprintf(stderr, "[UdpSocket] AF_INET socket() failed: %d\n",
                     last_socket_error());
        return false;
    }
    sockaddr_in v4_bind{};
    v4_bind.sin_family = AF_INET;
    v4_bind.sin_port = htons(port);
    v4_bind.sin_addr.s_addr = htonl(INADDR_ANY);
    if (::bind(impl_->v4, reinterpret_cast<sockaddr*>(&v4_bind),
               sizeof(v4_bind)) != 0) {
        std::fprintf(stderr, "[UdpSocket] AF_INET bind(%u) failed: %d\n",
                     port, last_socket_error());
        close_socket(impl_->v4);
        impl_->v4 = kInvalidSocket;
        return false;
    }

    // Discover the actually-assigned port (in case caller passed 0)
    // before opening the v6 socket so we can bind v6 to the same port.
    sockaddr_in actual{};
    socklen_t actual_len = sizeof(actual);
    if (::getsockname(impl_->v4, reinterpret_cast<sockaddr*>(&actual),
                      &actual_len) == 0) {
        bound_port_ = ntohs(actual.sin_port);
    } else {
        bound_port_ = port;
    }

    // IPv6 socket. Set IPV6_V6ONLY so dual-stack OSes don't return both
    // mappings on each datagram — we want v4 traffic on v4 socket only.
    impl_->v6 = ::socket(AF_INET6, SOCK_DGRAM, 0);
    if (impl_->v6 == kInvalidSocket) {
        std::fprintf(stderr, "[UdpSocket] AF_INET6 socket() failed: %d (continuing v4-only)\n",
                     last_socket_error());
        // Non-fatal — v4-only is still usable; STUN/QUIC paths just
        // won't reach v6 peers.
    } else {
        int v6_only = 1;
        setsockopt(impl_->v6, IPPROTO_IPV6, IPV6_V6ONLY,
                   reinterpret_cast<const char*>(&v6_only), sizeof(v6_only));

        sockaddr_in6 v6_bind{};
        v6_bind.sin6_family = AF_INET6;
        v6_bind.sin6_port = htons(bound_port_);
        v6_bind.sin6_addr = in6addr_any;
        if (::bind(impl_->v6, reinterpret_cast<sockaddr*>(&v6_bind),
                   sizeof(v6_bind)) != 0) {
            std::fprintf(stderr, "[UdpSocket] AF_INET6 bind(%u) failed: %d (continuing v4-only)\n",
                         bound_port_, last_socket_error());
            close_socket(impl_->v6);
            impl_->v6 = kInvalidSocket;
        }
    }

    active_.store(true, std::memory_order_release);
    stop_.store(false, std::memory_order_release);
    reader_thread_ = std::thread([this]() { ReaderLoop(); });
    return true;
}

void UdpSocket::Stop() {
    if (!active_.load(std::memory_order_acquire)) return;
    stop_.store(true, std::memory_order_release);
    if (reader_thread_.joinable()) {
        // Reader loop wakes via select() timeout — bounded by 1s.
        reader_thread_.join();
    }
    if (impl_->v4 != kInvalidSocket) {
        close_socket(impl_->v4);
        impl_->v4 = kInvalidSocket;
    }
    if (impl_->v6 != kInvalidSocket) {
        close_socket(impl_->v6);
        impl_->v6 = kInvalidSocket;
    }
    active_.store(false, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(cb_mutex_);
        cb_ = nullptr;
    }
}

bool UdpSocket::SendTo(const UdpAddr& dest, const uint8_t* data, size_t len) {
    if (!active_.load(std::memory_order_acquire) || !data || len == 0) {
        return false;
    }
    if (dest.family == UdpAddr::Family::V4) {
        if (impl_->v4 == kInvalidSocket) return false;
        sockaddr_in to{};
        to.sin_family = AF_INET;
        to.sin_port = htons(dest.port);
        std::memcpy(&to.sin_addr.s_addr, dest.ip.data(), 4);
        const auto sent = ::sendto(impl_->v4,
                                   reinterpret_cast<const char*>(data),
                                   static_cast<int>(len), 0,
                                   reinterpret_cast<sockaddr*>(&to), sizeof(to));
        return sent == static_cast<int>(len);
    }
    if (dest.family == UdpAddr::Family::V6) {
        if (impl_->v6 == kInvalidSocket) return false;
        sockaddr_in6 to{};
        to.sin6_family = AF_INET6;
        to.sin6_port = htons(dest.port);
        std::memcpy(&to.sin6_addr, dest.ip.data(), 16);
        const auto sent = ::sendto(impl_->v6,
                                   reinterpret_cast<const char*>(data),
                                   static_cast<int>(len), 0,
                                   reinterpret_cast<sockaddr*>(&to), sizeof(to));
        return sent == static_cast<int>(len);
    }
    return false;
}

void UdpSocket::ReaderLoop() {
    constexpr size_t kRecvBufSize = 65535;  // max UDP payload
    std::vector<uint8_t> buf(kRecvBufSize);

    while (!stop_.load(std::memory_order_acquire)) {
        fd_set rd;
        FD_ZERO(&rd);
        socket_t max_fd = 0;
        if (impl_->v4 != kInvalidSocket) {
            FD_SET(impl_->v4, &rd);
            if (impl_->v4 > max_fd) max_fd = impl_->v4;
        }
        if (impl_->v6 != kInvalidSocket) {
            FD_SET(impl_->v6, &rd);
            if (impl_->v6 > max_fd) max_fd = impl_->v6;
        }
        if (max_fd == 0) break;  // both sockets gone

        timeval tv{};
        tv.tv_sec = 1;
        tv.tv_usec = 0;
#ifdef _WIN32
        int sel = ::select(0, &rd, nullptr, nullptr, &tv);
#else
        int sel = ::select(static_cast<int>(max_fd) + 1, &rd, nullptr, nullptr, &tv);
#endif
        if (sel <= 0) continue;  // timeout or error; loop and check stop_

        if (impl_->v4 != kInvalidSocket && FD_ISSET(impl_->v4, &rd)) {
            sockaddr_in from{};
            socklen_t from_len = sizeof(from);
            const auto got = ::recvfrom(impl_->v4,
                                        reinterpret_cast<char*>(buf.data()),
                                        static_cast<int>(buf.size()), 0,
                                        reinterpret_cast<sockaddr*>(&from), &from_len);
            if (got > 0) {
                UdpAddr src;
                src.family = UdpAddr::Family::V4;
                std::memcpy(src.ip.data(), &from.sin_addr.s_addr, 4);
                src.port = ntohs(from.sin_port);
                std::vector<uint8_t> data(buf.begin(), buf.begin() + got);
                ReceiveCallback cb_copy;
                {
                    std::lock_guard<std::mutex> lock(cb_mutex_);
                    cb_copy = cb_;
                }
                if (cb_copy) cb_copy(src, std::move(data));
            }
        }
        if (impl_->v6 != kInvalidSocket && FD_ISSET(impl_->v6, &rd)) {
            sockaddr_in6 from{};
            socklen_t from_len = sizeof(from);
            const auto got = ::recvfrom(impl_->v6,
                                        reinterpret_cast<char*>(buf.data()),
                                        static_cast<int>(buf.size()), 0,
                                        reinterpret_cast<sockaddr*>(&from), &from_len);
            if (got > 0) {
                UdpAddr src;
                src.family = UdpAddr::Family::V6;
                std::memcpy(src.ip.data(), &from.sin6_addr, 16);
                src.port = ntohs(from.sin6_port);
                std::vector<uint8_t> data(buf.begin(), buf.begin() + got);
                ReceiveCallback cb_copy;
                {
                    std::lock_guard<std::mutex> lock(cb_mutex_);
                    cb_copy = cb_;
                }
                if (cb_copy) cb_copy(src, std::move(data));
            }
        }
    }
}

void UdpSocket::DispatchOne(int /*family_id*/) {
    // Reserved for future use (e.g. external poll-driven mode). Today
    // ReaderLoop handles dispatch internally — this declaration exists
    // so callers can subclass / replace the dispatch path without API
    // churn when QUIC integration in Phase B2 needs explicit drives.
}

}  // namespace dinero::network
