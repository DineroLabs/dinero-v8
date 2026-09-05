// Monotonic generation for operator decisions about block validity.
//
// The bug this exists for: BlockAcceptor deliberately preserves a block's
// BLOCK_FAILED_VALID / BLOCK_FAILED_CHILD flags when a peer re-relays a
// previously-invalidated block (Bug #6/#38), so a relay cannot silently undo an
// invalidation. Correct in isolation. But a block that is re-announced
// continuously — 571 to 667 deliveries were measured for a single height —
// always has another relay in flight, so after `reconsiderblock` clears the
// flags the very next relay re-asserts them. Measured: 650 re-assertions AFTER
// the reconsider, and the tip never recovered. An operator invalidation became
// effectively irreversible.
//
// The rule: a stale block-processing result may never overwrite a NEWER
// operator decision.
//
// Mechanism: a monotonic counter bumped by every invalidate and every
// reconsider, persisted with the decision. Acceptance captures the generation
// BEFORE it starts work and may only preserve or write failure flags if the
// generation is unchanged at commit time.
//
//   relay begins ........ gen=7, captured
//   reconsiderblock ..... gen=8, flags cleared, persisted
//   relay commits ....... captured 7 != current 8 -> STALE, flags NOT reasserted
//
//   reconsiderblock ..... gen=8
//   relay begins ........ gen=8, captured
//   block truly invalid . gen still 8 -> failure flags written normally
//
// Correctness must not depend on cache residency, timing, or process lifetime,
// so this is durable state, not an in-memory optimisation. The duplicate-
// announcement LRU (docs/specs/duplicate_announcement_suppression.md) sits
// ABOVE this rule as an efficiency measure and never substitutes for it.
#pragma once

#include <cstdint>
#include <limits>
#include <optional>
#include <string>

namespace dinero {
namespace consensus {

/// ChainDB utreexo-meta key holding the decimal generation counter.
inline constexpr const char* kBlockStatusGenerationKey = "block_status_generation";

/// Generation 0 means "never recorded"; any real decision produces >= 1, so a
/// captured 0 can be distinguished from a genuine early generation.
using BlockStatusGeneration = uint64_t;

/// True when a result captured at `captured` is still authoritative at
/// `current`. Stale results must not write or preserve failure flags.
inline bool GenerationStillCurrent(BlockStatusGeneration captured,
                                   BlockStatusGeneration current) {
    return captured == current;
}

/// The next generation, or nullopt when the counter is saturated.
///
/// Saturation must REFUSE rather than wrap. Wrapping to 0 would make every
/// in-flight result read stale forever, and a later wrap could make a genuinely
/// stale capture compare EQUAL to the current value — the one outcome the whole
/// mechanism exists to prevent. Extracted so the refusal is unit-testable
/// instead of only reasoned about at the call site.
std::optional<BlockStatusGeneration> NextGeneration(BlockStatusGeneration current);

/// Parse a persisted counter. Malformed or absent reads as 0, which is never
/// equal to a bumped generation — so an unreadable counter fails CLOSED
/// (results treated as stale) rather than silently allowing a stale write.
BlockStatusGeneration ParseBlockStatusGeneration(const std::string& raw);

/// Serialize for persistence.
std::string FormatBlockStatusGeneration(BlockStatusGeneration gen);

}  // namespace consensus
}  // namespace dinero
