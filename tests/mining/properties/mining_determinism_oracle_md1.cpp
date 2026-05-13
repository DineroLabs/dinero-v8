#include "mining_determinism_oracle_md1.h"
#include <sstream>

// Ring 4 Phase 4f: MD1 - Same Seed → Identical Trace Implementation

namespace mining_test {

// ============================================================================
// Oracle Interface
// ============================================================================

std::string MD1Oracle::name() const {
    return "MD1: Same Seed → Identical Trace";
}

std::vector<DeterminismViolation> MD1Oracle::check(
    const MiningTrace& reference,
    const MiningTrace& candidate
) const {
    std::vector<DeterminismViolation> violations;

    // Fast path: compare trace hashes
    if (!compareTraceHashes(reference, candidate, violations)) {
        return violations;  // Hash mismatch detected
    }

    // Detailed path: compare events
    if (!compareEvents(reference, candidate, violations)) {
        return violations;  // Event divergence detected
    }

    // Detailed path: compare states
    if (!compareStates(reference, candidate, violations)) {
        return violations;  // State divergence detected
    }

    // Traces are identical
    return violations;
}

// ============================================================================
// Comparison Methods
// ============================================================================

bool MD1Oracle::compareTraceHashes(
    const MiningTrace& reference,
    const MiningTrace& candidate,
    std::vector<DeterminismViolation>& violations
) const {
    // Compare final trace hashes (fast check)
    if (reference.final_hash != candidate.final_hash) {
        std::ostringstream msg;
        msg << "Trace hash mismatch: reference=0x" << std::hex << reference.final_hash
            << ", candidate=0x" << candidate.final_hash;

        violations.emplace_back("MD1", msg.str(), 0);
        return false;
    }

    return true;
}

bool MD1Oracle::compareEvents(
    const MiningTrace& reference,
    const MiningTrace& candidate,
    std::vector<DeterminismViolation>& violations
) const {
    // Check event count
    if (reference.events.size() != candidate.events.size()) {
        std::ostringstream msg;
        msg << "Event count mismatch: reference=" << reference.events.size()
            << ", candidate=" << candidate.events.size();

        violations.emplace_back("MD1", msg.str(), 0);
        return false;
    }

    // Compare events element-by-element
    const size_t count = reference.events.size();
    for (size_t i = 0; i < count; ++i) {
        const auto& ev_ref = reference.events[i];
        const auto& ev_cand = candidate.events[i];

        if (!eventsEqual(ev_ref, ev_cand)) {
            std::ostringstream msg;
            msg << "Event divergence at index " << i
                << ": reference type=" << static_cast<int>(ev_ref.type)
                << ", candidate type=" << static_cast<int>(ev_cand.type);

            violations.emplace_back("MD1", msg.str(), i);
            return false;
        }
    }

    return true;
}

bool MD1Oracle::compareStates(
    const MiningTrace& reference,
    const MiningTrace& candidate,
    std::vector<DeterminismViolation>& violations
) const {
    // Check snapshot count
    if (reference.snapshots.size() != candidate.snapshots.size()) {
        std::ostringstream msg;
        msg << "State snapshot count mismatch: reference=" << reference.snapshots.size()
            << ", candidate=" << candidate.snapshots.size();

        violations.emplace_back("MD1", msg.str(), 0);
        return false;
    }

    // Compare states element-by-element
    const size_t count = reference.snapshots.size();
    for (size_t i = 0; i < count; ++i) {
        const auto& st_ref = reference.snapshots[i];
        const auto& st_cand = candidate.snapshots[i];

        if (!statesEqual(st_ref, st_cand)) {
            std::ostringstream msg;
            msg << "State divergence at index " << i
                << ": phase or timestamp mismatch";

            violations.emplace_back("MD1", msg.str(), i);
            return false;
        }
    }

    return true;
}

// ============================================================================
// Helper Methods
// ============================================================================

bool MD1Oracle::eventsEqual(
    const MiningEvent& a,
    const MiningEvent& b
) const {
    // Use existing operator== from MiningEvent
    return a == b;
}

bool MD1Oracle::statesEqual(
    const MiningState& a,
    const MiningState& b
) const {
    // Use existing operator== from MiningState
    return a == b;
}

}  // namespace mining_test
