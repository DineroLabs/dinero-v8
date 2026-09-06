// Fork-below-base guard: is a reorg crossing the snapshot boundary fatal?
//
// Spec (assumeutxo-fatal-state-machine.md, Fatal Mismatch Semantics): a
// higher-work chain diverging BELOW the snapshot base must go fatal, not
// silently reorg. Mechanically, undo below the base may not exist -- promotion
// persists only the audited tail -- so the disconnect would fail anyway;
// classifying it as the proof failure it is beats failing obscurely later.
//
// Extracted as a pure predicate so the boundary cases can be enumerated in a
// unit test. The interesting ones are all off-by-one: a fork exactly AT the
// base is legal, one block below it is fatal, and a pure extension from a tip
// that happens to sit below the base is not a reorg at all.
#pragma once

#include <cstdint>

namespace dinero {
namespace consensus {

/// The snapshot-boundary facts the decision depends on.
struct ForkGuardContext {
    bool assumeutxo_active = false;
    /// Live base; meaningful only while assumeutxo_active.
    uint32_t assumeutxo_base_height = 0;
    /// Set on promotion success, restored at startup from the FullyValidated
    /// lifecycle record, and NEVER cleared. After promotion the exit gate
    /// clears assumeutxo_active_/assumeutxo_base_height_, but a below-base fork
    /// is still fatal -- which is why the rule is not mode-scoped.
    uint32_t promoted_base_height = 0;
};

/// max(live base, promoted base). Zero means no snapshot boundary exists.
uint32_t EffectiveSnapshotBase(const ForkGuardContext& ctx);

/// True when a reorg to `fork_point_height` must be treated as fatal.
///
/// `has_fork_point`  — false when no fork point was resolved.
/// `fork_is_active_tip` — true when the fork point IS the current tip, i.e. a
///     pure extension that disconnects nothing. That is not a reorg below the
///     base, and going fatal there would brick honest nodes during a transient
///     genesis-tip bootstrap window.
bool IsForkBelowSnapshotBaseFatal(const ForkGuardContext& ctx,
                                  bool has_fork_point,
                                  bool fork_is_active_tip,
                                  int fork_point_height);

}  // namespace consensus
}  // namespace dinero
