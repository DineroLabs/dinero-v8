// Copyright (c) 2026 The Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
//
// Regression tests for QuicSession stream delivery under loss and bursts.
// ngtcp2 references (does not copy) the bytes passed to
// ngtcp2_conn_writev_stream and re-reads them when a lost STREAM frame is
// retransmitted, so QuicSession must keep sent bytes until ngtcp2 reports
// them acknowledged. A send_buffer reused per payload made a retransmit of
// payload k carry payload k+1's bytes (storage reuse) or freed memory
// (reallocation).
//
// Method: loopback client(A)/server(B) pair with a controllable A->B filter.
// After the handshake settles, arm the filter, then drop exactly one A->B
// 1-RTT (short-header) datagram that is large enough to carry stream data.
// Assert B's decrypted stream is byte-identical to what A sent.

#include "network/quic_session.h"
#include "network/quic_transport.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

using dinero::network::QuicSession;

constexpr auto kPrivateKey = R"(-----BEGIN PRIVATE KEY-----
MIGHAgEAMBMGByqGSM49AgEGCCqGSM49AwEHBG0wawIBAQQgwEvkGGgXAcRaG7Z8
gA7C6+W2RsW9gcjV9e5ybr0ikaahRANCAASCo35bDi+Q/q/CzHI1e5QaBrbqbFhW
G20QbVAeMK8l0oC8OGD3PSpZK1HXwALwzhMuwhxDos3ANb5naa5y17fQ
-----END PRIVATE KEY-----
)";

constexpr auto kCertificate = R"(-----BEGIN CERTIFICATE-----
MIICBzCCAa2gAwIBAgIUd2l6Pce3S0QH3dQC0Q/CjHbmggowCgYIKoZIzj0EAwIw
WTELMAkGA1UEBhMCQVUxEzARBgNVBAgMClNvbWUtU3RhdGUxITAfBgNVBAoMGElu
dGVybmV0IFdpZGdpdHMgUHR5IEx0ZDESMBAGA1UEAwwJbG9jYWxob3N0MB4XDTI1
MTExNDExNTcwMFoXDTI1MTIxNDExNTcwMFowWTELMAkGA1UEBhMCQVUxEzARBgNV
BAgMClNvbWUtU3RhdGUxITAfBgNVBAoMGEludGVybmV0IFdpZGdpdHMgUHR5IEx0
ZDESMBAGA1UEAwwJbG9jYWxob3N0MFkwEwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAE
gqN+Ww4vkP6vwsxyNXuUGga26mxYVhttEG1QHjCvJdKAvDhg9z0qWStR18AC8M4T
LsIcQ6LNwDW+Z2mucte30KNTMFEwHQYDVR0OBBYEFFVgXLoLwzpf6+twP5z8Ujr2
5mxnMB8GA1UdIwQYMBaAFFVgXLoLwzpf6+twP5z8Ujr25mxnMA8GA1UdEwEB/wQF
MAMBAf8wCgYIKoZIzj0EAwIDSAAwRQIhAO4tnDNRAcooz62vf2m7vTyDqFCjcaIv
SJ9Gq0lvEXEcAiBwWBNUASBqLaje3hmtgwxcF7EIqqiGo5j8f9Ufgu6SRg==
-----END CERTIFICATE-----
)";

dinero::network::UdpAddr Localhost(uint16_t port) {
    const uint8_t ip[4] = {127, 0, 0, 1};
    return dinero::network::UdpAddr::FromIPv4(ip, port);
}

dinero::network::QuicSessionOptions TestOptions() {
    dinero::network::QuicSessionOptions options;
    options.alpn = "dinero-relay-test/1";
    options.server_name = "localhost";
    options.certificate_pem = kCertificate;
    options.private_key_pem = kPrivateKey;
    options.verify_peer = false;
    return options;
}

// Datagrams at least this large carry STREAM data (ACK-only / control
// packets are tens of bytes). PMTUD probes are also large, which is why the
// filter is armed only after a settle period and every packet is logged.
constexpr size_t kStreamPacketMinSize = 1000;

