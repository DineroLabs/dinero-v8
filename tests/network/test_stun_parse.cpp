// STUN on the P2P UDP port: the binding-response parser (pinned to the RFC
// 5769 test vectors), the packet classifier the QuicEndpoint demuxes on, and a
// StunClient that sends through somebody else's socket and keeps every
// server's answer for NAT classification.

#include "network/stun_client.h"
#include "network/udp_socket.h"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <memory>
#include <thread>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <optional>
#include <vector>

namespace {

using dinero::network::StunClient;
using dinero::network::StunResult;
using dinero::network::UdpAddr;

// RFC 5769 §2.2: sample IPv4 response, mapped address 192.0.2.1:32853.
const std::vector<uint8_t> kRfc5769Ipv4 = {
    0x01, 0x01, 0x00, 0x3c, 0x21, 0x12, 0xa4, 0x42, 0xb7, 0xe7, 0xa7, 0x01,
    0xbc, 0x34, 0xd6, 0x86, 0xfa, 0x87, 0xdf, 0xae, 0x80, 0x22, 0x00, 0x0b,
    0x74, 0x65, 0x73, 0x74, 0x20, 0x76, 0x65, 0x63, 0x74, 0x6f, 0x72, 0x20,
    0x00, 0x20, 0x00, 0x08, 0x00, 0x01, 0xa1, 0x47, 0xe1, 0x12, 0xa6, 0x43,
    0x00, 0x08, 0x00, 0x14, 0x2b, 0x91, 0xf5, 0x99, 0xfd, 0x9e, 0x90, 0xc3,
    0x8c, 0x74, 0x89, 0xf9, 0x2a, 0xf9, 0xba, 0x53, 0xf0, 0x6b, 0xe7, 0xd7,
    0x80, 0x28, 0x00, 0x04, 0xc0, 0x7d, 0x4c, 0x96,
};

// RFC 5769 §2.3: sample IPv6 response, mapped address
// [2001:db8:1234:5678:11:2233:4455:6677]:32853.
const std::vector<uint8_t> kRfc5769Ipv6 = {
    0x01, 0x01, 0x00, 0x48, 0x21, 0x12, 0xa4, 0x42, 0xb7, 0xe7, 0xa7, 0x01,
    0xbc, 0x34, 0xd6, 0x86, 0xfa, 0x87, 0xdf, 0xae, 0x80, 0x22, 0x00, 0x0b,
    0x74, 0x65, 0x73, 0x74, 0x20, 0x76, 0x65, 0x63, 0x74, 0x6f, 0x72, 0x20,
    0x00, 0x20, 0x00, 0x14, 0x00, 0x02, 0xa1, 0x47, 0x01, 0x13, 0xa9, 0xfa,
    0xa5, 0xd3, 0xf1, 0x79, 0xbc, 0x25, 0xf4, 0xb5, 0xbe, 0xd2, 0xb9, 0xd9,
    0x00, 0x08, 0x00, 0x14, 0xa3, 0x82, 0x95, 0x4e, 0x4b, 0xe6, 0x7b, 0xf1,
    0x17, 0x84, 0xc9, 0x7c, 0x82, 0x92, 0xc2, 0x75, 0xbf, 0xe3, 0xed, 0x41,
    0x80, 0x28, 0x00, 0x04, 0xc8, 0xfb, 0x0b, 0x4c,
};

UdpAddr V4(uint8_t a, uint8_t b, uint8_t c, uint8_t d, uint16_t port) {
    const uint8_t ip[4] = {a, b, c, d};
    return UdpAddr::FromIPv4(ip, port);
}

// A binding success for `txn` reporting `mapped` (IPv4) in XOR-MAPPED-ADDRESS.
std::vector<uint8_t> Response(const uint8_t* txn, const UdpAddr& mapped) {
    std::vector<uint8_t> m = {0x01, 0x01, 0x00, 0x0c, 0x21, 0x12, 0xa4, 0x42};
    m.insert(m.end(), txn, txn + 12);
    const uint16_t xport = mapped.port ^ 0x2112;
    m.insert(m.end(), {0x00, 0x20, 0x00, 0x08, 0x00, 0x01,
                       static_cast<uint8_t>(xport >> 8), static_cast<uint8_t>(xport)});
    const uint8_t cookie[4] = {0x21, 0x12, 0xa4, 0x42};
    for (int i = 0; i < 4; ++i) m.push_back(mapped.ip[i] ^ cookie[i]);
    return m;
}

// Captures what a StunClient sends instead of putting it on a socket.
struct FakeWire {
    std::mutex mu;
    std::vector<std::pair<UdpAddr, std::vector<uint8_t>>> sent;
    StunClient::SendFn Fn() {
        return [this](const UdpAddr& dst, const uint8_t* d, size_t n) {
            std::lock_guard<std::mutex> l(mu);
            sent.emplace_back(dst, std::vector<uint8_t>(d, d + n));
            return true;
        };
    }
};

struct Waiter {
    std::mutex mu;
    std::condition_variable cv;
    std::optional<StunResult> result;
    StunClient::ResultCallback Fn() {
        return [this](const StunResult& r) {
            std::lock_guard<std::mutex> l(mu);
            result = r;
            cv.notify_all();
        };
    }
    bool Wait(std::chrono::milliseconds d) {
        std::unique_lock<std::mutex> l(mu);
        return cv.wait_for(l, d, [&] { return result.has_value(); });
    }
};

}  // namespace

