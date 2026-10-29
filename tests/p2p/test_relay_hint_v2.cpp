// relayhn2: target-signed relay hints. Only the target can vouch for its own
// relay, so a hint survives forwarding; a forged, tampered or stale one is
// dropped before it is stored or forwarded, and forwarding stays bounded.

#include "p2p/relay_hint_v2.h"
#include "daemon/node_identity.h"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>

namespace {

using dinero::p2p::HintFields;
using dinero::p2p::HintForwarder;
using dinero::p2p::NetworkType;
using dinero::p2p::SignHint;
using dinero::p2p::VerifyHint;

constexpr uint32_t kNow = 1'800'000'000;

std::unique_ptr<dinero::daemon::NodeIdentity> MakeIdentity() {
    const auto dir = std::filesystem::temp_directory_path() /
                     ("dinero-hint-v2-" + std::to_string(
                         std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(dir);
    auto id = std::make_unique<dinero::daemon::NodeIdentity>();
    const bool ok = id->initialize(dir.string());
    std::filesystem::remove_all(dir);
    if (!ok) throw std::runtime_error("identity");
    return id;
}

HintFields ViaRelay(uint32_t expiry) {
    HintFields h;
    h.relay_net = NetworkType::IPV4;
    h.relay_addr = {8, 8, 10, 1};
    h.relay_port = 20999;
    h.expiry = expiry;
    return h;
}

}  // namespace

TEST(SignedHints, ValidHintVerifies) {
    auto a = MakeIdentity();
    const auto wire = SignHint(*a, ViaRelay(kNow + 1800));
    ASSERT_FALSE(wire.empty());
    const auto h = VerifyHint(wire, kNow);
    ASSERT_TRUE(h.has_value());
    EXPECT_EQ(h->node_id, a->get_node_id_bytes());
    EXPECT_EQ(h->pubkey, a->get_pubkey_bytes());
    EXPECT_EQ(h->relay_addr, (std::vector<uint8_t>{8, 8, 10, 1}));
    EXPECT_EQ(h->relay_port, 20999);
    EXPECT_EQ(h->expiry, kNow + 1800);
    EXPECT_EQ(h->hop, 0);

    // The hop count is not signed: a forwarder may bump it.
    const auto bumped = VerifyHint(dinero::p2p::WithHop(wire, 2), kNow);
    ASSERT_TRUE(bumped.has_value());
    EXPECT_EQ(bumped->hop, 2);
}

TEST(SignedHints, ForgedOrExpiredHintIsDroppedAndNotForwarded) {
    auto a = MakeIdentity();
    auto mallory = MakeIdentity();
    const auto good = SignHint(*a, ViaRelay(kNow + 1800));
    ASSERT_TRUE(VerifyHint(good, kNow).has_value());

    // Wrong key: Mallory signs a hint and swaps in A's node id and key.
    auto wrong_key = SignHint(*mallory, ViaRelay(kNow + 1800));
    std::copy(good.begin(), good.begin() + 20 + 33, wrong_key.begin());
    EXPECT_FALSE(VerifyHint(wrong_key, kNow).has_value()) << "signature by another key";

    // node_id != HASH160(pubkey): A's node id with Mallory's key and signature.
    auto mismatched = SignHint(*mallory, ViaRelay(kNow + 1800));
    std::copy(good.begin(), good.begin() + 20, mismatched.begin());
    EXPECT_FALSE(VerifyHint(mismatched, kNow).has_value()) << "node id not the key's hash";

    // Tampered relay address: "A is reachable via Mallory's relay".
    auto tampered = good;
    tampered[20 + 33 + 2 + 3] ^= 0x01;  // last byte of the IPv4 relay address
    EXPECT_FALSE(VerifyHint(tampered, kNow).has_value()) << "relay address changed";

    // Expired, and implausibly far in the future.
    EXPECT_FALSE(VerifyHint(SignHint(*a, ViaRelay(kNow - 1)), kNow).has_value());
    EXPECT_FALSE(VerifyHint(SignHint(*a, ViaRelay(kNow + 7200)), kNow).has_value());
    // Replayed after it expired.
    EXPECT_FALSE(VerifyHint(good, kNow + 1800).has_value());
    // Truncated.
    EXPECT_FALSE(VerifyHint(std::span<const uint8_t>(good.data(), good.size() - 1), kNow));
}

TEST(SignedHints, ForwarderDedupsAndHonorsHopLimit) {
    auto a = MakeIdentity();
    const auto h = VerifyHint(SignHint(*a, ViaRelay(kNow + 1800)), kNow);
    ASSERT_TRUE(h.has_value());
    HintForwarder f;
    const auto t = std::chrono::steady_clock::now();
    EXPECT_TRUE(f.ShouldForward(*h, kNow, t));
    EXPECT_FALSE(f.ShouldForward(*h, kNow, t)) << "once per (target, relay, expiry)";

    auto renewed = *h;
    renewed.expiry += 600;  // the target re-signed
    EXPECT_TRUE(f.ShouldForward(renewed, kNow, t));

    auto far = *h;
    far.expiry += 1200;
    far.hop = dinero::p2p::kHintMaxHops;
    EXPECT_FALSE(f.ShouldForward(far, kNow, t)) << "hop limit";
}

TEST(SignedHints, ForwardingIsRateLimited) {
    auto a = MakeIdentity();
    HintForwarder f(50.0);
    const auto t = std::chrono::steady_clock::now();
    int forwarded = 0;
    for (uint32_t i = 0; i < 80; ++i) {
        HintFields h = ViaRelay(kNow + 100 + i);
        h.node_id = a->get_node_id_bytes();
        forwarded += f.ShouldForward(h, kNow, t) ? 1 : 0;
    }
    EXPECT_EQ(forwarded, 50);
}

// Review I4: a hint the budget refused used to be marked seen anyway, so it
// was never forwarded even after the bucket refilled.
TEST(SignedHints, RefusedForBudgetIsForwardedLater) {
    auto a = MakeIdentity();
    HintForwarder f(1.0);  // one forward a minute
    const auto t = std::chrono::steady_clock::now();
    HintFields first = ViaRelay(kNow + 100);
    first.node_id = a->get_node_id_bytes();
    HintFields second = ViaRelay(kNow + 200);
    second.node_id = a->get_node_id_bytes();
    ASSERT_TRUE(f.ShouldForward(first, kNow, t));
    EXPECT_FALSE(f.ShouldForward(second, kNow, t)) << "budget exhausted";
    EXPECT_TRUE(f.ShouldForward(second, kNow + 61, t + std::chrono::seconds(61)))
        << "a hint refused for budget was never forwarded";
}

// Review I1: the signature authenticates the target, not the relay address,
// so a hint may only name a public relay: nodes would dial it.
TEST(SignedHints, HintNamingAPrivateRelayIsRejected) {
    auto a = MakeIdentity();
    for (std::vector<uint8_t> ip : {std::vector<uint8_t>{127, 0, 0, 1}, {192, 168, 1, 1}, {10, 0, 0, 5},
                                    {172, 16, 3, 4}, {169, 254, 1, 1}, {100, 64, 0, 1}, {0, 0, 0, 0},
                                    {224, 0, 0, 1}}) {
        HintFields h = ViaRelay(kNow + 1800);
        h.relay_addr = ip;
        const auto wire = SignHint(*a, h);
        EXPECT_FALSE(VerifyHint(wire, kNow).has_value())
            << int(ip[0]) << "." << int(ip[1]) << "." << int(ip[2]) << "." << int(ip[3]);
        EXPECT_TRUE(VerifyHint(wire, kNow, /*allow_private_relay=*/true).has_value());
    }
    HintFields v6 = ViaRelay(kNow + 1800);
    v6.relay_net = NetworkType::IPV6;
    v6.relay_addr.assign(16, 0);
    v6.relay_addr[15] = 1;  // ::1
    EXPECT_FALSE(VerifyHint(SignHint(*a, v6), kNow).has_value());
}