struct PacketLogEntry {
    size_t index;
    size_t size;
    uint8_t first_byte;
    bool dropped;
};

// Same ownership/teardown discipline as QuicLoopbackPair, plus an A->B
// (client->server) drop filter. B->A is never filtered so ACKs flow.
class FilteredPair {
    struct Routing {
        std::mutex mutex;
        QuicSession* client{nullptr};
        QuicSession* server{nullptr};

        // Filter state (guarded by mutex).
        bool armed{false};
        int drop_nth_big_short{0};  // 0 = no drop; N = drop the Nth big short-header packet
        int big_short_seen{0};
        int drops{0};
        size_t post_arm_index{0};
        std::vector<PacketLogEntry> log;

        void deliver(bool to_server, std::vector<uint8_t> bytes) {
            std::lock_guard<std::mutex> lock(mutex);
            if (to_server && armed) {
                const bool short_header = !bytes.empty() && (bytes[0] & 0x80) == 0;
                bool drop = false;
                if (short_header && bytes.size() >= kStreamPacketMinSize) {
                    ++big_short_seen;
                    if (drop_nth_big_short > 0 && big_short_seen == drop_nth_big_short) {
                        drop = true;
                    }
                }
                log.push_back({post_arm_index++, bytes.size(),
                               bytes.empty() ? uint8_t{0} : bytes[0], drop});
                if (drop) {
                    ++drops;
                    return;
                }
            }
            auto* target = to_server ? server : client;
            if (target) target->EnqueueIncomingPacket(std::move(bytes));
        }
    };
    std::shared_ptr<Routing> routing_ = std::make_shared<Routing>();

public:
    std::unique_ptr<QuicSession> client;  // A
    std::unique_ptr<QuicSession> server;  // B

    FilteredPair() {
        server = std::make_unique<QuicSession>([route = routing_](std::vector<uint8_t> bytes) {
            route->deliver(false, std::move(bytes));
        });
        client = std::make_unique<QuicSession>([route = routing_](std::vector<uint8_t> bytes) {
            route->deliver(true, std::move(bytes));
        });
        std::lock_guard<std::mutex> lock(routing_->mutex);
        routing_->client = client.get();
        routing_->server = server.get();
    }

    void Arm(int drop_nth_big_short) {
        std::lock_guard<std::mutex> lock(routing_->mutex);
        routing_->drop_nth_big_short = drop_nth_big_short;
        routing_->armed = true;
    }

    int Drops() {
        std::lock_guard<std::mutex> lock(routing_->mutex);
        return routing_->drops;
    }

    std::string LogString() {
        std::lock_guard<std::mutex> lock(routing_->mutex);
        std::ostringstream os;
        for (const auto& e : routing_->log) {
            char buf[96];
            std::snprintf(buf, sizeof(buf), "  A->B #%zu size=%zu b0=0x%02x%s\n",
                          e.index, e.size, e.first_byte, e.dropped ? "  <== DROPPED" : "");
            os << buf;
        }
        return os.str();
    }

    ~FilteredPair() {
        {
            std::lock_guard<std::mutex> lock(routing_->mutex);
            routing_->client = nullptr;
            routing_->server = nullptr;
        }
        client->Close();
        server->Close();
    }
};

// Self-describing payload: 8-byte blocks
//   {idx, 0xA5, off>>24, off>>16, off>>8, off, idx^0x5A, 0xC3}
// where off is the block's byte offset inside payload idx.
std::vector<uint8_t> MakePayload(uint8_t idx, size_t size) {
    std::vector<uint8_t> out(size);
    for (size_t i = 0; i < size; ++i) {
        const uint32_t off = static_cast<uint32_t>(i & ~size_t{7});
        uint8_t b = 0;
        switch (i & 7) {
            case 0: b = idx; break;
            case 1: b = 0xA5; break;
            case 2: b = static_cast<uint8_t>(off >> 24); break;
            case 3: b = static_cast<uint8_t>(off >> 16); break;
            case 4: b = static_cast<uint8_t>(off >> 8); break;
            case 5: b = static_cast<uint8_t>(off); break;
            case 6: b = static_cast<uint8_t>(idx ^ 0x5A); break;
            case 7: b = 0xC3; break;
        }
        out[i] = b;
    }
    return out;
}

