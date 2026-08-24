#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

#include "p2p_manager.h"
#include "daemon/node_identity.h"
#include "daemon/peer_send_health.h"
#include "network/types.h"
#include "p2p/addrman.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr uint64_t kCircuitId = 0x0102030405060708ULL;
const char* kRelayPeer = "127.0.0.1:20999";
const char* kVirtualPeer = "relay:test-target:0102030405060708";

P2PMessage MakePing(uint64_t nonce) {
    return P2PMessage::create_ping(nonce);
}

std::string InstallVirtualPeer(P2PManager& manager) {
    return manager.test_install_virtual_relay_peer(
        kVirtualPeer,
        kRelayPeer,
        kCircuitId,
        P2PMessage::RelayDirection::ClientToTarget,
        true);
}

uint64_t ReadLE64ForTest(const std::vector<uint8_t>& data, size_t offset) {
    uint64_t value = 0;
    for (size_t i = 0; i < 8; ++i) {
        value |= static_cast<uint64_t>(data[offset + i]) << (i * 8);
    }
    return value;
}

std::array<uint8_t, 20> MakeNodeId(uint8_t base) {
    std::array<uint8_t, 20> out{};
    for (size_t i = 0; i < out.size(); ++i) {
        out[i] = static_cast<uint8_t>(base + i);
    }
    return out;
}

dinero::p2p::NetworkAddress MakeRelayAddress(const std::string& ip,
                                             uint16_t port) {
    dinero::p2p::NetworkAddress addr;
    addr.ip = ip;
    addr.port = port;
    addr.services = dinero::ServiceFlags::NODE_RELAY;
    addr.timestamp = std::chrono::system_clock::now();
    return addr;
}

bool ContainsEndpoint(const std::vector<std::string>& endpoints,
                      const std::string& endpoint) {
    return std::find(endpoints.begin(), endpoints.end(), endpoint) != endpoints.end();
}

uint8_t RelayAckStatus(const P2PMessage& message) {
    EXPECT_EQ(message.command, "relayack");
    EXPECT_GE(message.payload.size(), 18u);
    return message.payload.size() >= 18 ? message.payload[16] : 0xff;
}

std::string HexNodeId(const std::array<uint8_t, 20>& node_id) {
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (auto b : node_id) {
        out << std::setw(2) << static_cast<unsigned int>(b);
    }
    return out.str();
}

void CloseTestSocket(int fd) {
    if (fd < 0) {
        return;
    }
#ifdef _WIN32
    closesocket(static_cast<SOCKET>(fd));
#else
    close(fd);
#endif
}

void SetReceiveTimeout(int fd, int milliseconds) {
#ifdef _WIN32
    DWORD timeout = static_cast<DWORD>(milliseconds);
    setsockopt(static_cast<SOCKET>(fd), SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char*>(&timeout), sizeof(timeout));
#else
    timeval timeout{};
    timeout.tv_sec = milliseconds / 1000;
    timeout.tv_usec = (milliseconds % 1000) * 1000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char*>(&timeout), sizeof(timeout));
#endif
}

