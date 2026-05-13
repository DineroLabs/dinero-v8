#include "dinero/seeder/connection.h"

#include "dinero/seeder/wire.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <random>

namespace dinero {
namespace seeder {

namespace {

class FdGuard {
 public:
    explicit FdGuard(int fd = -1) : fd_(fd) {}
    ~FdGuard() { reset(); }
    FdGuard(const FdGuard&) = delete;
    FdGuard& operator=(const FdGuard&) = delete;
    int get() const { return fd_; }
    int release() { int f = fd_; fd_ = -1; return f; }
    void reset(int fd = -1) {
        if (fd_ >= 0) ::close(fd_);
        fd_ = fd;
    }

 private:
    int fd_;
};

uint64_t random_nonce() {
    std::random_device rd;
    return (static_cast<uint64_t>(rd()) << 32) | rd();
}

int connect_with_timeout(const sockaddr_in& addr,
                          std::chrono::milliseconds timeout,
                          std::string& err) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        err = "socket(): ";
        err += std::strerror(errno);
        return -1;
    }

    int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        err = "fcntl(O_NONBLOCK): ";
        err += std::strerror(errno);
        ::close(fd);
        return -1;
    }

    int rc = ::connect(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr));
    if (rc == 0) {
        return fd;  // immediate success (rare)
    }
    if (rc < 0 && errno != EINPROGRESS) {
        err = "connect(): ";
        err += std::strerror(errno);
        ::close(fd);
        return -1;
    }

    fd_set wfds;
    FD_ZERO(&wfds);
    FD_SET(fd, &wfds);
    timeval tv{};
    tv.tv_sec = timeout.count() / 1000;
    tv.tv_usec = (timeout.count() % 1000) * 1000;

    rc = ::select(fd + 1, nullptr, &wfds, nullptr, &tv);
    if (rc == 0) {
        err = "connect timed out";
        ::close(fd);
        return -1;
    }
    if (rc < 0) {
        err = "select(): ";
        err += std::strerror(errno);
        ::close(fd);
        return -1;
    }

    int sockerr = 0;
    socklen_t errlen = sizeof(sockerr);
    if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &sockerr, &errlen) < 0) {
        err = "getsockopt(SO_ERROR): ";
        err += std::strerror(errno);
        ::close(fd);
        return -1;
    }
    if (sockerr != 0) {
        err = "connect rejected: ";
        err += std::strerror(sockerr);
        ::close(fd);
        return -1;
    }

    return fd;
}

bool send_all(int fd, const std::vector<uint8_t>& data,
              std::chrono::milliseconds budget) {
    const auto deadline = std::chrono::steady_clock::now() + budget;
    size_t sent = 0;
    while (sent < data.size()) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        ssize_t n = ::send(fd, data.data() + sent, data.size() - sent, 0);
        if (n > 0) {
            sent += static_cast<size_t>(n);
        } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            fd_set wfds;
            FD_ZERO(&wfds);
            FD_SET(fd, &wfds);
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now());
            timeval tv{};
            tv.tv_sec = remaining.count() / 1000;
            tv.tv_usec = (remaining.count() % 1000) * 1000;
            int rc = ::select(fd + 1, nullptr, &wfds, nullptr, &tv);
            if (rc <= 0) return false;
        } else if (n < 0 && errno == EINTR) {
            continue;
        } else {
            return false;
        }
    }
    return true;
}

// Read frames from socket into `inbuf` until `predicate` returns true on
// some parsed frame or the deadline passes. Sets `out` to the matching
// frame on success.
bool read_until(int fd,
                std::vector<uint8_t>& inbuf,
                std::chrono::steady_clock::time_point deadline,
                Frame& out,
                bool (*predicate)(const Frame&)) {
    while (true) {
        size_t consumed = 0;
        size_t scan = 0;
        while (scan < inbuf.size()) {
            Frame frame;
            consumed = 0;
            if (parse_frame(inbuf.data() + scan, inbuf.size() - scan, frame, consumed)) {
                if (predicate(frame)) {
                    out = std::move(frame);
                    inbuf.erase(inbuf.begin(), inbuf.begin() + scan + consumed);
                    return true;
                }
                scan += consumed;
            } else if (consumed > 0) {
                // resync by 1 byte
                scan += consumed;
            } else {
                break;  // need more data
            }
        }
        // Drop consumed bytes before this scan position so inbuf doesn't grow unbounded.
        if (scan > 0) inbuf.erase(inbuf.begin(), inbuf.begin() + scan);

        if (std::chrono::steady_clock::now() >= deadline) return false;

        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        timeval tv{};
        tv.tv_sec = remaining.count() / 1000;
        tv.tv_usec = (remaining.count() % 1000) * 1000;
        int rc = ::select(fd + 1, &rfds, nullptr, nullptr, &tv);
        if (rc < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (rc == 0) return false;  // timed out

        uint8_t scratch[4096];
        ssize_t n = ::recv(fd, scratch, sizeof(scratch), 0);
        if (n <= 0) return false;
        inbuf.insert(inbuf.end(), scratch, scratch + n);
    }
}

}  // namespace

