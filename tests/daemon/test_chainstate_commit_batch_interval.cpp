// Forest checkpoint delta campaign — phase 1
// (docs/design/forest-checkpoint-deltas.md).
//
// Every-N checkpoint gating at the commit-batch contract level. With
// utreexo_checkpoint_interval > 1, the full forest checkpoint is required
// only at interval heights; the ForestTipMarker and the per-block Utreexo
// delta sidecar stay required EVERY block (they are what phase 2's
// replay-restore and today's DisconnectTip depend on).

#include "daemon/chainstate_commit_batch.h"

#include <gtest/gtest.h>

#include <optional>
#include <string>

using dinero::ChainstateCommitBatch;

namespace {

// Stage everything except the utreexo trio, then opt pieces back in.
void StageBaseline(ChainstateCommitBatch& ccb) {
    ccb.MarkUtxoStaged();
    ccb.MarkTxIndexStaged();
    ccb.MarkBlockIndexStaged();
    ccb.MarkSetTipStaged();
    ccb.MarkHeightIndexStaged();
}

TEST(ChainstateCommitBatchInterval, DefaultIntervalRequiresCheckpointEveryBlock) {
    ChainstateCommitBatch ccb(/*tip_height=*/501, /*utreexo_active=*/true,
                              /*utreexo_stateless=*/false,
                              /*shielded_active=*/false);
    StageBaseline(ccb);
    ccb.MarkUtreexoForestTipMarkerStaged();
    ccb.MarkUtreexoDeltaStaged();

    const auto missing = ccb.AllRequiredStaged();
    ASSERT_TRUE(missing.has_value());
    EXPECT_EQ(*missing, "utreexo_checkpoint");

    ccb.MarkUtreexoCheckpointStaged();
    EXPECT_FALSE(ccb.AllRequiredStaged().has_value());
}

TEST(ChainstateCommitBatchInterval, NonIntervalHeightSkipsCheckpointOnly) {
    ChainstateCommitBatch ccb(/*tip_height=*/501, /*utreexo_active=*/true,
                              /*utreexo_stateless=*/false,
                              /*shielded_active=*/false,
                              /*utreexo_checkpoint_interval=*/500);
    StageBaseline(ccb);

    // Marker and delta sidecar are still required at a non-interval height.
    auto missing = ccb.AllRequiredStaged();
    ASSERT_TRUE(missing.has_value());
    EXPECT_EQ(*missing, "utreexo_forest_tip_marker");

    ccb.MarkUtreexoForestTipMarkerStaged();
    missing = ccb.AllRequiredStaged();
    ASSERT_TRUE(missing.has_value());
    EXPECT_EQ(*missing, "utreexo_delta_sidecar");

    // With marker + sidecar staged and NO checkpoint, height 501 (% 500 != 0)
    // must be commit-ready.
    ccb.MarkUtreexoDeltaStaged();
    EXPECT_FALSE(ccb.AllRequiredStaged().has_value());
}

TEST(ChainstateCommitBatchInterval, IntervalHeightStillRequiresCheckpoint) {
    ChainstateCommitBatch ccb(/*tip_height=*/1000, /*utreexo_active=*/true,
                              /*utreexo_stateless=*/false,
                              /*shielded_active=*/false,
                              /*utreexo_checkpoint_interval=*/500);
    StageBaseline(ccb);
    ccb.MarkUtreexoForestTipMarkerStaged();
    ccb.MarkUtreexoDeltaStaged();

    const auto missing = ccb.AllRequiredStaged();
    ASSERT_TRUE(missing.has_value());
    EXPECT_EQ(*missing, "utreexo_checkpoint");

    ccb.MarkUtreexoCheckpointStaged();
    EXPECT_FALSE(ccb.AllRequiredStaged().has_value());
}

TEST(ChainstateCommitBatchInterval, ZeroIntervalBehavesLikeEveryBlock) {
    ChainstateCommitBatch ccb(/*tip_height=*/7, /*utreexo_active=*/true,
                              /*utreexo_stateless=*/false,
                              /*shielded_active=*/false,
                              /*utreexo_checkpoint_interval=*/0);
    StageBaseline(ccb);
    ccb.MarkUtreexoForestTipMarkerStaged();
    ccb.MarkUtreexoDeltaStaged();

    const auto missing = ccb.AllRequiredStaged();
    ASSERT_TRUE(missing.has_value());
    EXPECT_EQ(*missing, "utreexo_checkpoint");
}

TEST(ChainstateCommitBatchInterval, StatelessModeUnaffectedByInterval) {
    ChainstateCommitBatch ccb(/*tip_height=*/500, /*utreexo_active=*/true,
                              /*utreexo_stateless=*/true,
                              /*shielded_active=*/false,
                              /*utreexo_checkpoint_interval=*/500);
    StageBaseline(ccb);
    // Stateless mode never required the utreexo trio in the unified batch.
    EXPECT_FALSE(ccb.AllRequiredStaged().has_value());
}

}  // namespace
