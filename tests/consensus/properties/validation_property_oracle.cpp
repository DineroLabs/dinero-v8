#include "validation_property_oracle.h"

namespace dinero::consensus::test {

void ValidationPropertyOracle::reset() {
    violations_.clear();
}

void ValidationPropertyOracle::finalize() {
    // Default: no-op
    // Subclasses can override for end-of-trace checks
}

std::vector<ValidationViolation> ValidationPropertyOracle::check(const ValidationTrace& trace) {
    // Reset oracle state
    reset();

    // Process all events
    for (size_t i = 0; i < trace.events.size(); i++) {
        const ValidationEvent& event = trace.events[i];

        // Get corresponding state snapshot (or empty state if no snapshots)
        ValidationState state;
        if (!trace.snapshots.empty()) {
            // Find nearest snapshot (simple: use latest snapshot up to this event)
            size_t snapshot_idx = std::min(i, trace.snapshots.size() - 1);
            state = trace.snapshots[snapshot_idx];
        }

        // Let oracle observe this event
        observe(state, event, i);
    }

    // Final check
    finalize();

    // Return accumulated violations
    return violations_;
}

void ValidationPropertyOracle::reportViolation(
    const std::string& property,
    const std::string& message,
    uint64_t event_index
) {
    violations_.emplace_back(property, message, event_index);
}

} // namespace dinero::consensus::test