ProbeResult probe_peer(const std::string& host,
                        uint16_t port,
                        const ProbeConfig& cfg) {
    const auto t_start = std::chrono::steady_clock::now();
    ProbeResult result;

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
        // Try DNS.
        addrinfo hints{};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        addrinfo* res = nullptr;
        if (::getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0 || !res) {
            result.outcome = ProbeOutcome::ConnectRefused;
            result.error_detail = "DNS resolution failed";
            result.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - t_start);
            if (res) ::freeaddrinfo(res);
            return result;
        }
        addr.sin_addr = reinterpret_cast<sockaddr_in*>(res->ai_addr)->sin_addr;
        ::freeaddrinfo(res);
    }

    FdGuard fd;
    {
        std::string err;
        int sock = connect_with_timeout(addr, cfg.connect_timeout, err);
        if (sock < 0) {
            result.outcome = err.find("timed out") != std::string::npos
                ? ProbeOutcome::ConnectTimeout
                : ProbeOutcome::ConnectRefused;
            result.error_detail = std::move(err);
            result.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - t_start);
            return result;
        }
        fd.reset(sock);
    }

    // Send our version.
    const uint64_t nonce = random_nonce();
    auto our_version = build_frame("version",
                                    build_version_payload(nonce, cfg.user_agent,
                                                          cfg.best_height));
    if (!send_all(fd.get(), our_version, cfg.handshake_timeout)) {
        result.outcome = ProbeOutcome::SocketError;
        result.error_detail = "send(version) failed";
        result.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t_start);
        return result;
    }

    std::vector<uint8_t> inbuf;
    const auto handshake_deadline =
        std::chrono::steady_clock::now() + cfg.handshake_timeout;

    // Wait for peer's version.
    Frame their_version;
    if (!read_until(fd.get(), inbuf, handshake_deadline, their_version,
                    [](const Frame& f) { return f.command == "version"; })) {
        result.outcome = ProbeOutcome::HandshakeTimeout;
        result.error_detail = "no version reply";
        result.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t_start);
        return result;
    }

    // Parse their version: protocol(4) + services(8) + timestamp(8) +
    // addrRecv(26) + addrFrom(26) + nonce(8) + ua_varstring + best_height(4).
    if (their_version.payload.size() < 4 + 8 + 8 + 26 + 26 + 8 + 1 + 4) {
        result.outcome = ProbeOutcome::HandshakeMalformed;
        result.error_detail = "version payload too short";
        result.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t_start);
        return result;
    }
    const auto& vp = their_version.payload;
    result.remote_protocol_version =
        static_cast<uint32_t>(vp[0]) | (static_cast<uint32_t>(vp[1]) << 8) |
        (static_cast<uint32_t>(vp[2]) << 16) | (static_cast<uint32_t>(vp[3]) << 24);
    for (int i = 0; i < 8; ++i) {
        result.remote_services |= (static_cast<uint64_t>(vp[4 + i]) << (i * 8));
    }
    // Skip timestamp(8) + addrRecv(26) + addrFrom(26) + nonce(8) = 68 bytes from offset 12.
    size_t ua_offset = 4 + 8 + 8 + 26 + 26 + 8;  // = 80
    uint64_t ua_len = vp[ua_offset++];
    if (ua_len == 0xFD && ua_offset + 2 <= vp.size()) {
        ua_len = vp[ua_offset] | (static_cast<uint64_t>(vp[ua_offset + 1]) << 8);
        ua_offset += 2;
    } else if (ua_len >= 0xFE) {
        // Skip exotic varint lengths — peer is misbehaving for a UA string.
        result.outcome = ProbeOutcome::HandshakeMalformed;
        result.error_detail = "implausible UA length";
        return result;
    }
    if (ua_offset + ua_len + 4 > vp.size()) {
        result.outcome = ProbeOutcome::HandshakeMalformed;
        result.error_detail = "version payload truncated at UA";
        return result;
    }
    result.remote_user_agent.assign(reinterpret_cast<const char*>(&vp[ua_offset]), ua_len);
    ua_offset += ua_len;
    result.remote_best_height =
        static_cast<uint32_t>(vp[ua_offset]) |
        (static_cast<uint32_t>(vp[ua_offset + 1]) << 8) |
        (static_cast<uint32_t>(vp[ua_offset + 2]) << 16) |
        (static_cast<uint32_t>(vp[ua_offset + 3]) << 24);

    if (result.remote_protocol_version < cfg.protocol_version_floor) {
        result.outcome = ProbeOutcome::ProtocolMismatch;
        result.error_detail = "remote protocol version " +
            std::to_string(result.remote_protocol_version) + " < floor " +
            std::to_string(cfg.protocol_version_floor);
        result.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t_start);
        return result;
    }

    // Send verack + getaddr.
    auto verack = build_frame("verack", {});
    auto getaddr = build_frame("getaddr", build_getaddr_payload());
    if (!send_all(fd.get(), verack, cfg.handshake_timeout) ||
        !send_all(fd.get(), getaddr, cfg.handshake_timeout)) {
        result.outcome = ProbeOutcome::SocketError;
        result.error_detail = "send(verack/getaddr) failed";
        result.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t_start);
        return result;
    }

    // Wait for verack (don't block long — peer may send addr first).
    const auto verack_deadline =
        std::chrono::steady_clock::now() + cfg.handshake_timeout;
    Frame ignored;
    (void)read_until(fd.get(), inbuf, verack_deadline, ignored,
                     [](const Frame& f) { return f.command == "verack"; });

    // Wait up to addr_timeout for an addr message. Peers may send 0 or
    // more addr replies — we collect the first one for now. The seeder
    // doesn't need exhaustive addr collection per peer; getting any
    // non-empty addr proves the peer is participating in gossip.
    const auto addr_deadline =
        std::chrono::steady_clock::now() + cfg.addr_timeout;
    Frame addr_frame;
    if (read_until(fd.get(), inbuf, addr_deadline, addr_frame,
                   [](const Frame& f) { return f.command == "addr"; })) {
        result.learned_addresses = parse_addr_payload(addr_frame.payload);
    }
    // If no addr arrives in time, that's NOT an error — peer is healthy
    // for handshake purposes; just didn't have peers to share. Many
    // fresh nodes legitimately have empty addr replies.

    result.outcome = ProbeOutcome::Success;
    result.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t_start);
    return result;
}

}  // namespace seeder
}  // namespace dinero
