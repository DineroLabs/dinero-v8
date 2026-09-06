// Regression coverage for the side-chain precheck exemptions.
//
// Background: under AssumeUTXO, BlockAcceptor decides "main-chain extension"
// by comparing the block's parent to ChainDB::getTip(). In classic deferred
// mode ChainDB's tip still trails the snapshot base during background replay,
// so the canonical child of the base is misclassified as a side-chain block.
// The side-chain branch then calls RestoreHistoricalForest on every delivery
// of that block — observed 250,135 times for height 99678 against a base of
// 99677 — on a scheduler thread, starving DispatchDeferredSends() until peer
// request deadlines expired and the download stalled at ~6 KB/s.
//
// The exemption is deliberately NOT a global `<` -> `<=` relaxation: it is
// restricted to classic deferred mode and to a parent that IS the base.
#include <gtest/gtest.h>

#include "daemon/assumeutxo_precheck.h"
#include "daemon/services/assumeutxo_state.h"

using dinero::uint256;
using dinero::daemon::AssumeUTXOPrecheckContext;
using dinero::daemon::IsDeferredSnapshotBaseChild;
using dinero::daemon::IsHistoricalPreBaseBody;
using dinero::daemon::ShouldSkipSideChainPrecheck;

namespace {

constexpr uint32_t kBaseHeight = 99677;

uint256 BaseBlock() {
    return uint256::FromHexUnsafe(
        "00000000000000000000000000000000000000000000000000000000deadbeef");
}

uint256 OtherBlock() {
    return uint256::FromHexUnsafe(
        "00000000000000000000000000000000000000000000000000000000feedface");
}

// Classic deferred mode: snapshot active, forward-connect OFF.
AssumeUTXOPrecheckContext DeferredCtx() {
    AssumeUTXOPrecheckContext ctx;
    ctx.assumeutxo_active = true;
    ctx.forward_connect_enabled = false;
    ctx.base_height = kBaseHeight;
    ctx.base_block = BaseBlock();
    return ctx;
}

AssumeUTXOPrecheckContext ForwardConnectCtx() {
    AssumeUTXOPrecheckContext ctx = DeferredCtx();
    ctx.forward_connect_enabled = true;
    return ctx;
}

}  // namespace

// Case 1: the bug. Deferred mode, block at base+1 whose parent is exactly the
// snapshot base => exempt, so RestoreHistoricalForest is never reached.
TEST(AssumeUTXOPrecheck, DeferredBaseChildIsExempt) {
    EXPECT_TRUE(IsDeferredSnapshotBaseChild(DeferredCtx(), kBaseHeight, BaseBlock()));
    EXPECT_TRUE(ShouldSkipSideChainPrecheck(DeferredCtx(), kBaseHeight, BaseBlock()));
}

// Case 2: the exemption keys on the PARENT, not on the block. A block at
// base+1 whose parent is NOT the base is a genuine side chain and keeps the
// full precheck.
//
// Note the honest limit: two distinct siblings that both descend from the
// canonical base both satisfy this predicate. That is unavoidable here --
// in deferred mode there is no canonical base+1 yet, which is the bug itself.
// Storing a sibling is safe because storage is not connection; ConnectBlock
// hard-rejects a bad Utreexo root when the chain is actually connected.
TEST(AssumeUTXOPrecheck, BaseHeightParentThatIsNotTheBaseIsNotExempt) {
    EXPECT_FALSE(IsDeferredSnapshotBaseChild(DeferredCtx(), kBaseHeight, OtherBlock()));
    EXPECT_FALSE(ShouldSkipSideChainPrecheck(DeferredCtx(), kBaseHeight, OtherBlock()));
}

// Case 3: forward-connect mode is a different regime -- the tip legitimately
// advances past the base, so the canonical base child is handled normally and
// must still get the ordinary Utreexo path.
TEST(AssumeUTXOPrecheck, ForwardConnectBaseChildIsNotExempt) {
    EXPECT_FALSE(IsDeferredSnapshotBaseChild(ForwardConnectCtx(), kBaseHeight, BaseBlock()));
    EXPECT_FALSE(ShouldSkipSideChainPrecheck(ForwardConnectCtx(), kBaseHeight, BaseBlock()));
}

