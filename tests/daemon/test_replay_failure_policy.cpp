/**
 * Regression tests for the AssumeUTXO background-replay hardening
 * (2026-07-14 field incident: a transient bad body read failed height 143
 * with bad-utreexo-root and was escalated straight to a persisted
 * fatal_mismatch + safe mode, even though the stored body was byte-identical
 * to the canonical block and an earlier pass had validated the same height).
 *
 * Two guards were added:
 *  1. ReplayFailurePolicy — a validation failure must reproduce on a fresh
 *     pass before the worker declares the spec fatal (#298 fatal semantics
 *     preserved for CONFIRMED mismatches).
 *  2. A merkle-root check on every replay body read — the block hash covers
 *     only the header, so a torn tx section otherwise walks into ConnectBlock
 *     (which never re-checks merkle) and manifests as a false consensus
 *     failure. This file locks the detection property the worker relies on.
 */

#include <gtest/gtest.h>

#include "consensus/chainparams.h"
#include "consensus/merkle_root.h"
#include "daemon/services/replay_failure_policy.h"
#include "primitives/block.h"
#include "primitives/transaction.h"

using dinero::assumeutxo::ReplayFailurePolicy;
using Action = dinero::assumeutxo::ReplayFailurePolicy::Action;

// ─────────────────────────────────────────────────────────────────────────
// ReplayFailurePolicy
// ─────────────────────────────────────────────────────────────────────────

TEST(ReplayFailurePolicy, FirstFailureRetries)
{
    ReplayFailurePolicy policy;
    EXPECT_EQ(policy.OnValidationFailure(143), Action::kRetryPass);
    EXPECT_EQ(policy.TotalRetries(), 1u);
}

TEST(ReplayFailurePolicy, SameHeightFailingTwiceIsConfirmedFatal)
{
    ReplayFailurePolicy policy;
    EXPECT_EQ(policy.OnValidationFailure(143), Action::kRetryPass);
    // Fresh pass re-read the body and the SAME height failed again:
    // stable => genuine mismatch => spec fatal.
    EXPECT_EQ(policy.OnValidationFailure(143), Action::kConfirmFatal);
}

TEST(ReplayFailurePolicy, SuccessClearsTheFailureRecord)
{
    ReplayFailurePolicy policy;
    EXPECT_EQ(policy.OnValidationFailure(143), Action::kRetryPass);
    // The retry pass validated the height: the failure was transient.
    policy.OnValidationSuccess(143);
    // A much later, unrelated transient at the same height must get its own
    // confirmation round, not an instant fatal.
    EXPECT_EQ(policy.OnValidationFailure(143), Action::kRetryPass);
}

TEST(ReplayFailurePolicy, DistinctHeightsEachGetTheirOwnConfirmation)
{
    ReplayFailurePolicy policy;
    EXPECT_EQ(policy.OnValidationFailure(143), Action::kRetryPass);
    EXPECT_EQ(policy.OnValidationFailure(9000), Action::kRetryPass);
    EXPECT_EQ(policy.OnValidationFailure(143), Action::kConfirmFatal);
}

TEST(ReplayFailurePolicy, GlobalRetryCapForcesFatal)
{
    // Failures hopping across heights forever must still converge.
    ReplayFailurePolicy policy(/*confirmations_required=*/2,
                               /*max_total_retries=*/3);
    EXPECT_EQ(policy.OnValidationFailure(1), Action::kRetryPass);
    EXPECT_EQ(policy.OnValidationFailure(2), Action::kRetryPass);
    EXPECT_EQ(policy.OnValidationFailure(3), Action::kRetryPass);
    EXPECT_EQ(policy.TotalRetries(), 3u);
    EXPECT_EQ(policy.OnValidationFailure(4), Action::kConfirmFatal);
}

// ─────────────────────────────────────────────────────────────────────────
// Torn-body detection contract (merkle guard on replay reads)
// ─────────────────────────────────────────────────────────────────────────

class ReplayBodyIntegrityTest : public ::testing::Test {
protected:
    static void SetUpTestSuite() { dinero::SelectParams(dinero::Chain::REGTEST); }

    static dinero::Transaction MakeTx(uint8_t seed) {
        dinero::Transaction tx;
        tx.vin.resize(1);
        tx.vin[0].prevout.txid = dinero::TxId();
        tx.vin[0].prevout.vout = 0xFFFFFFFF;
        tx.vout.resize(1);
        tx.vout[0].value = dinero::AmountUna::Una(1000 + seed);
        tx.vout[0].scriptPubKey = {0x51, seed};  // distinct per seed
        return tx;
    }
};

TEST_F(ReplayBodyIntegrityTest, IntactBodyMatchesHeaderMerkleRoot)
{
    dinero::Block block;
    block.vtx.push_back(MakeTx(1));
    block.vtx.push_back(MakeTx(2));
    block.header.merkle_root = dinero::consensus::ComputeMerkleRoot(block.vtx);

    EXPECT_EQ(dinero::consensus::ComputeMerkleRoot(block.vtx),
              block.header.merkle_root);
}

TEST_F(ReplayBodyIntegrityTest, TornTxSectionIsDetectedByMerkleGuard)
{
    dinero::Block block;
    block.vtx.push_back(MakeTx(1));
    block.vtx.push_back(MakeTx(2));
    block.header.merkle_root = dinero::consensus::ComputeMerkleRoot(block.vtx);

    // Simulate a torn read: header intact, tx section corrupted. The block
    // hash (header-only) still matches, so ONLY the merkle guard catches it.
    block.vtx[1].vout[0].value = dinero::AmountUna::Una(999999);

    EXPECT_NE(dinero::consensus::ComputeMerkleRoot(block.vtx),
              block.header.merkle_root);
}

TEST_F(ReplayBodyIntegrityTest, DroppedTrailingTxIsDetectedByMerkleGuard)
{
    dinero::Block block;
    block.vtx.push_back(MakeTx(1));
    block.vtx.push_back(MakeTx(2));
    block.header.merkle_root = dinero::consensus::ComputeMerkleRoot(block.vtx);

    // Simulate a partially-flushed body: trailing tx missing entirely.
    block.vtx.pop_back();

    EXPECT_NE(dinero::consensus::ComputeMerkleRoot(block.vtx),
              block.header.merkle_root);
}

int main(int argc, char** argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
