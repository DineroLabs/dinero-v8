// Copyright (c) 2026 The Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "network/relay_registry.h"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstdint>

using dinero::network::RelayRegistration;
using dinero::network::RelayRegistry;
using std::chrono::hours;
using std::chrono::seconds;

namespace {

std::array<uint8_t, 20> NodeId(uint8_t seed) {
    std::array<uint8_t, 20> id{};
    for (size_t i = 0; i < id.size(); ++i) {
        id[i] = static_cast<uint8_t>(seed + i);
    }
    return id;
}

RelayRegistration Registration(uint8_t seed,
                               const std::string& peer_address) {
    RelayRegistration reg;
    reg.node_id = NodeId(seed);
    for (size_t i = 0; i < reg.pubkey.size(); ++i) {
        reg.pubkey[i] = static_cast<uint8_t>(seed + i + 1);
    }
    reg.peer_address = peer_address;
    reg.expires_at = std::chrono::steady_clock::now() + hours(1);
    return reg;
}

}  // namespace

TEST(RelayRegistryGraceTest, active_registration_is_lookup_and_snapshot_visible) {
    RelayRegistry registry;
    const auto reg = Registration(0x10, "198.51.100.10:20999");

    ASSERT_TRUE(registry.Register(reg));

    EXPECT_TRUE(registry.Lookup(reg.node_id).has_value());
    EXPECT_EQ(registry.SnapshotValid().size(), 1u);
    EXPECT_EQ(registry.size(), 1u);
    EXPECT_EQ(registry.grace_pending_count(), 0u);
}

TEST(RelayRegistryGraceTest, disconnect_grace_hides_registration_without_erasing) {
    RelayRegistry registry;
    const auto reg = Registration(0x20, "198.51.100.11:20999");
    ASSERT_TRUE(registry.Register(reg));

    const size_t marked = registry.MarkGracePendingByPeerAddress(
        reg.peer_address,
        std::chrono::steady_clock::now() + seconds(90));

    EXPECT_EQ(marked, 1u);
    EXPECT_FALSE(registry.Lookup(reg.node_id).has_value());
    EXPECT_TRUE(registry.SnapshotValid().empty());
    EXPECT_EQ(registry.size(), 1u);
    EXPECT_EQ(registry.grace_pending_count(), 1u);
}

TEST(RelayRegistryGraceTest, reconnect_register_clears_grace_and_restores_visibility) {
    RelayRegistry registry;
    auto reg = Registration(0x30, "198.51.100.12:20999");
    ASSERT_TRUE(registry.Register(reg));
    ASSERT_TRUE(registry.MarkGracePending(
        reg.node_id,
        std::chrono::steady_clock::now() + seconds(90)));
    ASSERT_FALSE(registry.Lookup(reg.node_id).has_value());

    reg.peer_address = "198.51.100.12:21000";
    reg.expires_at = std::chrono::steady_clock::now() + hours(2);
    ASSERT_TRUE(registry.Register(reg));

    auto restored = registry.Lookup(reg.node_id);
    ASSERT_TRUE(restored.has_value());
    EXPECT_EQ(restored->peer_address, "198.51.100.12:21000");
    EXPECT_EQ(registry.SnapshotValid().size(), 1u);
    EXPECT_EQ(registry.grace_pending_count(), 0u);
}

TEST(RelayRegistryGraceTest, sweep_removes_entries_after_disconnect_grace_expires) {
    RelayRegistry registry;
    const auto reg = Registration(0x40, "198.51.100.13:20999");
    ASSERT_TRUE(registry.Register(reg));
    ASSERT_TRUE(registry.MarkGracePending(
        reg.node_id,
        std::chrono::steady_clock::now() - seconds(1)));

    EXPECT_EQ(registry.Sweep(), 1u);
    EXPECT_EQ(registry.size(), 0u);
    EXPECT_FALSE(registry.Lookup(reg.node_id).has_value());
}

TEST(RelayRegistryGraceTest, sweep_still_removes_hard_expired_entries) {
    RelayRegistry registry;
    auto reg = Registration(0x50, "198.51.100.14:20999");
    reg.expires_at = std::chrono::steady_clock::now() - seconds(1);
    ASSERT_TRUE(registry.Register(reg));

    EXPECT_EQ(registry.Sweep(), 1u);
    EXPECT_EQ(registry.size(), 0u);
}

