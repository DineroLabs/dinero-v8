// QuicEndpoint: one UDP socket on the P2P port, demultiplexed into per-peer
// QuicSessions. These cases pin the properties a public UDP port needs: a
// session only ever hears from its own remote address, new sessions start only
// from real QUIC Initials, and nobody can make us create sessions without bound.

#include "network/quic_endpoint.h"
#include "network/quic_session.h"
#include "network/relay_tls_keypair.h"
#include "network/udp_socket.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <random>
#include <thread>
#include <vector>

namespace {

using dinero::network::QuicEndpoint;
using dinero::network::QuicSession;
using dinero::network::QuicSessionOptions;
using dinero::network::UdpAddr;
using dinero::network::UdpSocket;

QuicSessionOptions Opts() {
    static std::string cert, key;
    static std::once_flag once;
    std::call_once(once, [] { ASSERT_TRUE(dinero::network::GenerateRelayTlsKeypair(&cert, &key, nullptr)); });
    QuicSessionOptions o;
    o.alpn = "dinero-p2p/1";
    o.certificate_pem = cert;
    o.private_key_pem = key;
    return o;
}

UdpAddr Loopback(uint16_t port) {
    const uint8_t ip[4] = {127, 0, 0, 1};
    return UdpAddr::FromIPv4(ip, port);
}

// A datagram shaped like a QUIC v1 client Initial (long header, fixed bit,
// type 0, version 1, padded to 1200 bytes) but with random contents.
std::vector<uint8_t> FakeInitial(std::mt19937& rng) {
    std::vector<uint8_t> p(1200);
    for (auto& b : p) b = static_cast<uint8_t>(rng());
    p[0] = 0xC3;
    p[1] = 0; p[2] = 0; p[3] = 0; p[4] = 1;
    p[5] = 8;  // DCID length
    return p;
}

struct Server {
    QuicEndpoint ep;
    std::mutex mu;
    std::vector<std::shared_ptr<QuicSession>> inbound;
    std::atomic<int> inbound_count{0};
    std::atomic<bool> accept{true};
    bool Start() {
        return ep.Start(0, Opts(),
            [this](const UdpAddr&) { return accept.load(); },
            [this](std::shared_ptr<QuicSession> s, const UdpAddr&) {
                std::lock_guard<std::mutex> l(mu);
                inbound.push_back(std::move(s));
                inbound_count.fetch_add(1);
            });
    }
};

bool Ready(const std::shared_ptr<QuicSession>& s, int seconds = 5) {
    auto f = s->WaitHandshakeReady();
    return f.wait_for(std::chrono::seconds(seconds)) == std::future_status::ready && f.get();
}

}  // namespace

TEST(QuicEndpoint, ClientAndServerCompleteHandshakeAndExchangeStream) {
    Server server;
    ASSERT_TRUE(server.Start());
    QuicEndpoint client;
    ASSERT_TRUE(client.Start(0, Opts(), [](const UdpAddr&) { return false; },
                             [](std::shared_ptr<QuicSession>, const UdpAddr&) {}));

    auto session = client.Connect(Loopback(server.ep.Port()), Opts());
    ASSERT_NE(session, nullptr);
    ASSERT_TRUE(Ready(session));
    ASSERT_EQ(server.inbound_count.load(), 1);
    std::shared_ptr<QuicSession> peer;
    { std::lock_guard<std::mutex> l(server.mu); peer = server.inbound[0]; }
    ASSERT_TRUE(Ready(peer));

    std::vector<uint8_t> payload(64 * 1024);
    for (size_t i = 0; i < payload.size(); ++i) payload[i] = static_cast<uint8_t>(i * 31);
    session->EnqueueOutgoingStream(payload);
    std::vector<uint8_t> got;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (got.size() < payload.size() && std::chrono::steady_clock::now() < deadline) {
        auto chunk = peer->ReadDecryptedStream(std::chrono::milliseconds(100));
        got.insert(got.end(), chunk.begin(), chunk.end());
    }
    EXPECT_EQ(got, payload);
    client.Stop();
    server.ep.Stop();
}