std::string Hex(const std::vector<uint8_t>& v, size_t from, size_t n) {
    std::ostringstream os;
    for (size_t i = from; i < from + n && i < v.size(); ++i) {
        char buf[4];
        std::snprintf(buf, sizeof(buf), "%02x", v[i]);
        os << buf << ((i + 1 - from) % 8 == 0 ? " " : "");
    }
    return os.str();
}

// Decode the 8-byte block of `v` starting at aligned stream offset `pos`.
std::string DecodeBlock(const std::vector<uint8_t>& v, size_t pos) {
    if (pos + 8 > v.size()) return "(partial block)";
    const uint8_t* b = v.data() + pos;
    if (b[1] == 0xA5 && b[7] == 0xC3 && b[6] == static_cast<uint8_t>(b[0] ^ 0x5A)) {
        const uint32_t off = (uint32_t{b[2]} << 24) | (uint32_t{b[3]} << 16) |
                             (uint32_t{b[4]} << 8) | uint32_t{b[5]};
        return "payload#" + std::to_string(b[0]) + " offset " + std::to_string(off);
    }
    return "NOT a payload block (foreign/garbage bytes)";
}

struct PayloadLayout {
    std::vector<std::vector<uint8_t>> payloads;
    std::vector<size_t> starts;  // stream offset of each payload
    std::vector<uint8_t> concat;
};

PayloadLayout BuildPayloads(const std::vector<size_t>& sizes) {
    PayloadLayout l;
    for (size_t i = 0; i < sizes.size(); ++i) {
        l.starts.push_back(l.concat.size());
        l.payloads.push_back(MakePayload(static_cast<uint8_t>(i + 1), sizes[i]));
        l.concat.insert(l.concat.end(), l.payloads.back().begin(), l.payloads.back().end());
    }
    return l;
}

// Read from B until `target` total bytes are accumulated or deadline.
void ReadUntil(QuicSession& b, std::vector<uint8_t>& got, size_t target,
               std::chrono::steady_clock::time_point deadline) {
    while (got.size() < target && std::chrono::steady_clock::now() < deadline) {
        auto chunk = b.ReadDecryptedStream(std::chrono::milliseconds(100));
        got.insert(got.end(), chunk.begin(), chunk.end());
    }
}

