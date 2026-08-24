#pragma once

#include <cstddef>

namespace dinero::p2p {

// Bitcoin-style topology: bootstrap endpoints are introductions and recovery
// fallbacks, not permanent members of every node's peer set. Once AddrMan has
// supplied a healthy community set, all durable slots are available to peers
// learned from the network. A feeler may temporarily connect above this target.
inline constexpr std::size_t kTargetDurableOutbound = 8;
inline constexpr std::size_t kBootstrapRecoveryOutbound = 2;
inline constexpr std::size_t kAutonomousCommunityThreshold = 4;

struct BootstrapAutonomyPolicy {
    bool autonomous{false};
    std::size_t max_bootstrap_hot{kBootstrapRecoveryOutbound};
};

constexpr BootstrapAutonomyPolicy EvaluateBootstrapAutonomy(
    std::size_t connected_community,
    std::size_t available_community) {
    const bool healthy = connected_community >= kAutonomousCommunityThreshold ||
                         (connected_community + available_community) >=
                             kAutonomousCommunityThreshold;
    return {healthy, healthy ? 0U : kBootstrapRecoveryOutbound};
}

static_assert(kTargetDurableOutbound == 8);

} // namespace dinero::p2p
