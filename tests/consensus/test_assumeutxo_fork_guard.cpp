// Snapshot/promotion boundary: which reorgs are fatal.
//
// Every interesting case here is an off-by-one or a mode transition, which is
// exactly what integration tests struggle to enumerate: a fork AT the base is
// legal, one block below it is fatal, and the rule must survive promotion
// clearing assumeutxo_active_.
#include <gtest/gtest.h>

#include "consensus/assumeutxo_fork_guard.h"

using dinero::consensus::EffectiveSnapshotBase;
using dinero::consensus::ForkGuardContext;
using dinero::consensus::IsForkBelowSnapshotBaseFatal;

namespace {

constexpr uint32_t kBase = 99677;

/// Snapshot loaded, replay not finished: live base set, nothing promoted.
ForkGuardContext BeforePromotion() {
    return ForkGuardContext{/*active=*/true, /*base=*/kBase, /*promoted=*/0};
}

/// Promotion done and the exit gate has cleared the live mode. Only the
/// never-cleared promoted height remains — the case the rule must survive.
ForkGuardContext AfterPromotion() {
    return ForkGuardContext{/*active=*/false, /*base=*/0, /*promoted=*/kBase};
}

/// No snapshot at all.
ForkGuardContext NormalSync() {
    return ForkGuardContext{/*active=*/false, /*base=*/0, /*promoted=*/0};
}

bool Fatal(const ForkGuardContext& c, int fork_height) {
    return IsForkBelowSnapshotBaseFatal(c, /*has_fork_point=*/true,
                                        /*fork_is_active_tip=*/false, fork_height);
}

}  // namespace

// --- the boundary itself ---------------------------------------------------

TEST(ForkGuard, ForkExactlyAtTheBaseIsLegal) {
    // The off-by-one that matters. base-1 is fatal, base is not: the base
    // block itself is proven, so a fork rooted there disconnects nothing below
    // the audited tail.
    EXPECT_FALSE(Fatal(BeforePromotion(), static_cast<int>(kBase)));
    EXPECT_FALSE(Fatal(AfterPromotion(), static_cast<int>(kBase)));
}

TEST(ForkGuard, ForkOneBlockBelowTheBaseIsFatal) {
    EXPECT_TRUE(Fatal(BeforePromotion(), static_cast<int>(kBase) - 1));
    EXPECT_TRUE(Fatal(AfterPromotion(), static_cast<int>(kBase) - 1));
}

TEST(ForkGuard, ForkAboveTheBaseIsAnOrdinaryReorg) {
    for (int d : {1, 2, 100, 5000}) {
        EXPECT_FALSE(Fatal(BeforePromotion(), static_cast<int>(kBase) + d)) << "d=" << d;
        EXPECT_FALSE(Fatal(AfterPromotion(), static_cast<int>(kBase) + d)) << "d=" << d;
    }
}

// --- the rule must survive promotion --------------------------------------

TEST(ForkGuard, RuleSurvivesPromotionClearingTheLiveMode) {
    // After the exit gate, assumeutxo_active_ and assumeutxo_base_height_ are
    // cleared. If the guard were mode-scoped it would silently stop protecting
    // exactly when the node starts serving as a fully-validated peer.
    const auto after = AfterPromotion();
    EXPECT_FALSE(after.assumeutxo_active);
    EXPECT_EQ(after.assumeutxo_base_height, 0u);
    EXPECT_EQ(EffectiveSnapshotBase(after), kBase);
    EXPECT_TRUE(Fatal(after, static_cast<int>(kBase) - 1));
}

TEST(ForkGuard, EffectiveBaseTakesTheHigherOfLiveAndPromoted) {
    // A second, higher snapshot may be active while an older promotion is on
    // record. The stricter boundary must win.
    ForkGuardContext both{/*active=*/true, /*base=*/kBase + 1000, /*promoted=*/kBase};
    EXPECT_EQ(EffectiveSnapshotBase(both), kBase + 1000);
    EXPECT_TRUE(Fatal(both, static_cast<int>(kBase) + 500));

    ForkGuardContext promoted_higher{/*active=*/true, /*base=*/kBase,
                                     /*promoted=*/kBase + 1000};
    EXPECT_EQ(EffectiveSnapshotBase(promoted_higher), kBase + 1000);
    EXPECT_TRUE(Fatal(promoted_higher, static_cast<int>(kBase) + 500));
}

// --- cases that must NOT be fatal -----------------------------------------

TEST(ForkGuard, NormalSyncNeverGoesFatal) {
    for (int h : {0, 1, 99676, 99677, 1000000}) {
        EXPECT_FALSE(Fatal(NormalSync(), h)) << "height " << h;
    }
    EXPECT_EQ(EffectiveSnapshotBase(NormalSync()), 0u);
}

TEST(ForkGuard, PureExtensionFromABelowBaseTipIsNotAReorg) {
    // fork_point == active_tip: nothing is disconnected. This happens in the
    // transient genesis-tip window during bootstrap; going fatal there would
    // brick honest nodes.
    EXPECT_FALSE(IsForkBelowSnapshotBaseFatal(BeforePromotion(),
                                              /*has_fork_point=*/true,
                                              /*fork_is_active_tip=*/true,
                                              /*fork_point_height=*/0));
    EXPECT_FALSE(IsForkBelowSnapshotBaseFatal(AfterPromotion(),
                                              /*has_fork_point=*/true,
                                              /*fork_is_active_tip=*/true,
                                              static_cast<int>(kBase) - 1));
}

TEST(ForkGuard, NoForkPointIsNotFatal) {
    EXPECT_FALSE(IsForkBelowSnapshotBaseFatal(BeforePromotion(),
                                              /*has_fork_point=*/false,
                                              /*fork_is_active_tip=*/false,
                                              /*fork_point_height=*/0));
}

TEST(ForkGuard, GenesisForkIsFatalOnlyWhenASnapshotBoundaryExists) {
    EXPECT_TRUE(Fatal(BeforePromotion(), 0));
    EXPECT_TRUE(Fatal(AfterPromotion(), 0));
    EXPECT_FALSE(Fatal(NormalSync(), 0));
}
