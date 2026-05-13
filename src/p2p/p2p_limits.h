#pragma once

#include <cstddef>
#include <cstdint>

// P2P Message Safety Limits
// Prevents memory exhaustion attacks and ensures stable peer communication

namespace dinero {
namespace p2p {

// Maximum size of a single P2P message payload (4 MiB)
// This prevents memory allocation failures when receiving large messages
constexpr std::size_t P2P_MAX_MSG_BYTES = 4 * 1024 * 1024;

// Maximum number of hashes in a single INV message (2000 hashes)
// Prevents excessive memory usage from string concatenation in text protocol
// At 64 chars per hash + 1 comma = ~130KB payload (well under P2P_MAX_MSG_BYTES)
constexpr std::size_t P2P_MAX_INV_HASHES_PER_MSG = 2000;

// Maximum outbound queue size per peer (8 MiB)
// Prevents slow peers from causing unbounded memory growth
// If peer's queue exceeds this, new messages are dropped for that peer
constexpr std::size_t P2P_MAX_OUTBOUND_QUEUE_BYTES = 8 * 1024 * 1024;

// Maximum number of messages in the global outbox queue
// This is the existing limit from p2p_manager.h
constexpr std::size_t P2P_MAX_OUTBOX_SIZE = 10000;

} // namespace p2p
} // namespace dinero