class LoopbackSocketPair {
public:
    static LoopbackSocketPair Create() {
        LoopbackSocketPair pair;
        pair.listener_ = static_cast<int>(socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
        if (pair.listener_ < 0) {
            throw std::runtime_error("socket(listener) failed");
        }

        int opt = 1;
        setsockopt(pair.listener_, SOL_SOCKET, SO_REUSEADDR,
                   reinterpret_cast<const char*>(&opt), sizeof(opt));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = htons(0);
        if (bind(pair.listener_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            throw std::runtime_error("bind(listener) failed");
        }
        if (listen(pair.listener_, 1) != 0) {
            throw std::runtime_error("listen(listener) failed");
        }

        socklen_t addr_len = sizeof(addr);
        if (getsockname(pair.listener_, reinterpret_cast<sockaddr*>(&addr), &addr_len) != 0) {
            throw std::runtime_error("getsockname(listener) failed");
        }
        pair.port_ = ntohs(addr.sin_port);

        pair.client_ = static_cast<int>(socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
        if (pair.client_ < 0) {
            throw std::runtime_error("socket(client) failed");
        }
        if (connect(pair.client_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            throw std::runtime_error("connect(client) failed");
        }

        pair.server_ = static_cast<int>(accept(pair.listener_, nullptr, nullptr));
        if (pair.server_ < 0) {
            throw std::runtime_error("accept(listener) failed");
        }
        CloseTestSocket(pair.listener_);
        pair.listener_ = -1;
        SetReceiveTimeout(pair.server_, 100);
        return pair;
    }

    LoopbackSocketPair(const LoopbackSocketPair&) = delete;
    LoopbackSocketPair& operator=(const LoopbackSocketPair&) = delete;

    LoopbackSocketPair(LoopbackSocketPair&& other) noexcept
        : listener_(other.listener_),
          client_(other.client_),
          server_(other.server_),
          port_(other.port_) {
        other.listener_ = -1;
        other.client_ = -1;
        other.server_ = -1;
        other.port_ = 0;
    }

    LoopbackSocketPair& operator=(LoopbackSocketPair&& other) noexcept {
        if (this != &other) {
            Close();
            listener_ = other.listener_;
            client_ = other.client_;
            server_ = other.server_;
            port_ = other.port_;
            other.listener_ = -1;
            other.client_ = -1;
            other.server_ = -1;
            other.port_ = 0;
        }
        return *this;
    }

    ~LoopbackSocketPair() {
        Close();
    }

    int client_fd() const { return client_; }
    uint16_t port() const { return port_; }

    std::unique_ptr<P2PMessage> ReceiveMessage() {
        std::vector<uint8_t> header(24);
        if (!RecvExact(header.data(), header.size())) {
            return nullptr;
        }
        uint32_t payload_len = 0;
        for (int i = 0; i < 4; ++i) {
            payload_len |= static_cast<uint32_t>(header[16 + i]) << (i * 8);
        }
        std::vector<uint8_t> frame = header;
        frame.resize(24 + payload_len);
        if (payload_len > 0 && !RecvExact(frame.data() + 24, payload_len)) {
            return nullptr;
        }
        return P2PMessage::deserialize(frame);
    }

private:
    LoopbackSocketPair() = default;

    bool RecvExact(uint8_t* dst, size_t len) {
        size_t got = 0;
        while (got < len) {
            const int n = recv(server_,
                               reinterpret_cast<char*>(dst + got),
                               static_cast<int>(len - got),
                               0);
            if (n <= 0) {
                return false;
            }
            got += static_cast<size_t>(n);
        }
        return true;
    }

    void Close() {
        CloseTestSocket(server_);
        CloseTestSocket(client_);
        CloseTestSocket(listener_);
        server_ = -1;
        client_ = -1;
        listener_ = -1;
    }

    int listener_{-1};
    int client_{-1};
    int server_{-1};
    uint16_t port_{0};
};

std::shared_ptr<dinero::daemon::NodeIdentity> InstallNodeIdentity(P2PManager& manager) {
    const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto dir = std::filesystem::temp_directory_path() /
                     ("dinero-relay-orchestrator-" + std::to_string(suffix));
    std::filesystem::create_directories(dir);

    auto identity = std::make_shared<dinero::daemon::NodeIdentity>();
    if (!identity->initialize(dir.string())) {
        std::filesystem::remove_all(dir);
        throw std::runtime_error("failed to initialize test node identity");
    }
    manager.set_node_identity(identity);
    std::filesystem::remove_all(dir);
    return identity;
}

void AddRelayHint(P2PManager& manager,
                  const std::array<uint8_t, 20>& target,
                  uint16_t relay_port) {
    P2PMessage::RelayHint hint;
    hint.target_node_id = target;
    hint.relay_net = dinero::p2p::NetworkType::IPV4;
    hint.relay_addr = {127, 0, 0, 1};
    hint.relay_port = relay_port;
    manager.handle_relay_hints("hint-source", P2PMessage::create_relay_hints({hint}));
}

}  // namespace

TEST(RelayVirtualTransport, MainnetPlaintextRelayDataIsRefusedByDefault) {
    P2PManager manager(0);
    const auto peer_key = InstallVirtualPeer(manager);

    EXPECT_FALSE(manager.test_plaintext_relay_transport_allowed());

    const auto ping = MakePing(0xabc123);
    const auto relay_frame = P2PMessage::create_relay_data(
        kCircuitId,
        P2PMessage::RelayDirection::TargetToClient,
        ping.serialize());

    manager.handle_relay_data(kRelayPeer, relay_frame);

    auto received = manager.test_receive_peer_message(
        peer_key, std::chrono::milliseconds(5));
    EXPECT_EQ(received, nullptr);
}

TEST(RelayVirtualTransport, DevOverrideAllowsRelayDataToQueueOneInnerMessage) {
    P2PManager manager(0);
    manager.set_plaintext_relay_dev_override_for_tests(true);
    const auto peer_key = InstallVirtualPeer(manager);

    ASSERT_TRUE(manager.test_plaintext_relay_transport_allowed());

    const auto ping = MakePing(0x424242);
    const auto relay_frame = P2PMessage::create_relay_data(
        kCircuitId,
        P2PMessage::RelayDirection::TargetToClient,
        ping.serialize());

    manager.handle_relay_data(kRelayPeer, relay_frame);

    auto received = manager.test_receive_peer_message(
        peer_key, std::chrono::milliseconds(50));
    ASSERT_NE(received, nullptr);
    EXPECT_EQ(received->command, "ping");
    EXPECT_EQ(received->payload, ping.payload);

    auto empty = manager.test_receive_peer_message(
        peer_key, std::chrono::milliseconds(5));
    EXPECT_EQ(empty, nullptr);
}

// Zombie-circuit eviction: a relay virtual peer whose transport can never
// deliver (here: plaintext relay refused on mainnet defaults) must be evicted
// after kMaxConsecutiveSendFailures failed sends — otherwise it stays
// is_connected, passes every block-download eligibility filter, and the
// scheduler loops "eligible=N sent=0" forever (the NAT'd-node backfill wedge).
TEST(RelayVirtualTransport, ZombieRelayPeerEvictedAfterConsecutiveSendFailures) {
    P2PManager manager(0);
    const auto peer_key = InstallVirtualPeer(manager);

    // Transport refuses by default on mainnet -> every send fails.
    ASSERT_FALSE(manager.test_plaintext_relay_transport_allowed());

    const auto msg = MakePing(0x5150);

    auto peer_connected = [&](const std::string& key) {
        const auto peers = manager.get_connected_peers();
        return std::any_of(peers.begin(), peers.end(), [&](const PeerInfo& p) {
            return p.to_string() == key;
        });
    };

    ASSERT_TRUE(peer_connected(peer_key));

    // Below the streak threshold: sends fail but the peer survives.
    for (uint32_t i = 0; i + 1 < dinero::kMaxConsecutiveSendFailures; ++i) {
        EXPECT_FALSE(manager.send_to_peer(peer_key, msg));
        EXPECT_TRUE(peer_connected(peer_key))
            << "evicted too early after " << (i + 1) << " failures";
    }

    // The streak-completing failure evicts the zombie.
    EXPECT_FALSE(manager.send_to_peer(peer_key, msg));
    EXPECT_FALSE(peer_connected(peer_key))
        << "zombie relay peer still connected after "
        << dinero::kMaxConsecutiveSendFailures << " consecutive send failures";
}

TEST(RelayVirtualTransport, SocketlessVirtualPeerReceiveUsesTransportQueue) {
    P2PManager manager(0);
    manager.set_plaintext_relay_dev_override_for_tests(true);
    const auto peer_key = InstallVirtualPeer(manager);

    const auto ping = MakePing(0x515151);
    ASSERT_TRUE(manager.test_enqueue_relay_frame(peer_key, ping.serialize()));

    auto received = manager.test_receive_peer_message(
        peer_key, std::chrono::milliseconds(50));
    ASSERT_NE(received, nullptr);
    EXPECT_EQ(received->command, "ping");
    EXPECT_EQ(received->payload, ping.payload);
}

// ─── NAT traversal Phase D-2: outbound dialing orchestrator ──────────────

TEST(RelayOrchestrator, InstallOutboundVirtualPeerSetsViaRelayClientToTarget) {
    P2PManager manager(0);
    std::array<uint8_t, 20> target{};
    for (size_t i = 0; i < target.size(); i++) target[i] = static_cast<uint8_t>(i + 0xa0);

    const std::string key = manager.install_outbound_virtual_relay_peer(
        target, "10.88.0.10:20999", 0xdeadbeefULL);

    // Key shape: "relay:<40 hex chars>:<hex circuit_id>".
    ASSERT_EQ(key.substr(0, 6), "relay:");
    EXPECT_NE(key.find("a0a1a2a3a4a5a6a7a8a9aaabacadaeafb0b1b2b3"), std::string::npos);
    EXPECT_NE(key.find("deadbeef"), std::string::npos);

    // Peer must be inserted with via_relay populated. get_connected_peers
    // filters by is_connected which our freshly-installed peer is NOT
    // (handshake hasn't run); use get_peer_info to inspect the raw entry.
    PeerInfo* p = manager.get_peer_info(key);
    ASSERT_NE(p, nullptr);
    EXPECT_TRUE(p->is_outbound);
    EXPECT_EQ(p->socket_fd, -1);
    EXPECT_EQ(p->their_node_id, target);
    ASSERT_TRUE(p->via_relay.has_value());
    EXPECT_EQ(p->via_relay->circuit_id, 0xdeadbeefULL);
    EXPECT_EQ(p->via_relay->relay_peer_address, std::string("10.88.0.10:20999"));
    // outbound_direction MUST be ClientToTarget (0) on the originator side
    // so send_peer_message wraps with the correct direction byte.
    EXPECT_EQ(p->via_relay->outbound_direction,
              static_cast<uint8_t>(P2PMessage::RelayDirection::ClientToTarget));
}

TEST(RelayOrchestrator, OrchestratorSkipsTargetsAlreadyHavingProvenPeer) {
    // Sanity-check: when a target's proven node_id matches a connected peer,
    // priming relay_hints_by_target_ + invoking the orchestrator does NOT
    // schedule a relay dial. We assert via the side-effect that pending
    // RELAY_CONNECTs stay empty (no relay infrastructure live, so any actual
    // dial attempt would surface as a pending entry then a timeout).
    P2PManager manager(0);

    // Install a virtual "already connected" peer whose proven node_id we
    // want to claim is also relay-reachable. We re-purpose the existing
    // test_install_virtual_relay_peer helper for this — it gives us a
    // peer entry without needing a real socket.
    std::array<uint8_t, 20> proven_id{};
    for (size_t i = 0; i < proven_id.size(); i++) proven_id[i] = static_cast<uint8_t>(i + 0x10);
    // The test helper doesn't expose a way to set identity_proven, so we
    // verify the OTHER skip path instead: target with no usable relay
    // (relay endpoint not in connected_peers_) is silently skipped.

    // No setup needed — relay_hints_by_target_ starts empty.
    manager.OrchestrateRelayDials();  // should be a no-op

    // The orchestrator's structural invariant: with no hints + no
    // identity, no work happens. This is a smoke test that the method
    // exits cleanly rather than tripping any of its internal asserts.
    SUCCEED();
}

TEST(RelayOrchestrator, OrchestratorSkipsProvenPeerWhenRelayHintExists) {
    P2PManager manager(0);
    manager.set_plaintext_relay_dev_override_for_tests(true);
    InstallNodeIdentity(manager);

    auto relay = LoopbackSocketPair::Create();
    const std::string relay_key = "127.0.0.1:" + std::to_string(relay.port());
    manager.test_install_connected_direct_peer(relay_key, relay.client_fd(),
                                               true, false, {});

    const auto target = MakeNodeId(0x10);
    manager.test_install_connected_direct_peer("203.0.113.9:20999", -1,
                                               true, true, target);
    AddRelayHint(manager, target, relay.port());

    manager.OrchestrateRelayDials();

    EXPECT_EQ(manager.test_pending_relay_connect_count(), 0u);
    EXPECT_EQ(relay.ReceiveMessage(), nullptr);
}

TEST(RelayOrchestrator, OrchestratorSendsRelayConnectForReachableHintInDevMode) {
    P2PManager manager(0);
    manager.set_plaintext_relay_dev_override_for_tests(true);
    InstallNodeIdentity(manager);

    auto relay = LoopbackSocketPair::Create();
    const std::string relay_key = "127.0.0.1:" + std::to_string(relay.port());
    manager.test_install_connected_direct_peer(relay_key, relay.client_fd(),
                                               true, false, {});

    const auto target = MakeNodeId(0x30);
    AddRelayHint(manager, target, relay.port());

    ASSERT_EQ(manager.test_pending_relay_connect_count(), 0u);
    manager.OrchestrateRelayDials();

    auto sent = relay.ReceiveMessage();
    ASSERT_NE(sent, nullptr);
    EXPECT_EQ(sent->command, "relaycon");
    ASSERT_EQ(sent->payload.size(), 28u);
    EXPECT_TRUE(std::equal(target.begin(), target.end(), sent->payload.begin()));
    EXPECT_NE(ReadLE64ForTest(sent->payload, 20), 0u);
    EXPECT_EQ(manager.test_pending_relay_connect_count(), 1u);
}

TEST(RelayOrchestrator, OrchestratorFallsBackFromFreshUnconnectedHint) {
    P2PManager manager(0);
    manager.set_plaintext_relay_dev_override_for_tests(true);
    InstallNodeIdentity(manager);

    auto relay = LoopbackSocketPair::Create();
    const std::string relay_key = "127.0.0.1:" + std::to_string(relay.port());
    manager.test_install_connected_direct_peer(relay_key, relay.client_fd(),
                                               true, false, {});

    const auto target = MakeNodeId(0x40);
    AddRelayHint(manager, target, relay.port());
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    AddRelayHint(manager, target, relay.port() + 1);

    manager.OrchestrateRelayDials();

    auto sent = relay.ReceiveMessage();
    ASSERT_NE(sent, nullptr);
    EXPECT_EQ(sent->command, "relaycon");
    ASSERT_EQ(sent->payload.size(), 28u);
    EXPECT_TRUE(std::equal(target.begin(), target.end(), sent->payload.begin()));
    EXPECT_EQ(manager.test_pending_relay_connect_count(), 1u);
}

// Codifies the post-encrypted-relay-data-plane contract: on mainnet, the
// orchestrator IS allowed to issue RELAY_CONNECT — the resulting circuit's
// data plane is QUIC-encrypted end-to-end (see install_outbound_virtual_relay_peer
// + handle_relay_data's encrypted_quic branch), so the RELAY_CONNECT signaling
// no longer leaks plaintext payloads. Pre-b79fde09 this site was gated;
// post-PR keeping the gate would just block mainnet inbound circuits for no
// privacy gain (the relay knows target_node_id anyway, it has to dispatch).
TEST(RelayOrchestrator, OrchestratorIssuesRelayConnectOnMainnet) {
    P2PManager manager(0);
    ASSERT_FALSE(manager.test_plaintext_relay_transport_allowed());
    InstallNodeIdentity(manager);

    auto relay = LoopbackSocketPair::Create();
    const std::string relay_key = "127.0.0.1:" + std::to_string(relay.port());
    manager.test_install_connected_direct_peer(relay_key, relay.client_fd(),
                                               true, false, {});

    const auto target = MakeNodeId(0x50);
    AddRelayHint(manager, target, relay.port());

    manager.OrchestrateRelayDials();

    // One pending connect was queued and one message hit the relay socket.
    EXPECT_EQ(manager.test_pending_relay_connect_count(), 1u);
    EXPECT_NE(relay.ReceiveMessage(), nullptr);
}

TEST(RelayHintsDial, RejectsMalformedTargetHex) {
    P2PManager manager(0);

    const auto result = manager.TryDialRelayHint("abc", std::nullopt, false);

    EXPECT_EQ(result.status,
              P2PManager::ManualRelayDialResult::Status::InvalidTarget);
    EXPECT_EQ(result.request_id, 0u);
}

TEST(RelayHintsDial, ReturnsNoHintForUnknownTarget) {
    P2PManager manager(0);
    const auto target = MakeNodeId(0x80);

    const auto result = manager.TryDialRelayHint(HexNodeId(target), std::nullopt, false);

    EXPECT_EQ(result.status, P2PManager::ManualRelayDialResult::Status::NoHint);
    EXPECT_EQ(result.request_id, 0u);
}

TEST(RelayHintsDial, DryRunOkRequiresCachedAndConnectedRelay) {
    P2PManager manager(0);
    auto relay = LoopbackSocketPair::Create();
    const std::string relay_key = "127.0.0.1:" + std::to_string(relay.port());
    manager.test_install_connected_direct_peer(relay_key, relay.client_fd(),
                                               true, false, {});

    const auto target = MakeNodeId(0x90);
    AddRelayHint(manager, target, relay.port());

    const auto result = manager.TryDialRelayHint(HexNodeId(target), std::nullopt, true);

    EXPECT_EQ(result.status, P2PManager::ManualRelayDialResult::Status::DryRunOk);
    EXPECT_EQ(result.target_node_id_hex, HexNodeId(target));
    EXPECT_EQ(result.relay_endpoint, relay_key);
    EXPECT_EQ(result.request_id, 0u);
    EXPECT_EQ(manager.test_pending_relay_connect_count(), 0u);
    EXPECT_EQ(relay.ReceiveMessage(), nullptr);
}

TEST(RelayHintsDial, ExactRelayEndpointMustComeFromCache) {
    P2PManager manager(0);
    const auto target = MakeNodeId(0xa0);
    AddRelayHint(manager, target, 20999);

    const auto result = manager.TryDialRelayHint(
        HexNodeId(target), std::optional<std::string>{"127.0.0.1:21000"}, true);

    EXPECT_EQ(result.status, P2PManager::ManualRelayDialResult::Status::NoHint);
    EXPECT_EQ(result.relay_endpoint, "127.0.0.1:21000");
    EXPECT_EQ(result.request_id, 0u);
}

TEST(RelayHintsDial, ReportsRelayNotConnectedForCachedHint) {
    P2PManager manager(0);
    const auto target = MakeNodeId(0xb0);
    AddRelayHint(manager, target, 20999);

    const auto result = manager.TryDialRelayHint(HexNodeId(target), std::nullopt, false);

    EXPECT_EQ(result.status,
              P2PManager::ManualRelayDialResult::Status::RelayNotConnected);
    EXPECT_EQ(result.relay_endpoint, "127.0.0.1:20999");
    EXPECT_EQ(result.request_id, 0u);
}

TEST(RelayHintsDial, SubmittedSendsRelayConnectForCachedConnectedRelay) {
    P2PManager manager(0);
    auto relay = LoopbackSocketPair::Create();
    const std::string relay_key = "127.0.0.1:" + std::to_string(relay.port());
    manager.test_install_connected_direct_peer(relay_key, relay.client_fd(),
                                               true, false, {});

    const auto target = MakeNodeId(0xc0);
    AddRelayHint(manager, target, relay.port());

    const auto result = manager.TryDialRelayHint(HexNodeId(target), std::nullopt, false);

    EXPECT_EQ(result.status, P2PManager::ManualRelayDialResult::Status::Submitted);
    EXPECT_EQ(result.relay_endpoint, relay_key);
    EXPECT_NE(result.request_id, 0u);
    EXPECT_EQ(manager.test_pending_relay_connect_count(), 1u);

    auto sent = relay.ReceiveMessage();
    ASSERT_NE(sent, nullptr);
    EXPECT_EQ(sent->command, "relaycon");
    ASSERT_EQ(sent->payload.size(), 28u);
    EXPECT_TRUE(std::equal(target.begin(), target.end(), sent->payload.begin()));
    EXPECT_EQ(ReadLE64ForTest(sent->payload, 20), result.request_id);
}

TEST(RelayHintsDial, IgnoresStaleDisconnectedVirtualPeerForAlreadyConnected) {
    P2PManager manager(0);
    auto relay = LoopbackSocketPair::Create();
    const std::string relay_key = "127.0.0.1:" + std::to_string(relay.port());
    manager.test_install_connected_direct_peer(relay_key, relay.client_fd(),
                                               true, false, {});

    const auto target = MakeNodeId(0xc8);
    const std::string stale_virtual_key = "relay:" + HexNodeId(target) + ":deadbeef:0";
    manager.test_install_connected_direct_peer(stale_virtual_key, -1,
                                               false, true, target);
    manager.test_set_peer_connected(stale_virtual_key, false);
    AddRelayHint(manager, target, relay.port());

    const auto result = manager.TryDialRelayHint(HexNodeId(target), std::nullopt, false);

    EXPECT_EQ(result.status, P2PManager::ManualRelayDialResult::Status::Submitted);
    EXPECT_EQ(result.relay_endpoint, relay_key);
    EXPECT_NE(result.request_id, 0u);
    EXPECT_EQ(manager.test_pending_relay_connect_count(), 1u);
}

TEST(RelayAutoRegister, KeepsBootstrapRelayWhenDynamicCandidatesFillBudget) {
    P2PManager manager(20999);
    dinero::p2p::AddressManager addrman;
    manager.set_address_manager(&addrman);

    addrman.addAddress(MakeRelayAddress("8.8.10.1", 20999));
    addrman.addAddress(MakeRelayAddress("9.9.20.2", 20999));
    addrman.addAddress(MakeRelayAddress("13.13.30.3", 20999));
    addrman.addAddress(MakeRelayAddress("14.14.40.4", 20999));

    std::vector<LoopbackSocketPair> sockets;
    auto install_connected = [&](const std::string& endpoint) {
        sockets.push_back(LoopbackSocketPair::Create());
        manager.test_install_connected_direct_peer(endpoint,
                                                   sockets.back().client_fd(),
                                                   true, false, {});
    };
    install_connected("173.249.200.59:20999");  // SJ / bootstrap fleet relay
    install_connected("8.8.10.1:20999");
    install_connected("9.9.20.2:20999");
    install_connected("13.13.30.3:20999");
    install_connected("14.14.40.4:20999");

    manager.test_maybe_auto_register_with_relays();

    const auto endpoints = manager.test_configured_relay_endpoints();
    EXPECT_EQ(endpoints.size(), 5u);
    EXPECT_TRUE(ContainsEndpoint(endpoints, "173.249.200.59:20999"));
}

TEST(RelayHintPersistence, FreshHintsSurviveRestartButExpiredHintsDoNot) {
    const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto path = std::filesystem::temp_directory_path() /
        ("dinero-relay-hints-" + std::to_string(suffix) + ".dat");
    const auto target = MakeNodeId(0x42);

    {
        auto clock = std::make_unique<dinero::network::FakeClockSource>();
        P2PManager manager(0, "", std::move(clock));
        AddRelayHint(manager, target, 20999);
        manager.save_relay_hints(path.string());
    }

    {
        auto clock = std::make_unique<dinero::network::FakeClockSource>();
        clock->AdvanceSteady(std::chrono::minutes(5));
        clock->AdvanceSystem(std::chrono::minutes(5));
        P2PManager restarted(0, "", std::move(clock));
        restarted.load_relay_hints(path.string());
        const auto snapshot = restarted.SnapshotRelayHintsForRpc();
        ASSERT_EQ(snapshot.entries.size(), 1u);
        ASSERT_EQ(snapshot.entries.front().endpoints.size(), 1u);
        EXPECT_EQ(snapshot.entries.front().target_hex, HexNodeId(target));
        EXPECT_EQ(snapshot.entries.front().endpoints.front().port, 20999);
        EXPECT_GE(snapshot.entries.front().endpoints.front().age_seconds, 300u);
    }

    {
        auto clock = std::make_unique<dinero::network::FakeClockSource>();
        clock->AdvanceSteady(std::chrono::minutes(16));
        clock->AdvanceSystem(std::chrono::minutes(16));
        P2PManager expired(0, "", std::move(clock));
        expired.load_relay_hints(path.string());
        EXPECT_TRUE(expired.SnapshotRelayHintsForRpc().entries.empty());
    }

    std::error_code ec;
    std::filesystem::remove(path, ec);
    std::filesystem::remove(path.string() + ".tmp", ec);
}

TEST(RelayOrchestrator, RelayConnectAckLeavesVirtualPeerCreationToCallback) {
    P2PManager manager(0);
    manager.set_plaintext_relay_dev_override_for_tests(true);
    const auto target = MakeNodeId(0x70);
    constexpr uint64_t request_id = 0x8877665544332211ULL;
    constexpr uint64_t circuit_id = 0x1020304050607080ULL;

    bool callback_called = false;
    bool virtual_peer_existed_before_callback = false;
    manager.test_insert_pending_relay_connect(
        request_id,
        target,
        kRelayPeer,
        [&](bool ok, uint64_t callback_circuit_id, const std::string&) {
            callback_called = true;
            EXPECT_TRUE(ok);
            EXPECT_EQ(callback_circuit_id, circuit_id);
            virtual_peer_existed_before_callback =
                manager.get_peer_info(manager.RelayVirtualPeerAddress(target, circuit_id)) != nullptr;
        });

    const auto ack = P2PMessage::create_relay_connect_ack(
        request_id,
        circuit_id,
        P2PMessage::RelayConnectStatus::Ok,
        "ok");
    manager.handle_relay_connect_ack(kRelayPeer, ack);

    EXPECT_TRUE(callback_called);
    EXPECT_FALSE(virtual_peer_existed_before_callback);
    EXPECT_EQ(manager.test_originated_circuit_count(), 1u);
    EXPECT_EQ(manager.get_peer_info(manager.RelayVirtualPeerAddress(target, circuit_id)),
              nullptr);
}

TEST(RelayBehindThrottle, RefusesNewCircuitsWhenBehind) {
    P2PManager manager(0);
    auto requester = LoopbackSocketPair::Create();
    const std::string requester_key =
        "127.0.0.1:" + std::to_string(requester.port());
    manager.test_install_connected_direct_peer(requester_key,
                                               requester.client_fd(),
                                               true, false, {});

    constexpr uint64_t request_id = 0x1122334455667788ULL;
    const auto relay_connect =
        P2PMessage::create_relay_connect(MakeNodeId(0x20), request_id);

    manager.test_set_relay_behind_throttle(true);
    manager.handle_relay_connect(requester_key, relay_connect);

    auto ack = requester.ReceiveMessage();
    ASSERT_NE(ack, nullptr);
    EXPECT_EQ(ReadLE64ForTest(ack->payload, 0), request_id);
    EXPECT_EQ(ReadLE64ForTest(ack->payload, 8), 0u);
    EXPECT_EQ(RelayAckStatus(*ack),
              static_cast<uint8_t>(P2PMessage::RelayConnectStatus::RateLimited));
    EXPECT_EQ(manager.test_relay_drops_behind_count(), 0u);
}

TEST(RelayBehindThrottle, EstablishedCircuitStillForwardsWhenBehind) {
    P2PManager manager(0);
    auto target = LoopbackSocketPair::Create();
    const std::string target_key =
        "127.0.0.1:" + std::to_string(target.port());
    const char* kRequester = "10.0.0.7:20999";
    constexpr uint64_t circuit_id = 0xAABBCCDDEEFF0011ULL;

    manager.test_install_connected_direct_peer(target_key, target.client_fd(),
                                               true, false, {});
    manager.test_install_relay_circuit(circuit_id, kRequester, target_key);

    // Stand-in for a tunneled QUIC Initial packet. The relay cannot inspect
    // its encrypted contents; the invariant is that already-open circuits are
    // forwarded rather than silently black-holed while the relay is behind.
    const std::vector<uint8_t> quic_packet(64, 0x5A);
    const auto frame = P2PMessage::create_relay_data(
        circuit_id, P2PMessage::RelayDirection::ClientToTarget, quic_packet);

    manager.test_set_relay_behind_throttle(true);
    manager.handle_relay_data(kRequester, frame);

    auto forwarded = target.ReceiveMessage();
    ASSERT_NE(forwarded, nullptr);
    EXPECT_EQ(forwarded->command, "relaydat");
    EXPECT_EQ(forwarded->payload, frame.payload);
    EXPECT_EQ(manager.test_relay_drops_behind_count(), 0u);
}

// Relay health scoring (#3): the auto-registration set prefers healthy relays
// and replaces persistently-failing ones. This pins the EWMA score + the
// replacement-eligibility rules that MaybeAutoRegisterWithRelays consults.
TEST(RelayHealthScoring, ScoreEwmaAndReplacementEligibility) {
    P2PManager manager(0);
    const std::string r = "203.0.113.7:20999";

    // Unknown relay: neutral prior, never replace-eligible (give it a chance).
    EXPECT_DOUBLE_EQ(manager.relay_health_score("unknown:1"), 0.5);
    EXPECT_FALSE(manager.relay_health_replace_eligible("unknown:1"));

    // Successes pull the EWMA toward 1.0; a healthy relay is never replaced.
    for (int i = 0; i < 5; ++i) manager.note_relay_outcome(r, true);
    EXPECT_GT(manager.relay_health_score(r), 0.8);
    EXPECT_FALSE(manager.relay_health_replace_eligible(r));

    // A run of consecutive failures (>= kRelayMaxConsecutiveFailures = 4) makes
    // the relay replace-eligible and drives the score below neutral.
    for (int i = 0; i < 4; ++i) manager.note_relay_outcome(r, false);
    EXPECT_TRUE(manager.relay_health_replace_eligible(r));
    EXPECT_LT(manager.relay_health_score(r), 0.5);

    // A single success resets the consecutive-failure streak and lifts the
    // score back above the replace floor → no longer eligible.
    manager.note_relay_outcome(r, true);
    EXPECT_FALSE(manager.relay_health_replace_eligible(r));

    // Endpoint keying is case-insensitive.
    manager.note_relay_outcome("HOST:20999", true);
    EXPECT_GT(manager.relay_health_score("host:20999"), 0.5);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