// Byte-compare; on mismatch, ADD_FAILURE with a localized report.
void CompareStreams(const PayloadLayout& l, const std::vector<uint8_t>& got) {
    ASSERT_EQ(got.size(), l.concat.size())
        << "B received a different byte count than A sent (stall/truncation, "
           "not corruption)";
    size_t first = got.size();
    for (size_t i = 0; i < got.size(); ++i) {
        if (got[i] != l.concat[i]) { first = i; break; }
    }
    if (first == got.size()) return;  // identical
    size_t last = first;
    size_t bad = 0;
    for (size_t i = first; i < got.size(); ++i) {
        if (got[i] != l.concat[i]) { last = i; ++bad; }
    }
    size_t pidx = 0;
    while (pidx + 1 < l.starts.size() && l.starts[pidx + 1] <= first) ++pidx;
    const size_t aligned = l.starts[pidx] + ((first - l.starts[pidx]) & ~size_t{7});
    std::ostringstream os;
    os << "STREAM CORRUPTION: first mismatch at stream offset " << first
       << " (payload#" << (pidx + 1) << " offset " << (first - l.starts[pidx]) << ")"
       << ", last mismatch at " << last << ", mismatched bytes=" << bad
       << " of " << got.size() << "\n"
       << "  expected @" << aligned << ": " << Hex(l.concat, aligned, 32) << "\n"
       << "  received @" << aligned << ": " << Hex(got, aligned, 32) << "\n"
       << "  expected block decodes as: " << DecodeBlock(l.concat, aligned) << "\n"
       << "  received block decodes as: " << DecodeBlock(got, aligned) << "\n";
    // Summarize received-origin of every mismatched aligned block.
    std::string prev;
    size_t run_start = 0;
    for (size_t p = aligned; p + 8 <= got.size() && p <= last; p += 8) {
        bool differs = false;
        for (size_t k = 0; k < 8; ++k) differs |= got[p + k] != l.concat[p + k];
        const std::string d = differs ? DecodeBlock(got, p) : std::string("ok");
        const std::string kind = d.rfind("payload#", 0) == 0
                                     ? d.substr(0, d.find(' '))
                                     : d;
        if (kind != prev) {
            if (!prev.empty())
                os << "    stream[" << run_start << ".." << p << ") received " << prev << "\n";
            prev = kind;
            run_start = p;
        }
    }
    if (!prev.empty())
        os << "    stream[" << run_start << ".." << last + 1 << ") received " << prev << "\n";
    ADD_FAILURE() << os.str();
}

void HandshakeAndSettle(FilteredPair& pair) {
    ASSERT_TRUE(pair.server->StartServer(Localhost(0), Localhost(0), TestOptions()));
    ASSERT_TRUE(pair.client->StartClient(Localhost(0), Localhost(0), TestOptions()));
    auto a = pair.client->WaitHandshakeReady();
    auto b = pair.server->WaitHandshakeReady();
    ASSERT_EQ(a.wait_for(std::chrono::seconds(5)), std::future_status::ready);
    ASSERT_EQ(b.wait_for(std::chrono::seconds(5)), std::future_status::ready);
    ASSERT_TRUE(a.get());
    ASSERT_TRUE(b.get());
    // Let post-handshake chatter (HANDSHAKE_DONE, NEW_CONNECTION_ID, ACKs,
    // PMTUD probes) settle so the first big post-arm packet is stream data.
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
}

void PrecheckBuild() {
    const auto info = dinero::network::QuicTransport::CompileInfo();
    ASSERT_TRUE(info.ngtcp2_available);
    ASSERT_TRUE(info.crypto_available) << info.disabled_reason;
}

// Burst: enqueue all payloads back-to-back without waiting.
void RunBurst(const std::vector<size_t>& sizes, int drop_nth_big_short, int expected_drops) {
    PrecheckBuild();
    FilteredPair pair;
    HandshakeAndSettle(pair);
    if (::testing::Test::HasFatalFailure()) return;

    const auto layout = BuildPayloads(sizes);
    pair.Arm(drop_nth_big_short);
    for (const auto& p : layout.payloads) pair.client->EnqueueOutgoingStream(p, false);

    std::vector<uint8_t> got;
    ReadUntil(*pair.server, got, layout.concat.size(),
              std::chrono::steady_clock::now() + std::chrono::seconds(5));
    std::cout << "[retransmit-test] post-arm A->B packets:\n" << pair.LogString()
              << "[retransmit-test] sent=" << layout.concat.size()
              << " received=" << got.size() << " drops=" << pair.Drops() << std::endl;
    ASSERT_EQ(pair.Drops(), expected_drops) << "filter did not drop as intended";
    CompareStreams(layout, got);
}

