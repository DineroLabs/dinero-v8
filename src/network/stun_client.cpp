// Copyright (c) 2026 The Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "network/stun_client.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <random>
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

// Generate a 12-byte random transaction id using mt19937. Not
// cryptographically critical — the goal is just to disambiguate
// concurrent in-flight requests, and the magic cookie + server
// response handling are the actual spoofing defenses.
void GenerateTxnId(std::array<uint8_t, 12>& out) {
    std::mt19937_64 rng{static_cast<uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count())};
    uint64_t a = rng();
    uint64_t b = rng();
    for (int i = 0; i < 8; i++) {
        out[i] = static_cast<uint8_t>((a >> (i * 8)) & 0xFF);
    }
    for (int i = 0; i < 4; i++) {
        out[8 + i] = static_cast<uint8_t>((b >> (i * 8)) & 0xFF);
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

StunClient::~StunClient() {
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
    socket_.Stop();
    if (!socket_.Bind(0)) {
        StunResult r;
        r.error_message = "STUN: could not bind UDP socket";
        cb(r);
        busy_.store(false, std::memory_order_release);
        return true;
    }

    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        outstanding_.clear();
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
        GenerateTxnId(o.txn_id);
        o.server_endpoint = host + ":" + std::to_string(port);

        // Build STUN BINDING REQUEST (20 bytes, no attributes).
        std::vector<uint8_t> msg;
        msg.reserve(20);
        WriteBE16(&msg, kStunBindingRequest);
        WriteBE16(&msg, 0);  // length
        WriteBE32(&msg, kStunMagicCookie);
        msg.insert(msg.end(), o.txn_id.begin(), o.txn_id.end());

        if (socket_.SendTo(dest, msg.data(), msg.size())) {
            {
                std::lock_guard<std::mutex> lock(state_mutex_);
                outstanding_.push_back(std::move(o));
            }
            sent++;
        }
    }

    if (sent == 0) {
        StunResult r;
        r.error_message = "STUN: all server resolutions / sends failed";
        DispatchResult(r);
        return true;
    }

    // Detached watcher thread that fires the result with a timeout
    // error if no response is received within `timeout`. The result
    // callback is fired at most once — DispatchResult guards on that.
    std::thread([this, timeout]() { TimeoutWatcher(timeout); }).detach();
    return true;
}

void StunClient::OnDatagram(const UdpAddr& src, std::vector<uint8_t> data) {
    // Minimum response: 20-byte header + 12-byte XOR-MAPPED-ADDRESS attr
    // (4-byte TLV header + 8-byte IPv4 value) = 32 bytes.
    if (data.size() < 32) return;
    if (ReadBE16(data.data()) != kStunBindingSuccess) return;
    const uint16_t msg_len = ReadBE16(data.data() + 2);
    if (msg_len < 12 || static_cast<size_t>(msg_len) + 20 > data.size()) return;
    if (ReadBE32(data.data() + 4) != kStunMagicCookie) return;

    // Match transaction id against an outstanding request.
    std::array<uint8_t, 12> txn{};
    std::memcpy(txn.data(), data.data() + 8, 12);

    std::string server_endpoint;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        for (auto& o : outstanding_) {
            if (o.txn_id == txn && !o.fulfilled) {
                o.fulfilled = true;
                server_endpoint = o.server_endpoint;
                break;
            }
        }
    }
    if (server_endpoint.empty()) return;  // unknown / duplicate / spoofed

    // Parse attributes looking for XOR-MAPPED-ADDRESS.
    size_t offset = 20;
    while (offset + 4 <= data.size()) {
        const uint16_t attr_type = ReadBE16(data.data() + offset);
        const uint16_t attr_len = ReadBE16(data.data() + offset + 2);
        offset += 4;
        if (offset + attr_len > data.size()) break;

        if (attr_type == kStunAttrXorMappedAddress && attr_len >= 8) {
            const uint8_t family = data[offset + 1];
            const uint16_t x_port = ReadBE16(data.data() + offset + 2);
            // X-Port XOR with first 2 bytes of magic cookie (BE).
            const uint16_t port = x_port ^ static_cast<uint16_t>(kStunMagicCookie >> 16);

            StunResult r;
            r.server_endpoint = server_endpoint;
            if (family == 0x01 && attr_len >= 8) {
                // IPv4: X-Address XOR with full magic cookie (BE).
                const uint32_t x_addr = ReadBE32(data.data() + offset + 4);
                const uint32_t addr = x_addr ^ kStunMagicCookie;
                uint8_t ip4[4] = {
                    static_cast<uint8_t>((addr >> 24) & 0xFF),
                    static_cast<uint8_t>((addr >> 16) & 0xFF),
                    static_cast<uint8_t>((addr >> 8) & 0xFF),
                    static_cast<uint8_t>(addr & 0xFF),
                };
                r.public_addr = UdpAddr::FromIPv4(ip4, port);
                DispatchResult(r);
                return;
            }
            if (family == 0x02 && attr_len >= 20) {
                // IPv6: X-Address XOR with magic cookie || txn_id (16 bytes).
                uint8_t cookie_and_txn[16];
                cookie_and_txn[0] = static_cast<uint8_t>((kStunMagicCookie >> 24) & 0xFF);
                cookie_and_txn[1] = static_cast<uint8_t>((kStunMagicCookie >> 16) & 0xFF);
                cookie_and_txn[2] = static_cast<uint8_t>((kStunMagicCookie >> 8) & 0xFF);
                cookie_and_txn[3] = static_cast<uint8_t>(kStunMagicCookie & 0xFF);
                std::memcpy(cookie_and_txn + 4, txn.data(), 12);

                uint8_t ip6[16];
                for (int i = 0; i < 16; i++) {
                    ip6[i] = data[offset + 4 + i] ^ cookie_and_txn[i];
                }
                r.public_addr = UdpAddr::FromIPv6(ip6, port);
                DispatchResult(r);
                return;
            }
        }

        // Attribute lengths are padded to 4-byte boundaries on the wire.
        offset += attr_len;
        if (attr_len % 4) offset += 4 - (attr_len % 4);
    }
    // Response decoded but no XOR-MAPPED-ADDRESS found — uncommon but
    // possible if the STUN server is broken. Move on to the next.
}

void StunClient::TimeoutWatcher(std::chrono::seconds timeout) {
    auto end = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < end) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (!busy_.load(std::memory_order_acquire)) return;  // already dispatched
    }
    StunResult r;
    r.error_message = "STUN: all servers timed out after " +
                      std::to_string(timeout.count()) + "s";
    DispatchResult(r);
}

void StunClient::DispatchResult(const StunResult& result) {
    ResultCallback cb;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (!result_cb_) return;  // already dispatched (race with timeout)
        cb = std::move(result_cb_);
        result_cb_ = nullptr;
    }
    busy_.store(false, std::memory_order_release);
    if (cb) cb(result);
}

}  // namespace dinero::network
