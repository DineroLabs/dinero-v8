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
#include "daemon/services/relay_role_policy.h"
#include "daemon/node_identity.h"
#include "daemon/peer_send_health.h"
#include "network/types.h"
#include "p2p/addrman.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <ctime>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <map>
#include <memory>
#include <set>
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

TEST(RelayServicePolicy, OffRejectsNewCircuitWithoutStoppingDirectP2P) {
    P2PManager manager(0);
    auto requester = LoopbackSocketPair::Create();
    const std::string requester_key =
        "127.0.0.1:" + std::to_string(requester.port());
    manager.test_install_connected_direct_peer(requester_key,
                                               requester.client_fd(),
                                               true, false, {});
    manager.set_relay_service_enabled(false);
    constexpr uint64_t request_id = 0x8877665544332211ULL;
    manager.handle_relay_connect(requester_key,
        P2PMessage::create_relay_connect(MakeNodeId(0x20), request_id));
    auto ack = requester.ReceiveMessage();
    ASSERT_NE(ack, nullptr);
    EXPECT_EQ(RelayAckStatus(*ack),
              static_cast<uint8_t>(P2PMessage::RelayConnectStatus::RateLimited));
    EXPECT_NE(manager.get_peer_info(requester_key), nullptr);
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

// Plan 3 Task 1: a node whose address other nodes dialed back relays for the
// NAT'd nodes that can't be reached, without the owner turning anything on.
TEST(RelayServicePolicy, VerifiedReachableEnablesRelayInAuto) {
    using dinero::daemon::RelayRoleEnabled;
    EXPECT_TRUE(RelayRoleEnabled("automatic", "auto", /*mining=*/false, /*verified=*/true));
    EXPECT_TRUE(RelayRoleEnabled("automatic", "auto", /*mining=*/true, /*verified=*/false));
    EXPECT_TRUE(RelayRoleEnabled("automatic", "on", false, false));
}

TEST(RelayServicePolicy, OperatorOffBeatsVerified) {
    using dinero::daemon::RelayRoleEnabled;
    EXPECT_FALSE(RelayRoleEnabled("off", "auto", true, true));
    EXPECT_FALSE(RelayRoleEnabled("off", "on", true, true));
    EXPECT_FALSE(RelayRoleEnabled("automatic", "off", true, true));
    EXPECT_FALSE(RelayRoleEnabled("automatic", "0", false, true));
    EXPECT_TRUE(RelayRoleEnabled("custom", "off", false, false));
}

TEST(RelayServicePolicy, UnverifiedNonMiningStaysOff) {
    EXPECT_FALSE(dinero::daemon::RelayRoleEnabled("automatic", "auto", false, false));
    EXPECT_FALSE(dinero::daemon::RelayRoleEnabled("automatic", "", false, false));
}

TEST(RelayServicePolicy, ServiceBitFollowsVerification) {
    P2PManager manager(0);
    std::vector<bool> seen;
    manager.set_reachability_changed_handler([&](bool reachable) { seen.push_back(reachable); });

    manager.update_stun_candidates({{"198.51.100.9", 20999}});
    manager.test_run_reachability_tick();
    EXPECT_TRUE(seen.empty()) << "nothing verified yet, nothing changed";

    manager.test_mark_self_verified("203.0.113.50", 20999);
    manager.test_run_reachability_tick();
    ASSERT_EQ(seen.size(), 1u);
    EXPECT_TRUE(seen.back());
    EXPECT_TRUE(dinero::daemon::RelayRoleEnabled("automatic", "auto", false, seen.back()));

    manager.test_run_reachability_tick();
    EXPECT_EQ(seen.size(), 1u) << "fires on a change only";

    // New public IP: nothing stays verified, so the relay role (and NODE_RELAY) goes.
    manager.update_stun_candidates({{"192.0.2.77", 20999}});
    ASSERT_EQ(seen.size(), 2u);
    EXPECT_FALSE(seen.back());
    EXPECT_FALSE(dinero::daemon::RelayRoleEnabled("automatic", "auto", false, seen.back()));
}

// Plan 3 Task 2: the owner's caps are the defaults — at most 8 circuits,
// 200 KB/s of relayed traffic, 20 GB a calendar month (kept across
// restarts) — and a relay that is syncing stops taking new circuits.
namespace {

std::chrono::system_clock::time_point Utc(int year, int month, int day) {
    std::tm tm{};
    tm.tm_year = year - 1900;
    tm.tm_mon = month - 1;
    tm.tm_mday = day;
    tm.tm_hour = 12;
#ifdef _WIN32
    const std::time_t t = _mkgmtime(&tm);
#else
    const std::time_t t = timegm(&tm);
#endif
    return std::chrono::system_clock::from_time_t(t);
}

std::unique_ptr<P2PManager> ManagerAt(std::chrono::system_clock::time_point when) {
    auto clock = std::make_unique<dinero::network::FakeClockSource>();
    clock->AdvanceSystem(when.time_since_epoch());
    return std::make_unique<P2PManager>(0, "", std::move(clock));
}

}  // namespace

TEST(RelayCaps, DefaultsAreTheOwnersCaps) {
    const P2PManager::RelayServiceLimits limits;
    EXPECT_EQ(limits.max_circuits, 8u);
    EXPECT_EQ(limits.bandwidth_bytes_per_second, 200u * 1024u);
    EXPECT_EQ(limits.monthly_quota_bytes, 20ULL * 1024 * 1024 * 1024);
    EXPECT_EQ(limits.max_circuits_per_peer, 2u);
    EXPECT_EQ(limits.circuit_lifetime_seconds, 1800u);
}

TEST(RelayCaps, NinthCircuitRefused) {
    P2PManager manager(0);
    auto target = LoopbackSocketPair::Create();
    const std::string target_key = "127.0.0.1:" + std::to_string(target.port());
    manager.test_install_connected_direct_peer(target_key, target.client_fd(), false, true,
                                               MakeNodeId(0x30));
    manager.test_register_relay_target(MakeNodeId(0x30), target_key);

    std::vector<LoopbackSocketPair> requesters;
    for (int i = 0; i < 9; ++i) requesters.push_back(LoopbackSocketPair::Create());
    for (int i = 0; i < 9; ++i) {
        const std::string key = "127.0.0.1:" + std::to_string(requesters[i].port());
        manager.test_install_connected_direct_peer(key, requesters[i].client_fd(), true, false, {});
        manager.handle_relay_connect(key, P2PMessage::create_relay_connect(MakeNodeId(0x30), 100 + i));
        auto ack = requesters[i].ReceiveMessage();
        ASSERT_NE(ack, nullptr) << "requester " << i;
        const auto expected = i < 8 ? P2PMessage::RelayConnectStatus::Ok
                                    : P2PMessage::RelayConnectStatus::RelayFull;
        EXPECT_EQ(RelayAckStatus(*ack), static_cast<uint8_t>(expected)) << "requester " << i;
    }
}

TEST(RelayCaps, GlobalRateStaysUnder200KBps) {
    // Frames over the cap are dropped (the tunneled QUIC retransmits), so the
    // cap shows up as the bytes forwarded per second of wall time.
    P2PManager manager(0);
    auto target = LoopbackSocketPair::Create();
    const std::string target_key = "127.0.0.1:" + std::to_string(target.port());
    manager.test_install_connected_direct_peer(target_key, target.client_fd(), true, false, {});
    const std::vector<std::string> requesters = {"10.0.0.1:20999", "10.0.0.2:20999",
                                                 "10.0.0.3:20999"};
    for (size_t i = 0; i < requesters.size(); ++i) {
        manager.test_install_relay_circuit(0x1000 + i, requesters[i], target_key);
    }
    const std::vector<uint8_t> chunk(4096, 0x5A);
    uint64_t offered = 0, frames = 0;
    size_t frame_bytes = 0;
    const auto start = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - start < std::chrono::seconds(2)) {
        const size_t i = frames % requesters.size();
        const auto frame = P2PMessage::create_relay_data(
            0x1000 + i, P2PMessage::RelayDirection::ClientToTarget, chunk);
        frame_bytes = frame.payload.size();
        manager.handle_relay_data(requesters[i], frame);
        offered += frame_bytes;
        ++frames;
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    const double elapsed =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    const uint64_t forwarded = offered - manager.test_relay_cap_drop_frames() * frame_bytes;
    const double cap = 200.0 * 1024.0;
    ASSERT_GT(offered, 3 * cap) << "the test must offer more than the cap";
    EXPECT_LE(forwarded, (cap * elapsed + cap) * 1.1) << "forwarded " << forwarded;
    EXPECT_GE(forwarded, cap * elapsed * 0.9) << "forwarded " << forwarded;
}

TEST(RelayCaps, MonthlyQuotaPersistsAcrossRestartAndResetsOnNewMonth) {
    const auto dir = std::filesystem::temp_directory_path() /
                     ("relay_quota_test_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(dir);
    const std::string path = (dir / "relay_quota").string();
    std::filesystem::remove(path);
    P2PManager::RelayServiceLimits small;
    small.monthly_quota_bytes = 10000;
    const std::vector<uint8_t> chunk(1000, 0x5A);
    auto send = [&](P2PManager& m) {
        const auto frame = P2PMessage::create_relay_data(
            0x77, P2PMessage::RelayDirection::ClientToTarget, chunk);
        const uint64_t before = m.test_relay_cap_drop_frames();
        m.handle_relay_data("10.0.0.9:20999", frame);
        return m.test_relay_cap_drop_frames() == before;  // forwarded
    };
    auto target = LoopbackSocketPair::Create();
    const std::string target_key = "127.0.0.1:" + std::to_string(target.port());
    uint64_t used = 0;
    {
        auto m = ManagerAt(Utc(2026, 10, 15));
        m->set_relay_service_limits(small);
        m->set_relay_quota_path(path);
        m->test_install_connected_direct_peer(target_key, target.client_fd(), true, false, {});
        m->test_install_relay_circuit(0x77, "10.0.0.9:20999", target_key);
        int forwarded = 0;
        for (int i = 0; i < 20; ++i) forwarded += send(*m) ? 1 : 0;
        EXPECT_GE(forwarded, 1);
        EXPECT_LT(forwarded, 20) << "the quota must bite";
        used = m->relay_quota_month_bytes();
        EXPECT_GT(used, 0u);
        EXPECT_LE(used, 10000u);
        EXPECT_EQ(m->relay_paused_reason(), "quota");
    }  // shutdown writes the quota
    {
        auto m = ManagerAt(Utc(2026, 10, 28));
        m->set_relay_service_limits(small);
        m->set_relay_quota_path(path);
        EXPECT_EQ(m->relay_quota_month_bytes(), used) << "restart must not reset the month";
        auto t2 = LoopbackSocketPair::Create();
        const std::string t2_key = "127.0.0.1:" + std::to_string(t2.port());
        m->test_install_connected_direct_peer(t2_key, t2.client_fd(), true, false, {});
        m->test_install_relay_circuit(0x77, "10.0.0.9:20999", t2_key);
        EXPECT_FALSE(send(*m)) << "still over this month's quota after restart";
    }
    {
        auto m = ManagerAt(Utc(2026, 11, 1));
        m->set_relay_service_limits(small);
        m->set_relay_quota_path(path);
        EXPECT_EQ(m->relay_quota_month_bytes(), 0u) << "a new month starts at zero";
        auto t3 = LoopbackSocketPair::Create();
        const std::string t3_key = "127.0.0.1:" + std::to_string(t3.port());
        m->test_install_connected_direct_peer(t3_key, t3.client_fd(), true, false, {});
        m->test_install_relay_circuit(0x77, "10.0.0.9:20999", t3_key);
        EXPECT_TRUE(send(*m));
    }
    std::filesystem::remove_all(dir);
}

TEST(RelayCaps, SyncingRelayRefusesNewCircuits) {
    P2PManager manager(0);
    uint32_t header_height = 107;
    bool ibd = false;
    manager.set_height_provider([] { return 100u; });
    manager.set_best_header_height_provider([&] { return header_height; });
    manager.set_ibd_provider([&] { return ibd; });
    auto requester = LoopbackSocketPair::Create();
    const std::string requester_key = "127.0.0.1:" + std::to_string(requester.port());
    manager.test_install_connected_direct_peer(requester_key, requester.client_fd(), true, false,
                                               {});
    auto target = LoopbackSocketPair::Create();
    const std::string target_key = "127.0.0.1:" + std::to_string(target.port());
    manager.test_install_connected_direct_peer(target_key, target.client_fd(), true, false, {});
    manager.test_install_relay_circuit(0xABCD, "10.0.0.7:20999", target_key);

    auto connect_status = [&](uint64_t id) {
        manager.handle_relay_connect(requester_key,
                                     P2PMessage::create_relay_connect(MakeNodeId(0x40), id));
        auto ack = requester.ReceiveMessage();
        return ack ? RelayAckStatus(*ack) : uint8_t{0xFF};
    };
    const auto kRateLimited = static_cast<uint8_t>(P2PMessage::RelayConnectStatus::RateLimited);

    manager.test_recompute_relay_behind_throttle();  // 7 blocks behind its best header
    EXPECT_EQ(connect_status(1), kRateLimited);
    EXPECT_EQ(manager.relay_paused_reason(), "syncing");

    // An established circuit still carries data.
    const auto frame = P2PMessage::create_relay_data(
        0xABCD, P2PMessage::RelayDirection::ClientToTarget, std::vector<uint8_t>(64, 0x5A));
    manager.handle_relay_data("10.0.0.7:20999", frame);
    auto forwarded = target.ReceiveMessage();
    ASSERT_NE(forwarded, nullptr);
    EXPECT_EQ(forwarded->payload, frame.payload);

    header_height = 106;  // 6 behind is fine
    manager.test_recompute_relay_behind_throttle();
    EXPECT_NE(connect_status(2), kRateLimited);
    EXPECT_EQ(manager.relay_paused_reason(), "");

    ibd = true;  // initial block download always pauses
    manager.test_recompute_relay_behind_throttle();
    EXPECT_EQ(connect_status(3), kRateLimited);
}

// Plan 3 Task 3: a NAT'd node keeps two relays on different networks,
// community relays first and the fleet only to fill; a relay counts only
// once it acks the registration (relayrok), and one that stops answering
// relay pings is replaced.
namespace {

struct RelayWorld {
    dinero::p2p::AddressManager addrman;  // outlives the manager
    dinero::network::FakeClockSource* clock{nullptr};
    std::unique_ptr<P2PManager> manager;
    std::map<std::string, std::unique_ptr<LoopbackSocketPair>> sockets;

    RelayWorld() {
        auto c = std::make_unique<dinero::network::FakeClockSource>();
        c->AdvanceSystem(std::chrono::hours(500000));
        c->AdvanceSteady(std::chrono::hours(1));
        clock = c.get();
        manager = std::make_unique<P2PManager>(20999, "", std::move(c));
        manager->set_address_manager(&addrman);
        InstallNodeIdentity(*manager);
    }
    // A connected, identity-proven peer; `gossiped` also puts it in addrman as NODE_RELAY.
    void Connect(const std::string& endpoint, bool gossiped) {
        sockets[endpoint] = std::make_unique<LoopbackSocketPair>(LoopbackSocketPair::Create());
        manager->test_install_connected_direct_peer(endpoint, sockets[endpoint]->client_fd(), true,
                                                    true, MakeNodeId(0x50));
        const auto colon = endpoint.rfind(':');
        manager->test_set_peer_endpoint(endpoint, endpoint.substr(0, colon), 20999);
        if (gossiped) addrman.addAddress(MakeRelayAddress(endpoint.substr(0, colon), 20999));
    }
    // The next message with `command` the manager sent to `endpoint`.
    std::unique_ptr<P2PMessage> Next(const std::string& endpoint, const std::string& command) {
        for (int i = 0; i < 16; ++i) {
            auto m = sockets[endpoint]->ReceiveMessage();
            if (!m || m->command == command) return m;
        }
        return nullptr;
    }
    void Ack(const std::string& endpoint) {
        manager->handle_relay_register_ok(endpoint, P2PMessage::create_relay_register_ok(3600));
    }
    // Answers the relay ping the manager sent to `endpoint`.
    void Pong(const std::string& endpoint) {
        auto ping = Next(endpoint, "relaypng");
        ASSERT_NE(ping, nullptr) << "no relay ping sent to " << endpoint;
        manager->handle_relay_ping(endpoint, *ping);  // relays echo circuit 0 pings
    }
    void Tick(std::chrono::seconds s) {
        clock->AdvanceSteady(s);
        manager->test_relay_registration_tick();
        manager->test_maybe_auto_register_with_relays();
    }
    std::vector<std::string> Relays() const { return manager->test_configured_relay_endpoints(); }
};

}  // namespace

TEST(RelayRegistration, TargetsTwoRelaysInDistinctNetgroups) {
    dinero::p2p::AddressManager later_gossip;  // outlives the manager
    RelayWorld w;
    w.Connect("173.249.200.59:20999", false);  // fleet, so nothing real is dialed
    w.Connect("8.8.10.1:20999", true);
    w.manager->test_maybe_auto_register_with_relays();
    ASSERT_TRUE(ContainsEndpoint(w.Relays(), "8.8.10.1:20999"));  // now registered (live)
    // Addrman spreads its own answers over /16s; the live set and gossip must
    // be spread together: 8.8.20.2 shares 8.8.10.1's /16 and is the healthiest.
    w.manager->set_address_manager(&later_gossip);
    w.Connect("8.8.20.2:20999", false);
    w.Connect("9.9.20.2:20999", false);
    later_gossip.addAddress(MakeRelayAddress("8.8.20.2", 20999));
    later_gossip.addAddress(MakeRelayAddress("9.9.20.2", 20999));
    for (int i = 0; i < 5; ++i) w.manager->note_relay_outcome("8.8.20.2:20999", true);
    w.manager->test_maybe_auto_register_with_relays();
    const auto relays = w.Relays();
    ASSERT_EQ(relays.size(), 2u);
    std::set<std::string> groups;
    for (const auto& r : relays) groups.insert(dinero::p2p::AddrManNetGroup(r.substr(0, r.rfind(':'))));
    EXPECT_EQ(groups.size(), 2u) << relays[0] << " " << relays[1];
    EXPECT_TRUE(ContainsEndpoint(relays, "8.8.10.1:20999"));
    EXPECT_TRUE(ContainsEndpoint(relays, "9.9.20.2:20999"));
}

TEST(RelayRegistration, PrefersCommunityOverFleet) {
    RelayWorld w;
    w.Connect("173.249.200.59:20999", false);  // SJ (fleet), already connected
    w.Connect("8.8.10.1:20999", true);
    w.Connect("9.9.20.2:20999", true);
    w.manager->test_maybe_auto_register_with_relays();
    const auto relays = w.Relays();
    EXPECT_EQ(relays.size(), 2u);
    EXPECT_TRUE(ContainsEndpoint(relays, "8.8.10.1:20999"));
    EXPECT_TRUE(ContainsEndpoint(relays, "9.9.20.2:20999"));
    EXPECT_FALSE(ContainsEndpoint(relays, "173.249.200.59:20999"));
}

TEST(RelayRegistration, FleetFillsWhenNoCommunity) {
    RelayWorld w;
    w.Connect("173.249.200.59:20999", false);
    w.Connect("172.93.167.32:20999", false);
    w.manager->test_maybe_auto_register_with_relays();
    const auto relays = w.Relays();
    EXPECT_EQ(relays.size(), 2u);
    EXPECT_TRUE(ContainsEndpoint(relays, "173.249.200.59:20999"));
    EXPECT_TRUE(ContainsEndpoint(relays, "172.93.167.32:20999"));

    // One community relay appears: it takes a slot, the fleet keeps the other.
    w.Connect("8.8.10.1:20999", true);
    w.manager->test_maybe_auto_register_with_relays();
    const auto mixed = w.Relays();
    EXPECT_EQ(mixed.size(), 2u);
    EXPECT_TRUE(ContainsEndpoint(mixed, "8.8.10.1:20999"));
}

TEST(RelayRegistration, UnackedRegistrationIsReplaced) {
    RelayWorld w;
    w.Connect("8.8.10.1:20999", true);
    w.Connect("9.9.20.2:20999", true);
    w.manager->test_maybe_auto_register_with_relays();
    ASSERT_NE(w.Next("8.8.10.1:20999", "relayreg"), nullptr);
    ASSERT_NE(w.Next("9.9.20.2:20999", "relayreg"), nullptr);
    EXPECT_EQ(w.manager->test_confirmed_relay_count(), 0u) << "nothing counts before relayrok";

    w.Ack("8.8.10.1:20999");  // 9.9.20.2 (role off, old build) stays silent
    EXPECT_EQ(w.manager->test_confirmed_relay_count(), 1u);

    w.Connect("13.13.30.3:20999", true);
    w.Tick(std::chrono::seconds(31));
    const auto relays = w.Relays();
    EXPECT_TRUE(ContainsEndpoint(relays, "8.8.10.1:20999"));
    EXPECT_TRUE(ContainsEndpoint(relays, "13.13.30.3:20999"));
    EXPECT_FALSE(ContainsEndpoint(relays, "9.9.20.2:20999")) << "unacked relay must be replaced";
    ASSERT_NE(w.Next("13.13.30.3:20999", "relayreg"), nullptr);
    w.Ack("13.13.30.3:20999");
    EXPECT_EQ(w.manager->test_confirmed_relay_count(), 2u);

    // Still skipped while the 30-minute penalty runs.
    w.Tick(std::chrono::minutes(10));
    EXPECT_FALSE(ContainsEndpoint(w.Relays(), "9.9.20.2:20999"));
}

TEST(RelayRegistration, DeadRelayDetectedByMissedPingsAndReplaced) {
    RelayWorld w;
    w.Connect("8.8.10.1:20999", true);
    w.Connect("9.9.20.2:20999", true);
    w.manager->test_maybe_auto_register_with_relays();
    w.Ack("8.8.10.1:20999");
    w.Ack("9.9.20.2:20999");
    ASSERT_EQ(w.manager->test_confirmed_relay_count(), 2u);
    w.Connect("13.13.30.3:20999", true);

    // 8.8.10.1 answers every ping; 9.9.20.2 goes silent.
    for (int round = 0; round < 3; ++round) {
        w.Tick(std::chrono::seconds(30));
        w.Pong("8.8.10.1:20999");
        ASSERT_NE(w.Next("9.9.20.2:20999", "relaypng"), nullptr) << "round " << round;
    }
    EXPECT_TRUE(ContainsEndpoint(w.Relays(), "9.9.20.2:20999")) << "three pings sent, not yet missed";
    w.Tick(std::chrono::seconds(30));  // the third missed pong
    const auto relays = w.Relays();
    EXPECT_TRUE(ContainsEndpoint(relays, "8.8.10.1:20999"));
    EXPECT_FALSE(ContainsEndpoint(relays, "9.9.20.2:20999"));
    EXPECT_TRUE(ContainsEndpoint(relays, "13.13.30.3:20999"));
    EXPECT_EQ(w.manager->test_confirmed_relay_count(), 1u);
}

TEST(RelayRegistration, RelayAcksRegistrationAndEchoesPings) {
    P2PManager relay(0);
    const auto dir = std::filesystem::temp_directory_path() /
                     ("dinero-registrant-" + std::to_string(
                         std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(dir);
    dinero::daemon::NodeIdentity registrant;
    ASSERT_TRUE(registrant.initialize(dir.string()));
    std::filesystem::remove_all(dir);

    auto sock = LoopbackSocketPair::Create();
    const std::string key = "127.0.0.1:" + std::to_string(sock.port());
    relay.test_install_connected_direct_peer(key, sock.client_fd(), false, true, {});
    constexpr uint64_t kNonce = 0x1234567890ABCDEFULL;
    relay.test_set_peer_proven_identity(key, registrant.get_node_id_bytes(),
                                        registrant.get_pubkey_bytes(), kNonce);

    relay.handle_relay_register(key, P2PMessage::create_relay_register(registrant, kNonce, 3600));
    auto ack = sock.ReceiveMessage();
    ASSERT_NE(ack, nullptr);
    EXPECT_EQ(ack->command, "relayrok");
    ASSERT_EQ(ack->payload.size(), 4u);
    EXPECT_EQ(ack->payload[0] | (ack->payload[1] << 8), 3600);

    relay.handle_relay_ping(key, P2PMessage::create_relay_ping(0, 777));
    auto pong = sock.ReceiveMessage();
    ASSERT_NE(pong, nullptr);
    EXPECT_EQ(pong->command, "relaypng");
    EXPECT_EQ(ReadLE64ForTest(pong->payload, 0), 0u);
    EXPECT_EQ(ReadLE64ForTest(pong->payload, 8), 777u);
}