// Serialized: enqueue one payload, wait until B has all of it, then next.
void RunSerialized(const std::vector<size_t>& sizes, int drop_nth_big_short, int expected_drops) {
    PrecheckBuild();
    FilteredPair pair;
    HandshakeAndSettle(pair);
    if (::testing::Test::HasFatalFailure()) return;

    const auto layout = BuildPayloads(sizes);
    pair.Arm(drop_nth_big_short);
    std::vector<uint8_t> got;
    for (size_t i = 0; i < layout.payloads.size(); ++i) {
        pair.client->EnqueueOutgoingStream(layout.payloads[i], false);
        const size_t target = layout.starts[i] + layout.payloads[i].size();
        ReadUntil(*pair.server, got, target,
                  std::chrono::steady_clock::now() + std::chrono::seconds(5));
        ASSERT_GE(got.size(), target) << "B never received payload#" << (i + 1)
                                      << "\n" << pair.LogString();
    }
    std::cout << "[retransmit-test] post-arm A->B packets:\n" << pair.LogString()
              << "[retransmit-test] sent=" << layout.concat.size()
              << " received=" << got.size() << " drops=" << pair.Drops() << std::endl;
    ASSERT_EQ(pair.Drops(), expected_drops) << "filter did not drop as intended";
    CompareStreams(layout, got);
}

// Mixed sizes: 2000 after 3000 reuses storage; 5000 and 7000 reallocate.
const std::vector<size_t> kMixedSizes = {3000, 2000, 5000, 1300, 7000, 900};
// Every payload <= the first: send_buffer storage is reused, never freed.
const std::vector<size_t> kShrinkingSizes = {3000, 2000, 2500, 1300, 2900, 900};
// Every payload larger than its predecessor: each assignment reallocates
// and frees the storage the in-flight frames still point into.
const std::vector<size_t> kGrowingSizes = {2600, 3400, 4200, 5000, 5800, 6600};

}  // namespace

// Burst + one dropped stream packet.
TEST(QuicSessionRetransmit, BurstWithOneDroppedStreamPacketDeliversIdenticalBytes) {
    RunBurst(kMixedSizes, /*drop_nth_big_short=*/1, /*expected_drops=*/1);
}

// Storage-reuse path only.
TEST(QuicSessionRetransmit, BurstDropReusePathDeliversIdenticalBytes) {
    RunBurst(kShrinkingSizes, 1, 1);
}

// Reallocation (freed memory) path only.
TEST(QuicSessionRetransmit, BurstDropReallocPathDeliversIdenticalBytes) {
    RunBurst(kGrowingSizes, 1, 1);
}

// Control: same burst, no drop.
TEST(QuicSessionRetransmit, ControlBurstNoDropDeliversIdenticalBytes) {
    RunBurst(kMixedSizes, /*drop_nth_big_short=*/0, /*expected_drops=*/0);
}

// Control: same drop, serialized sends (the lost frame is retransmitted
// before the next payload is queued).
TEST(QuicSessionRetransmit, ControlSerializedWithDropDeliversIdenticalBytes) {
    RunSerialized(kMixedSizes, /*drop_nth_big_short=*/1, /*expected_drops=*/1);
}

// No drop, payloads enqueued with a short pause between them, so the
// producer races the session thread. A payload that cannot be queued yet
// used to be requeued at the BACK of the live outgoing_streams deque, where
// a payload enqueued after the swap overtook it (whole-payload reordering).
// Timing-dependent; run with --gtest_repeat.
TEST(QuicSessionRetransmit, PacedEnqueueNoDropPreservesPayloadOrder) {
    PrecheckBuild();
    FilteredPair pair;
    HandshakeAndSettle(pair);
    if (::testing::Test::HasFatalFailure()) return;
    const auto layout = BuildPayloads(kMixedSizes);
    pair.Arm(0);
    for (const auto& p : layout.payloads) {
        pair.client->EnqueueOutgoingStream(p, false);
        const auto until = std::chrono::steady_clock::now() + std::chrono::microseconds(50);
        while (std::chrono::steady_clock::now() < until) {}
    }
    std::vector<uint8_t> got;
    ReadUntil(*pair.server, got, layout.concat.size(),
              std::chrono::steady_clock::now() + std::chrono::seconds(5));
    ASSERT_EQ(pair.Drops(), 0);
    CompareStreams(layout, got);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
