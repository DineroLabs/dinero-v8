#include "consensus/assumeutxo_fork_guard.h"

#include <algorithm>

namespace dinero {
namespace consensus {

uint32_t EffectiveSnapshotBase(const ForkGuardContext& ctx) {
    return std::max(ctx.assumeutxo_active ? ctx.assumeutxo_base_height : 0u,
                    ctx.promoted_base_height);
}

bool IsForkBelowSnapshotBaseFatal(const ForkGuardContext& ctx,
                                  bool has_fork_point,
                                  bool fork_is_active_tip,
                                  int fork_point_height) {
    const uint32_t effective_base = EffectiveSnapshotBase(ctx);
    // Every conjunct is load-bearing:
    //   has_fork_point        - nothing to classify without one
    //   !fork_is_active_tip   - a pure extension disconnects nothing
    //   effective_base > 0    - no snapshot, no boundary
    //   height < base         - AT the base is legal; below it is not
    return has_fork_point && !fork_is_active_tip && effective_base > 0 &&
           fork_point_height < static_cast<int>(effective_base);
}

}  // namespace consensus
}  // namespace dinero
