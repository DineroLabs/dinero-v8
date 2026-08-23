#pragma once

#include <cstddef>

namespace dinero::p2p {

// Transitional 3+3 topology: three operator-controlled recovery anchors stay
// connected while three independently selected AddrMan peers provide the
// decentralised edge. A feeler may temporarily connect above this target.
inline constexpr std::size_t kMandatoryAnchorOutbound = 3;
inline constexpr std::size_t kDynamicCommunityOutbound = 3;
inline constexpr std::size_t kTargetDurableOutbound =
    kMandatoryAnchorOutbound + kDynamicCommunityOutbound;

static_assert(kTargetDurableOutbound == 6);

} // namespace dinero::p2p
