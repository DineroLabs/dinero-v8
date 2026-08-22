#pragma once

#include <cstddef>

namespace dinero::p2p {

// Five durable peers preserve the three recovery anchors plus room for two
// independent nodes. A feeler may temporarily connect above this target.
inline constexpr std::size_t kTargetDurableOutbound = 5;

} // namespace dinero::p2p
