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

/// Which failure flags a block-acceptance result may carry forward.
///
/// Pure, so the decision can be tested without a daemon: a stale result
/// (captured != current) carries NOTHING forward, a current one carries the
/// flags it observed.
inline uint32_t PreservedFailureFlags(BlockStatusGeneration captured,
                                      BlockStatusGeneration current,
                                      uint32_t observed_failure_flags) {
    return GenerationStillCurrent(captured, current) ? observed_failure_flags : 0u;
}

/// Three outcomes of reading the persisted counter, kept apart.
///
/// Collapsing them into a bare number was the defect: "read failed" and
/// "the counter is 0" produced the same value, and 0 is what a chain reads
/// before any operator decision has ever been made. GenerationStillCurrent(0,0)
/// is true, so on a fresh chain a failed read was indistinguishable from a
/// confirmed-current one.
enum class GenerationReadState : uint8_t {
    Present = 0,  ///< the key exists and parsed
    Absent  = 1,  ///< no key yet: a legitimate generation 0
    Error   = 2,  ///< the read itself failed: nothing may be concluded
};

struct GenerationRead {
    GenerationReadState state = GenerationReadState::Error;
    BlockStatusGeneration value = 0;

    /// True when the value may be compared. Absent counts: it is a real 0.
    bool usable() const { return state != GenerationReadState::Error; }
};

/// Which failure flags survive, given reads that may have FAILED.
///
/// Fails closed in both directions by declining to act on unknown state:
///
///   * INVALIDATION — if either read failed, the observed failure flags are
///     PRESERVED. Dropping them clears persisted invalidity and lets a block
///     an operator ruled invalid re-enter the candidate set; nothing later
///     undoes that. A read failure must never clear persisted invalidity.
///
///   * RECONSIDERATION — preserving is also not a re-assertion: it carries
///     forward exactly what was already on disk and writes no new decision.
///     A reconsider that raced a failed read is not silently undone, it is
///     simply not observed, and the operator can re-issue it. That is
///     recoverable; a cleared invalidity is not.
///
/// The asymmetry is deliberate. Both errors are bad, but only one is
/// permanent, so uncertainty resolves toward the recoverable side.
inline uint32_t PreservedFailureFlagsFromReads(const GenerationRead& captured,
                                               const GenerationRead& now,
                                               uint32_t observed_failure_flags) {
    if (!captured.usable() || !now.usable()) {
        return observed_failure_flags;  // preserve: never clear on uncertainty
    }
    return PreservedFailureFlags(captured.value, now.value, observed_failure_flags);
}

/// The compare-then-commit sequence, with the window between them made
/// explicit so a test can act inside it.
///
/// This models exactly what ConnectBlock does: read the current generation,
/// decide which flags survive, then commit. `in_window` runs BETWEEN those two
/// steps — the TOCTOU window that ReconsiderBlock's activation_mutex_ exists to
/// eliminate. In production nothing can run there because the whole sequence
/// holds that lock; in a test, `in_window` is where the damaging interleaving
/// is injected.
///
/// Templated so tests can supply fakes with no daemon, no ChainDB, and no
/// ActivateBestChain to mask the write — the surrounding machinery is exactly
/// what made this unprovable at integration level.
template <typename ReadGeneration, typename InWindow, typename CommitFlags>
void CompareThenCommitFailureFlags(BlockStatusGeneration captured,
                                   ReadGeneration read_generation,
                                   uint32_t observed_failure_flags,
                                   InWindow in_window,
                                   CommitFlags commit) {
    const BlockStatusGeneration current = read_generation();
    const uint32_t preserved =
        PreservedFailureFlags(captured, current, observed_failure_flags);
    in_window();          // <-- the race window
    commit(preserved);
}

/// Parse a persisted counter. Malformed or absent reads as 0.
///
/// NOTE: 0 is also a LEGITIMATE generation — a chain on which no operator
/// invalidate/reconsider has ever run reads 0 — so this value alone cannot
/// tell "the counter says 0" from "the counter could not be read". Callers
/// making a decision must use GenerationRead (above), which keeps those apart.
BlockStatusGeneration ParseBlockStatusGeneration(const std::string& raw);

/// Serialize for persistence.
std::string FormatBlockStatusGeneration(BlockStatusGeneration gen);

}  // namespace consensus
}  // namespace dinero