// Case 4 (predicate half): nothing is exempt when AssumeUTXO is not active.
// A normally-syncing node keeps every existing check.
TEST(AssumeUTXOPrecheck, NothingIsExemptWhenAssumeUTXOInactive) {
    AssumeUTXOPrecheckContext ctx = DeferredCtx();
    ctx.assumeutxo_active = false;
    EXPECT_FALSE(IsDeferredSnapshotBaseChild(ctx, kBaseHeight, BaseBlock()));
    EXPECT_FALSE(IsHistoricalPreBaseBody(ctx, kBaseHeight - 1));
    EXPECT_FALSE(ShouldSkipSideChainPrecheck(ctx, kBaseHeight, BaseBlock()));
}

// The exemption must not widen into a `<=`: a parent ABOVE the base is past
// the snapshot boundary entirely and is never exempt.
TEST(AssumeUTXOPrecheck, ParentAboveBaseIsNeverExempt) {
    EXPECT_FALSE(ShouldSkipSideChainPrecheck(DeferredCtx(), kBaseHeight + 1, BaseBlock()));
    EXPECT_FALSE(ShouldSkipSideChainPrecheck(DeferredCtx(), kBaseHeight + 500, BaseBlock()));
}

// Pre-existing behaviour, pinned so the refactor into a named predicate does
// not change it: pre-base historical bodies stay exempt.
TEST(AssumeUTXOPrecheck, PreBaseHistoricalBodyStaysExempt) {
    EXPECT_TRUE(IsHistoricalPreBaseBody(DeferredCtx(), kBaseHeight - 1));
    EXPECT_TRUE(ShouldSkipSideChainPrecheck(DeferredCtx(), 0, OtherBlock()));
    // ...and in forward-connect mode too, which the old inline guard allowed.
    EXPECT_TRUE(IsHistoricalPreBaseBody(ForwardConnectCtx(), kBaseHeight - 1));
}

// A null base block means the snapshot base is not established. Comparing a
// parent hash against it would exempt anything that failed to parse; refuse.
TEST(AssumeUTXOPrecheck, NullBaseBlockNeverExempts) {
    AssumeUTXOPrecheckContext ctx = DeferredCtx();
    ctx.base_block = uint256();
    EXPECT_FALSE(IsDeferredSnapshotBaseChild(ctx, kBaseHeight, uint256()));
}

// ── the drain-ceiling lifecycle ───────────────────────────────────────────
//
// The ceiling was armed only at the tail of a missing-bodies pass, deep inside
// BackgroundValidationWorker, AFTER the `if (blocks_skipped == 0) break;` that
// a completing pass takes. So the normal path -- replay finishes -- returned
// without ever clearing it, and the scheduler refused to drain above the
// snapshot base for the life of the process. Post-promotion that permanently
// disables catch-up while the ceiling's own 60s log claims replay is still
// running; only a restart recovers.
//
// Deriving it from state, as a pure function, is what makes the DISARM half
// checkable at all. These cases pin both halves.
TEST(DeferredDrainCeiling, AppliesOnlyInClassicDeferredMode) {
    using dinero::daemon::ShouldApplyDeferredDrainCeiling;
    EXPECT_TRUE(ShouldApplyDeferredDrainCeiling(/*active=*/true,
                                                /*forward_connect=*/false))
        << "classic deferred mode is exactly where the ceiling belongs";
    EXPECT_FALSE(ShouldApplyDeferredDrainCeiling(true, true))
        << "forward-connect: the tip legitimately passes the base";
}

// THE case. Snapshot state gone -> ceiling gone. Every mode exit runs through
// assumeutxo::ClearState, which sets active=false, so a ceiling derived from
// that flag cannot outlive the mode. The bug was that nothing re-derived it.
TEST(DeferredDrainCeiling, IsLoweredOnceSnapshotStateIsCleared) {
    using dinero::daemon::ShouldApplyDeferredDrainCeiling;

    bool active = true;
    dinero::uint256 base_block = BaseBlock();
    uint32_t base_height = kBaseHeight;
    ASSERT_TRUE(ShouldApplyDeferredDrainCeiling(active, false))
        << "precondition: armed while the snapshot is live";

    dinero::assumeutxo::ClearState(
        {active, base_block, base_height, /*utxo_index=*/nullptr},
        /*clear_persisted_metadata=*/false);

    EXPECT_FALSE(active) << "ClearState must retire the mode flag";
    EXPECT_FALSE(ShouldApplyDeferredDrainCeiling(active, false))
        << "a ceiling that outlives the snapshot stops every drain above a "
           "base that no longer exists";
}
