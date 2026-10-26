// Copyright (c) 2026 The Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "network/stun_client.h"

#include "crypto/secure_random.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif

namespace dinero::network {

namespace {

constexpr uint32_t kStunMagicCookie = 0x2112A442u;
constexpr uint16_t kStunBindingRequest = 0x0001u;
constexpr uint16_t kStunBindingSuccess = 0x0101u;
constexpr uint16_t kStunAttrXorMappedAddress = 0x0020u;

// STUN transaction ids must be unpredictable enough to make off-path spoofing
// impractical; the magic cookie identifies RFC 5389 traffic, but the txn id is
// what binds a response to one of our outstanding requests.
bool GenerateTxnId(std::array<uint8_t, 12>& out) {
    try {
        auto bytes = secure_random_bytes(out.size());
        std::copy(bytes.begin(), bytes.end(), out.begin());
        return true;
    } catch (...) {
        return false;
    }
}

void WriteBE16(std::vector<uint8_t>* out, uint16_t v) {
    out->push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    out->push_back(static_cast<uint8_t>(v & 0xFF));
}

void WriteBE32(std::vector<uint8_t>* out, uint32_t v) {
    out->push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
    out->push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
    out->push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    out->push_back(static_cast<uint8_t>(v & 0xFF));
}

uint16_t ReadBE16(const uint8_t* p) {
    return (static_cast<uint16_t>(p[0]) << 8) | static_cast<uint16_t>(p[1]);
}

uint32_t ReadBE32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) |
           static_cast<uint32_t>(p[3]);
}

// Resolve a host:port pair to a UdpAddr (IPv4 preferred for STUN).
// Returns an empty UdpAddr on failure. Synchronous — caller invokes
// this once per server at request time, so the blocking is bounded.
UdpAddr ResolveHost(const std::string& host, uint16_t port) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo* res = nullptr;
    char port_str[16];
    std::snprintf(port_str, sizeof(port_str), "%u", port);
    if (getaddrinfo(host.c_str(), port_str, &hints, &res) != 0 || !res) {
        return {};
    }
    UdpAddr out;
    // Prefer IPv4 — the first AF_INET result wins; fall back to v6 if
    // none. Keeps the result reproducible regardless of getaddrinfo
    // ordering quirks across platforms.
    for (addrinfo* cur = res; cur; cur = cur->ai_next) {
        if (cur->ai_family == AF_INET) {
            auto* in4 = reinterpret_cast<sockaddr_in*>(cur->ai_addr);
            uint8_t ip4[4];
            std::memcpy(ip4, &in4->sin_addr.s_addr, 4);
            out = UdpAddr::FromIPv4(ip4, port);
            freeaddrinfo(res);
            return out;
        }
    }
    for (addrinfo* cur = res; cur; cur = cur->ai_next) {
        if (cur->ai_family == AF_INET6) {
            auto* in6 = reinterpret_cast<sockaddr_in6*>(cur->ai_addr);
            uint8_t ip6[16];
            std::memcpy(ip6, &in6->sin6_addr, 16);
            out = UdpAddr::FromIPv6(ip6, port);
            break;
        }
    }
    freeaddrinfo(res);
    return out;
}

}  // namespace

StunClient::StunClient() {
    socket_.OnReceive([this](const UdpAddr& src, std::vector<uint8_t> data) {
        OnDatagram(src, std::move(data));
    });
}

StunClient::StunClient(SendFn send) : send_(std::move(send)) {}

StunClient::~StunClient() {
    stopping_.store(true, std::memory_order_release);
    if (watcher_.joinable()) watcher_.join();
    socket_.Stop();
}

void StunClient::AddServer(const std::string& host, uint16_t port) {
    servers_.emplace_back(host, port);
}

void StunClient::SetServers(std::vector<std::pair<std::string, uint16_t>> servers) {
    servers_ = std::move(servers);
}

bool StunClient::Discover(std::chrono::seconds timeout, ResultCallback cb) {
    if (busy_.exchange(true, std::memory_order_acq_rel)) {
        return false;
    }
    if (servers_.empty()) {
        StunResult r;
        r.error_message = "STUN: no servers configured";
        cb(r);
        busy_.store(false, std::memory_order_release);
        return true;
    }

    // Lazy-bind the local UDP socket to an ephemeral port. We rebind on
    // each Discover so consecutive rounds get fresh source ports —
    // matches the RFC 5780 multi-test pattern for NAT behavior probing
    // (Phase C2 work, not active here but a clean foundation).
    // An external socket (SendFn) is the caller's to manage.
    if (!send_) socket_.Stop();
    if (!send_ && !socket_.Bind(0)) {
        StunResult r;
        r.error_message = "STUN: could not bind UDP socket";
        cb(r);
        busy_.store(false, std::memory_order_release);
        return true;
    }

    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        outstanding_.clear();
        responses_.clear();
        result_cb_ = std::move(cb);
        deadline_ = std::chrono::steady_clock::now() + timeout;
    }

    int sent = 0;
    for (const auto& [host, port] : servers_) {
        UdpAddr dest = ResolveHost(host, port);
        if (dest.empty()) {
            continue;
        }

        Outstanding o;
        if (!GenerateTxnId(o.txn_id)) {
            continue;
        }
        o.server_endpoint = host + ":" + std::to_string(port);
        o.server_addr = dest;

        // Build STUN BINDING REQUEST (20 bytes, no attributes).
        std::vector<uint8_t> msg;
        msg.reserve(20);
        WriteBE16(&msg, kStunBindingRequest);
        WriteBE16(&msg, 0);  // length
        WriteBE32(&msg, kStunMagicCookie);
        msg.insert(msg.end(), o.txn_id.begin(), o.txn_id.end());

        // Registered before sending: through an external socket the answer
        // can arrive on another thread before SendTo returns.
        const auto txn_id = o.txn_id;
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            outstanding_.push_back(std::move(o));
        }
        const bool ok = send_ ? send_(dest, msg.data(), msg.size())
                              : socket_.SendTo(dest, msg.data(), msg.size());
        if (ok) {
            sent++;
        } else {
            std::lock_guard<std::mutex> lock(state_mutex_);
            outstanding_.erase(std::remove_if(outstanding_.begin(), outstanding_.end(),
                                              [&](const Outstanding& x) { return x.txn_id == txn_id; }),
                               outstanding_.end());
        }
    }

    if (sent == 0) {
        StunResult r;
        r.error_message = "STUN: all server resolutions / sends failed";
        DispatchResult(r);
        return true;
    }

    // Watcher thread that fires the result with a timeout error if no
    // response is received within `timeout`. The result callback is fired at
    // most once — DispatchResult guards on that. The previous round's watcher
    // has already seen busy_ drop (or will within 100 ms).
    if (watcher_.joinable()) watcher_.join();
    watcher_ = std::thread([this, timeout]() { TimeoutWatcher(timeout); });
    return true;
}