TEST(StunParse, Rfc5769Ipv4AndIpv6Vectors) {
    ASSERT_TRUE(StunClient::IsStunPacket(kRfc5769Ipv4.data(), kRfc5769Ipv4.size()));
    auto v4 = StunClient::ParseBindingResponse(kRfc5769Ipv4.data(), kRfc5769Ipv4.size());
    ASSERT_TRUE(v4.has_value());
    EXPECT_EQ(v4->to_string(), "192.0.2.1:32853");

    ASSERT_TRUE(StunClient::IsStunPacket(kRfc5769Ipv6.data(), kRfc5769Ipv6.size()));
    auto v6 = StunClient::ParseBindingResponse(kRfc5769Ipv6.data(), kRfc5769Ipv6.size());
    ASSERT_TRUE(v6.has_value());
    EXPECT_EQ(v6->to_string(), "[2001:db8:1234:5678:11:2233:4455:6677]:32853");
}

TEST(StunParse, RejectsWrongCookieAndTruncation) {
    auto bad_cookie = kRfc5769Ipv4;
    bad_cookie[4] ^= 0xFF;
    EXPECT_FALSE(StunClient::IsStunPacket(bad_cookie.data(), bad_cookie.size()));
    EXPECT_FALSE(StunClient::ParseBindingResponse(bad_cookie.data(), bad_cookie.size()));
    // Declared length runs past the datagram.
    EXPECT_FALSE(StunClient::ParseBindingResponse(kRfc5769Ipv4.data(), 40));
    EXPECT_FALSE(StunClient::IsStunPacket(kRfc5769Ipv4.data(), 19));
}

TEST(StunParse, QuicPacketsAreNeverStun) {
    // RFC 9000 fixed bit 0x40: every QUIC packet's first byte is >= 0x40.
    std::vector<uint8_t> quic(1200, 0);
    quic[0] = 0xC3;
    quic[4] = 0x21; quic[5] = 0x12; quic[6] = 0xa4; quic[7] = 0x42;  // cookie bytes too
    EXPECT_FALSE(StunClient::IsStunPacket(quic.data(), quic.size()));
    quic[0] = 0x41;  // short header
    EXPECT_FALSE(StunClient::IsStunPacket(quic.data(), quic.size()));
}