TEST(QuicEndpoint, DropsDatagramFromWrongSourceForSession) {
    Server server;
    ASSERT_TRUE(server.Start());
    QuicEndpoint client;
    ASSERT_TRUE(client.Start(0, Opts(), [](const UdpAddr&) { return false; },
                             [](std::shared_ptr<QuicSession>, const UdpAddr&) {}));
    auto session = client.Connect(Loopback(server.ep.Port()), Opts());
    ASSERT_TRUE(Ready(session));
    const auto sessions_before = server.ep.SessionCount();
    const auto dropped_before = server.ep.Stats().datagrams_dropped;

    UdpSocket intruder;
    ASSERT_TRUE(intruder.Bind(0));
    std::mt19937 rng(7);
    std::vector<uint8_t> junk(200);
    for (auto& b : junk) b = static_cast<uint8_t>(rng());
    junk[0] = 0x40;  // short header: looks like 1-RTT data for an existing session
    for (int i = 0; i < 50; ++i) intruder.SendTo(Loopback(server.ep.Port()), junk.data(), junk.size());
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    // Every datagram is either routed or dropped; all 50 intruder datagrams
    // being dropped means none reached the session. (The real client keeps
    // sending ACKs meanwhile, so "routed" alone cannot show this.)
    EXPECT_EQ(server.ep.SessionCount(), sessions_before);
    EXPECT_GE(server.ep.Stats().datagrams_dropped - dropped_before, 50u)
        << "a datagram from an unknown source reached an existing session";
    intruder.Stop();
    client.Stop();
    server.ep.Stop();
}

TEST(QuicEndpoint, NonInitialFromUnknownSourceCreatesNoSession) {
    Server server;
    ASSERT_TRUE(server.Start());
    UdpSocket s;
    ASSERT_TRUE(s.Bind(0));
    std::vector<uint8_t> shorthdr(64, 0x11);
    shorthdr[0] = 0x41;
    s.SendTo(Loopback(server.ep.Port()), shorthdr.data(), shorthdr.size());
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    EXPECT_EQ(server.ep.SessionCount(), 0u);
    EXPECT_EQ(server.inbound_count.load(), 0);
    s.Stop();
    server.ep.Stop();
}

TEST(QuicEndpoint, InitialFloodIsRateLimitedPerIp) {
    Server server;
    ASSERT_TRUE(server.Start());
    std::mt19937 rng(42);
    std::vector<std::unique_ptr<UdpSocket>> flooders;
    for (int i = 0; i < 30; ++i) {  // 30 distinct source ports from one IP
        auto s = std::make_unique<UdpSocket>();
        ASSERT_TRUE(s->Bind(0));
        auto pkt = FakeInitial(rng);
        s->SendTo(Loopback(server.ep.Port()), pkt.data(), pkt.size());
        flooders.push_back(std::move(s));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    EXPECT_LE(server.inbound_count.load(), 5) << "per-IP Initial limit (4/s) not enforced";

    // A real dial from the same host still gets through once the bucket refills.
    QuicEndpoint client;
    ASSERT_TRUE(client.Start(0, Opts(), [](const UdpAddr&) { return false; },
                             [](std::shared_ptr<QuicSession>, const UdpAddr&) {}));
    auto session = client.Connect(Loopback(server.ep.Port()), Opts());
    EXPECT_TRUE(Ready(session, 10));
    for (auto& s : flooders) s->Stop();
    client.Stop();
    server.ep.Stop();
}

TEST(QuicEndpoint, AcceptFnRefusalCreatesNoSession) {
    Server server;
    server.accept = false;
    ASSERT_TRUE(server.Start());
    QuicEndpoint client;
    ASSERT_TRUE(client.Start(0, Opts(), [](const UdpAddr&) { return false; },
                             [](std::shared_ptr<QuicSession>, const UdpAddr&) {}));
    auto session = client.Connect(Loopback(server.ep.Port()), Opts());
    ASSERT_NE(session, nullptr);
    EXPECT_FALSE(Ready(session, 5));
    EXPECT_EQ(server.inbound_count.load(), 0);
    EXPECT_EQ(server.ep.SessionCount(), 0u);
    client.Stop();
    server.ep.Stop();
}