void StunClient::OnDatagram(const UdpAddr& src, std::vector<uint8_t> data) {
    HandleDatagram(src, data.data(), data.size());
}

std::optional<UdpAddr> StunClient::ParseBindingResponse(const uint8_t* data, size_t len) {
    if (!IsStunPacket(data, len)) return std::nullopt;
    if (ReadBE16(data) != kStunBindingSuccess) return std::nullopt;
    const size_t end = 20 + static_cast<size_t>(ReadBE16(data + 2));
    if (end > len) return std::nullopt;
    const uint8_t* txn = data + 8;

    size_t offset = 20;
    while (offset + 4 <= end) {
        const uint16_t attr_type = ReadBE16(data + offset);
        const uint16_t attr_len = ReadBE16(data + offset + 2);
        offset += 4;
        if (offset + attr_len > end) return std::nullopt;

        if (attr_type == kStunAttrXorMappedAddress && attr_len >= 8) {
            const uint8_t family = data[offset + 1];
            // X-Port XOR with first 2 bytes of magic cookie (BE).
            const uint16_t port = ReadBE16(data + offset + 2) ^
                                  static_cast<uint16_t>(kStunMagicCookie >> 16);
            if (family == 0x01) {
                // IPv4: X-Address XOR with full magic cookie (BE).
                const uint32_t addr = ReadBE32(data + offset + 4) ^ kStunMagicCookie;
                const uint8_t ip4[4] = {
                    static_cast<uint8_t>((addr >> 24) & 0xFF),
                    static_cast<uint8_t>((addr >> 16) & 0xFF),
                    static_cast<uint8_t>((addr >> 8) & 0xFF),
                    static_cast<uint8_t>(addr & 0xFF),
                };
                return UdpAddr::FromIPv4(ip4, port);
            }
            if (family == 0x02 && attr_len >= 20) {
                // IPv6: X-Address XOR with magic cookie || txn_id (16 bytes).
                uint8_t mask[16];
                for (int i = 0; i < 4; i++) mask[i] = data[4 + i];
                std::memcpy(mask + 4, txn, 12);
                uint8_t ip6[16];
                for (int i = 0; i < 16; i++) ip6[i] = data[offset + 4 + i] ^ mask[i];
                return UdpAddr::FromIPv6(ip6, port);
            }
            return std::nullopt;
        }

        // Attribute lengths are padded to 4-byte boundaries on the wire.
        offset += attr_len;
        if (attr_len % 4) offset += 4 - (attr_len % 4);
    }
    // Decoded, but no XOR-MAPPED-ADDRESS -- a broken server.
    return std::nullopt;
}

void StunClient::HandleDatagram(const UdpAddr& /*src*/, const uint8_t* data, size_t len) {
    const auto mapped = ParseBindingResponse(data, len);
    if (!mapped) return;
    std::array<uint8_t, 12> txn{};
    std::memcpy(txn.data(), data + 8, 12);

    bool all_answered = false;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        auto it = std::find_if(outstanding_.begin(), outstanding_.end(),
                               [&](const Outstanding& o) { return o.txn_id == txn && !o.fulfilled; });
        if (it == outstanding_.end()) return;  // unknown / duplicate / spoofed
        it->fulfilled = true;
        if (responses_.empty()) first_server_endpoint_ = it->server_endpoint;
        responses_.emplace_back(it->server_addr, *mapped);
        all_answered = std::all_of(outstanding_.begin(), outstanding_.end(),
                                   [](const Outstanding& o) { return o.fulfilled; });
    }
    if (all_answered) DispatchResult(StunResult{});
}

void StunClient::TimeoutWatcher(std::chrono::seconds timeout) {
    auto end = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < end) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (!busy_.load(std::memory_order_acquire)) return;  // already dispatched
        if (stopping_.load(std::memory_order_acquire)) return;  // being destroyed
    }
    StunResult r;
    r.error_message = "STUN: all servers timed out after " +
                      std::to_string(timeout.count()) + "s";
    DispatchResult(r);
}

void StunClient::DispatchResult(const StunResult& result) {
    StunResult out = result;
    ResultCallback cb;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (!result_cb_) return;  // already dispatched (race with timeout)
        cb = std::move(result_cb_);
        result_cb_ = nullptr;
        out.responses = responses_;
        if (!responses_.empty()) {
            out.public_addr = responses_.front().second;
            out.server_endpoint = first_server_endpoint_;
            out.error_message.clear();
        }
    }
    busy_.store(false, std::memory_order_release);
    if (cb) cb(out);
}

}  // namespace dinero::network