TEST(StunDemux, UnknownTxnIgnored) {
    FakeWire wire;
    StunClient client(wire.Fn());
    client.SetServers({{"192.0.2.10", 3478}});
    Waiter w;
    ASSERT_TRUE(client.Discover(std::chrono::seconds(2), w.Fn()));
    ASSERT_EQ(wire.sent.size(), 1u);

    uint8_t other_txn[12];
    std::memcpy(other_txn, wire.sent[0].second.data() + 8, 12);
    other_txn[0] ^= 0xFF;
    auto spoofed = Response(other_txn, V4(6, 6, 6, 6, 666));
    client.HandleDatagram(wire.sent[0].first, spoofed.data(), spoofed.size());

    ASSERT_TRUE(w.Wait(std::chrono::seconds(4)));
    EXPECT_TRUE(w.result->public_addr.empty()) << w.result->public_addr.to_string();
    EXPECT_TRUE(w.result->responses.empty());
}

TEST(StunDemux, EveryServerResponseKept) {
    FakeWire wire;
    StunClient client(wire.Fn());
    client.SetServers({{"192.0.2.10", 3478}, {"198.51.100.20", 3478}, {"203.0.113.30", 19302}});
    Waiter w;
    ASSERT_TRUE(client.Discover(std::chrono::seconds(5), w.Fn()));
    ASSERT_EQ(wire.sent.size(), 3u);
    const UdpAddr mapped = V4(162, 200, 227, 214, 45123);
    for (const auto& [dst, req] : wire.sent) {
        auto resp = Response(req.data() + 8, mapped);
        client.HandleDatagram(dst, resp.data(), resp.size());
    }
    // All three answered: the round closes without waiting out the timeout.
    ASSERT_TRUE(w.Wait(std::chrono::seconds(2)));
    EXPECT_EQ(w.result->public_addr.to_string(), "162.200.227.214:45123");
    ASSERT_EQ(w.result->responses.size(), 3u);
    for (size_t i = 0; i < 3; ++i) {
        EXPECT_EQ(w.result->responses[i].first.to_string(), wire.sent[i].first.to_string());
        EXPECT_EQ(w.result->responses[i].second.to_string(), "162.200.227.214:45123");
    }
}

TEST(StunDemux, DestroyedMidRoundNeverCallsBack) {
    // Shutdown can destroy the client while a round waits out its timeout (UDP
    // to STUN blocked: every round does). Nothing may touch it afterwards.
    std::atomic<int> fired{0};
    {
        FakeWire wire;
        auto client = std::make_unique<StunClient>(wire.Fn());
        client->SetServers({{"192.0.2.10", 3478}});
        ASSERT_TRUE(client->Discover(std::chrono::seconds(1), [&](const StunResult&) { fired++; }));
        client.reset();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    EXPECT_EQ(fired.load(), 0) << "timeout watcher ran on a destroyed client";
}

// Review of #852 (Codex): through an external socket a reply can arrive
// before Discover has registered the next server's request; the round then
// counted as complete with one observation and later replies were lost.
TEST(StunDemux, FastFirstReplyDoesNotEndATwoServerRound) {
    StunClient* client = nullptr;
    size_t delivered = 0;
    int callbacks = 0;
    StunClient c([&](const UdpAddr& server, const uint8_t* request, size_t) {
        auto response = Response(request + 8, V4(44, 55, 66, 77, 20999));
        client->HandleDatagram(server, response.data(), response.size());  // answers at once
        return true;
    });
    client = &c;
    c.SetServers({{"192.0.2.10", 3478}, {"198.51.100.10", 3478}});
    ASSERT_TRUE(c.Discover(std::chrono::seconds(1), [&](const StunResult& r) {
        ++callbacks;
        delivered = r.responses.size();
    }));
    EXPECT_EQ(callbacks, 1);
    EXPECT_EQ(delivered, 2u) << "the round ended before the second server was asked";
}
