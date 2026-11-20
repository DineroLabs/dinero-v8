#include "wallet/swap/btc_p2p_client.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <ctime>
#include <random>
#include <set>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace dinero::swap {
namespace {

#ifdef _WIN32
using SocketHandle = SOCKET;
constexpr SocketHandle kNoSocket = INVALID_SOCKET;
void CloseSocket(SocketHandle s) { ::closesocket(s); }
bool WouldBlock() {
    const int e = ::WSAGetLastError();
    return e == WSAEWOULDBLOCK || e == WSAEINPROGRESS;
}
int PollSocket(SocketHandle s, bool for_write, int timeout_ms) {
    WSAPOLLFD p{};
    p.fd = s;
    p.events = for_write ? POLLWRNORM : POLLRDNORM;
    return ::WSAPoll(&p, 1, timeout_ms);
}
void StartSockets() {
    static const bool started = [] {
        WSADATA data;
        return ::WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    (void)started;
}
bool SetNonBlocking(SocketHandle s) {
    u_long on = 1;
    return ::ioctlsocket(s, FIONBIO, &on) == 0;
}
constexpr int kSendFlags = 0;
#else
using SocketHandle = int;
constexpr SocketHandle kNoSocket = -1;
void CloseSocket(SocketHandle s) { ::close(s); }
bool WouldBlock() { return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINPROGRESS || errno == EINTR; }
int PollSocket(SocketHandle s, bool for_write, int timeout_ms) {
    pollfd p{};
    p.fd = s;
    p.events = for_write ? POLLOUT : POLLIN;
    return ::poll(&p, 1, timeout_ms);
}
void StartSockets() {}
bool SetNonBlocking(SocketHandle s) {
    const int flags = ::fcntl(s, F_GETFL, 0);
    return flags >= 0 && ::fcntl(s, F_SETFL, flags | O_NONBLOCK) == 0;
}
#ifdef MSG_NOSIGNAL
constexpr int kSendFlags = MSG_NOSIGNAL;  // a peer that hung up must not raise SIGPIPE
#else
constexpr int kSendFlags = 0;
#endif
#endif

struct Socket {
    SocketHandle handle{kNoSocket};
    Socket() = default;
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
    ~Socket() {
        if (handle != kNoSocket) CloseSocket(handle);
    }
};

using Clock = std::chrono::steady_clock;

int MillisecondsUntil(Clock::time_point deadline) {
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
    return left <= 0 ? 0 : static_cast<int>(std::min<long long>(left, 60'000));
}

uint32_t WallClock() { return static_cast<uint32_t>(std::time(nullptr)); }

// Writes everything or says why not.
std::string SendAll(SocketHandle s, const std::vector<uint8_t>& bytes, Clock::time_point deadline) {
    size_t sent = 0;
    while (sent < bytes.size()) {
        const auto n = ::send(s, reinterpret_cast<const char*>(bytes.data()) + sent,
                              static_cast<int>(std::min<size_t>(bytes.size() - sent, 1 << 20)), kSendFlags);
        if (n > 0) {
            sent += static_cast<size_t>(n);
            continue;
        }
        if (n < 0 && !WouldBlock()) return "send failed";
        const int wait = MillisecondsUntil(deadline);
        if (wait == 0 || PollSocket(s, /*for_write=*/true, wait) <= 0) return "timed out sending";
    }
    return "";
}

}  // namespace

std::vector<BtcPeerEndpoint> ResolveBtcPeers(const std::vector<std::string>& hosts, uint16_t port, size_t limit) {
    StartSockets();
    std::vector<std::vector<BtcPeerEndpoint>> per_host;
    for (const std::string& host : hosts) {
        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        addrinfo* found = nullptr;
        if (::getaddrinfo(host.c_str(), nullptr, &hints, &found) != 0 || !found) continue;
        std::vector<BtcPeerEndpoint> endpoints;
        for (const addrinfo* a = found; a && endpoints.size() < 64; a = a->ai_next) {
            char text[INET6_ADDRSTRLEN] = {};
            const void* raw = a->ai_family == AF_INET
                                  ? static_cast<const void*>(&reinterpret_cast<const sockaddr_in*>(a->ai_addr)->sin_addr)
                                  : a->ai_family == AF_INET6
                                        ? static_cast<const void*>(&reinterpret_cast<const sockaddr_in6*>(a->ai_addr)->sin6_addr)
                                        : nullptr;
            if (raw && ::inet_ntop(a->ai_family, raw, text, sizeof text)) endpoints.push_back({text, port});
        }
        ::freeaddrinfo(found);
        per_host.push_back(std::move(endpoints));
    }
    // One from each host in turn, so a single seed cannot fill the list.
    std::vector<BtcPeerEndpoint> out;
    std::set<std::string> seen;
    for (size_t round = 0; out.size() < limit; ++round) {
        bool any = false;
        for (const auto& endpoints : per_host) {
            if (round >= endpoints.size()) continue;
            any = true;
            if (out.size() < limit && seen.insert(endpoints[round].address).second) out.push_back(endpoints[round]);
        }
        if (!any) break;
    }
    return out;
}

BtcPeerOutcome RunBtcPeer(const BtcPeerEndpoint& peer, const BtcP2pNetwork& net, BtcHeaderChain& chain,
                          const std::vector<std::vector<uint8_t>>& txs_to_announce, const BtcPeerOptions& options) {
    BtcPeerOutcome out;
    StartSockets();
    const auto finish = Clock::now() + std::chrono::seconds(options.total_timeout_seconds);

    // The address is numeric: no lookup happens here.
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_NUMERICHOST | AI_NUMERICSERV;
    addrinfo* target = nullptr;
    if (::getaddrinfo(peer.address.c_str(), std::to_string(peer.port).c_str(), &hints, &target) != 0 || !target) {
        out.error = "not a numeric peer address";
        return out;
    }
    Socket socket;
    socket.handle = ::socket(target->ai_family, target->ai_socktype, target->ai_protocol);
    if (socket.handle == kNoSocket || !SetNonBlocking(socket.handle)) {
        ::freeaddrinfo(target);
        out.error = "cannot open a socket";
        return out;
    }
#ifdef SO_NOSIGPIPE
    const int on = 1;
    ::setsockopt(socket.handle, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof on);
#endif
    const int started = ::connect(socket.handle, target->ai_addr, static_cast<int>(target->ai_addrlen));
    ::freeaddrinfo(target);
    if (started != 0) {
        if (!WouldBlock()) {
            out.error = "connection refused";
            return out;
        }
        const auto connect_by = std::min(finish, Clock::now() + std::chrono::seconds(options.connect_timeout_seconds));
        if (PollSocket(socket.handle, /*for_write=*/true, MillisecondsUntil(connect_by)) <= 0) {
            out.error = "timed out connecting";
            return out;
        }
        int failure = 0;
        socklen_t length = sizeof failure;
        if (::getsockopt(socket.handle, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&failure), &length) != 0 || failure != 0) {
            out.error = "connection refused";
            return out;
        }
    }
    out.connected = true;

    std::random_device entropy;
    const uint64_t nonce = (static_cast<uint64_t>(entropy()) << 32) | entropy();
    BtcPeerSession session(net, chain, nonce);
    if (const auto why = SendAll(socket.handle, session.Start(WallClock()), finish); !why.empty()) {
        out.error = why;
        return out;
    }

    bool announced = txs_to_announce.empty();
    size_t announced_count = 0;
    std::set<Bytes32> fetched;  // announced transactions the peer has asked for and been given
    Clock::time_point linger_until{};
    std::vector<uint8_t> buffer(64 * 1024);
    while (true) {
        // What we may say now.
        std::vector<uint8_t> say;
        if (session.handshake_complete() && !session.headers_synced() && !session.awaiting_headers()) {
            say = session.RequestHeaders();
        } else if (session.headers_synced() && !announced) {
            for (const auto& tx : txs_to_announce) {
                const auto inv = session.AnnounceTx(tx);
                if (inv.empty()) continue;  // not a transaction, or too many: skipped, reported as not sent
                say.insert(say.end(), inv.begin(), inv.end());
                ++announced_count;
            }
            announced = true;
            linger_until = Clock::now() + std::chrono::seconds(options.linger_seconds);
        }
        if (!say.empty()) {
            if (const auto why = SendAll(socket.handle, say, finish); !why.empty()) {
                out.error = why;
                break;
            }
        }
        // Done?
        if (session.headers_synced() && announced) {
            if (fetched.size() >= announced_count) break;            // the peer has everything we offered
            if (Clock::now() >= linger_until) break;                 // it did not ask: the caller sees which were sent
        }
        // Wait for the peer.
        auto wait_until = std::min(finish, Clock::now() + std::chrono::seconds(options.idle_timeout_seconds));
        if (session.headers_synced() && announced) wait_until = std::min(wait_until, linger_until);
        const int ready = PollSocket(socket.handle, /*for_write=*/false, MillisecondsUntil(wait_until));
        if (ready < 0) {
            out.error = "connection lost";
            break;
        }
        if (ready == 0) {
            if (session.headers_synced() && announced) continue;  // the linger ran out: handled above
            out.error = Clock::now() >= finish ? "conversation took too long" : "peer went silent";
            break;
        }
        const auto n = ::recv(socket.handle, reinterpret_cast<char*>(buffer.data()), static_cast<int>(buffer.size()), 0);
        if (n == 0) {
            out.error = "peer hung up";
            break;
        }
        if (n < 0) {
            if (WouldBlock()) continue;
            out.error = "connection lost";
            break;
        }
        const auto step = session.OnReceived(buffer.data(), static_cast<size_t>(n), WallClock());
        out.headers_accepted += step.headers_accepted;
        fetched.insert(step.txs_sent.begin(), step.txs_sent.end());
        if (session.fault()) {
            out.error = "peer misbehaved: " + *session.fault();
            break;
        }
        if (!step.send.empty()) {
            if (const auto why = SendAll(socket.handle, step.send, finish); !why.empty()) {
                out.error = why;
                break;
            }
        }
        if (Clock::now() >= finish) {
            out.error = "conversation took too long";
            break;
        }
    }
    out.handshake = session.handshake_complete();
    out.headers_synced = session.headers_synced();
    out.peer_height = session.peer_start_height();
    out.txs_sent.assign(fetched.begin(), fetched.end());
    return out;
}

}  // namespace dinero::swap
