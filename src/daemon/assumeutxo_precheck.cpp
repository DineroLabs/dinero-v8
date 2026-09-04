#include "daemon/assumeutxo_precheck.h"

namespace dinero {
namespace daemon {

bool IsHistoricalPreBaseBody(const AssumeUTXOPrecheckContext& ctx,
                             uint32_t parent_height) {
    return ctx.assumeutxo_active && parent_height < ctx.base_height;
}

bool IsDeferredSnapshotBaseChild(const AssumeUTXOPrecheckContext& ctx,
                                 uint32_t parent_height,
                                 const dinero::uint256& parent_hash) {
    // Deliberately NOT `parent_height <= base_height`. A global relaxation
    // would exempt any block claiming a parent at the base, including a fork
    // that never touched it. Every conjunct below is load-bearing:
    //
    //   assumeutxo_active        - normal sync keeps every existing check
    //   !forward_connect_enabled - in forward-connect the tip legitimately
    //                              passes the base, so the child is handled
    //                              normally and must not be exempted
    //   parent_height == base    - exactly the boundary, never above it
    //   parent_hash == base      - the parent IS the compiled-in trust
    //                              anchor, which is the whole justification:
    //                              "is the parent on the main chain?" is
    //                              already answered for it
    //
    // A null base means no snapshot base is established; comparing against it
    // would exempt anything that failed to parse, so refuse.
    return ctx.assumeutxo_active &&
           !ctx.forward_connect_enabled &&
           parent_height == ctx.base_height &&
           !ctx.base_block.IsNull() &&
           parent_hash == ctx.base_block;
}

bool ShouldSkipSideChainPrecheck(const AssumeUTXOPrecheckContext& ctx,
                                 uint32_t parent_height,
                                 const dinero::uint256& parent_hash) {
    return IsHistoricalPreBaseBody(ctx, parent_height) ||
           IsDeferredSnapshotBaseChild(ctx, parent_height, parent_hash);
}

}  // namespace daemon
}  // namespace dinero
