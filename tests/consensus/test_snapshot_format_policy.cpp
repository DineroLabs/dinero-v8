// Fail-closed AssumeUTXO container-format policy.
//
// V2 is DEPRECATED AND REJECTED UNCONDITIONALLY -- a deprecation, not a
// validation gate. No V2 file can pass, by design: a V2 container carries no
// utreexo section, so its forest would be rebuilt by sorting UTXOs by OutPoint
// rather than in chronological insertion order, and nothing downstream can
// repair a wrong leaf ORDER. Testing it as though a "good" V2 might exist would
// misrepresent the policy.
//
// V3 carries a utreexo section but no shielded section: harmless below shielded
// activation, fatal at or after it (empty shielded commitment tree -> wedge on
// the first post-snapshot shielded spend).
//
// V4 is the supported production format.
//
// These tests cover the PREDICATE exhaustively -- boundaries, dormant
// activation, unknown versions -- without standing up a chainstate. The
// zero-state-mutation property and a genuine V4 import are proven separately
// against the real LoadSnapshot() path.

#include "consensus/chainparams.h"
#include "consensus/utxo_snapshot.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>

namespace {

using dinero::consensus::EvaluateSnapshotFormat;
using dinero::consensus::SnapshotFormatVerdict;
using dinero::consensus::SNAPSHOT_VERSION_V2;
using dinero::consensus::SNAPSHOT_VERSION_V3;
using dinero::consensus::SNAPSHOT_VERSION_V4;

constexpr uint32_t kU32Max = std::numeric_limits<uint32_t>::max();

// ---------------------------------------------------------------------------
// V2: unconditional
// ---------------------------------------------------------------------------

TEST(SnapshotFormatPolicy, V2IsRejectedAtEveryHeightAndEveryActivation) {
    // Swept deliberately rather than spot-checked: the claim is that NO
    // combination accepts V2. A single sampled height would not establish that.
    for (const uint32_t height : {0U, 1U, 8649U, 8650U, 8651U, 73035U, kU32Max}) {
        for (const uint32_t activation : {0U, 1U, 8650U, 73035U, kU32Max}) {
            EXPECT_EQ(EvaluateSnapshotFormat(SNAPSHOT_VERSION_V2, height, activation),
                      SnapshotFormatVerdict::RejectV2Deprecated)
                << "V2 accepted at height " << height
                << " with activation " << activation
                << " -- V2 is deprecated, not conditionally valid";
        }
    }
}

// ---------------------------------------------------------------------------
// V3: exact boundary
// ---------------------------------------------------------------------------

TEST(SnapshotFormatPolicy, V3BoundaryIsExactAtShieldedActivation) {
    constexpr uint32_t kActivation = 8650;

    // Strictly below activation: allowed.
    EXPECT_EQ(EvaluateSnapshotFormat(SNAPSHOT_VERSION_V3, 0, kActivation),
              SnapshotFormatVerdict::Accept);
    EXPECT_EQ(EvaluateSnapshotFormat(SNAPSHOT_VERSION_V3, kActivation - 2, kActivation),
              SnapshotFormatVerdict::Accept);
    EXPECT_EQ(EvaluateSnapshotFormat(SNAPSHOT_VERSION_V3, kActivation - 1, kActivation),
              SnapshotFormatVerdict::Accept)
        << "the last height below activation must still be allowed";

    // At activation and above: rejected. `>=`, not `>`.
    EXPECT_EQ(EvaluateSnapshotFormat(SNAPSHOT_VERSION_V3, kActivation, kActivation),
              SnapshotFormatVerdict::RejectV3PostShieldedActivation)
        << "the activation height ITSELF must be rejected";
    EXPECT_EQ(EvaluateSnapshotFormat(SNAPSHOT_VERSION_V3, kActivation + 1, kActivation),
              SnapshotFormatVerdict::RejectV3PostShieldedActivation);
    EXPECT_EQ(EvaluateSnapshotFormat(SNAPSHOT_VERSION_V3, kU32Max, kActivation),
              SnapshotFormatVerdict::RejectV3PostShieldedActivation);
}

// Dormant activation must fall out of the plain comparison, with no special
// case. If someone later "fixes" UINT32_MAX by hand, this pins the behaviour.
TEST(SnapshotFormatPolicy, V3IsAllowedWhenShieldedActivationIsDormant) {
    for (const uint32_t height : {0U, 1U, 8650U, 73035U, kU32Max - 1}) {
        EXPECT_EQ(EvaluateSnapshotFormat(SNAPSHOT_VERSION_V3, height, kU32Max),
                  SnapshotFormatVerdict::Accept)
            << "V3 rejected at height " << height
            << " while shielded activation is dormant (UINT32_MAX)";
    }
    // The single degenerate point: height == UINT32_MAX == activation. `>=`
    // holds, so it rejects. Recorded rather than special-cased -- an
    // unreachable height in practice.
    EXPECT_EQ(EvaluateSnapshotFormat(SNAPSHOT_VERSION_V3, kU32Max, kU32Max),
              SnapshotFormatVerdict::RejectV3PostShieldedActivation);
}

// Regtest activates shielded at 0, so EVERY V3 snapshot is rejected there.
// Called out because it is surprising and could otherwise look like a bug.
TEST(SnapshotFormatPolicy, V3IsAlwaysRejectedWhenActivationIsZero) {
    for (const uint32_t height : {0U, 1U, 1000U, kU32Max}) {
        EXPECT_EQ(EvaluateSnapshotFormat(SNAPSHOT_VERSION_V3, height, 0),
                  SnapshotFormatVerdict::RejectV3PostShieldedActivation);
    }
}

// ---------------------------------------------------------------------------
// V4 and unknown versions
// ---------------------------------------------------------------------------

TEST(SnapshotFormatPolicy, V4IsAcceptedEverywhere) {
    for (const uint32_t height : {0U, 8649U, 8650U, 73035U, kU32Max}) {
        for (const uint32_t activation : {0U, 8650U, kU32Max}) {
            EXPECT_EQ(EvaluateSnapshotFormat(SNAPSHOT_VERSION_V4, height, activation),
                      SnapshotFormatVerdict::Accept)
                << "V4 rejected at height " << height
                << " with activation " << activation;
        }
    }
}

TEST(SnapshotFormatPolicy, UnknownVersionsAreRejected) {
    for (const uint32_t version : {0U, 1U, 5U, 6U, 99U, kU32Max}) {
        EXPECT_EQ(EvaluateSnapshotFormat(version, 1000, 8650),
                  SnapshotFormatVerdict::RejectUnknownVersion)
            << "version " << version << " was not rejected";
    }
}

// ---------------------------------------------------------------------------
// Against the real chain parameters
// ---------------------------------------------------------------------------

TEST(SnapshotFormatPolicy, MainnetRejectsV3AtAndAboveRealActivation) {
    dinero::SelectParams(dinero::Chain::MAINNET);
    const uint32_t activation = dinero::Params().shielded_activation_height;
    ASSERT_EQ(activation, 8650U)
        << "mainnet shielded activation moved; the registry's no-v3-anchor rule "
           "and this policy both reference 8650";

    EXPECT_EQ(EvaluateSnapshotFormat(SNAPSHOT_VERSION_V3, activation - 1, activation),
              SnapshotFormatVerdict::Accept);
    EXPECT_EQ(EvaluateSnapshotFormat(SNAPSHOT_VERSION_V3, activation, activation),
              SnapshotFormatVerdict::RejectV3PostShieldedActivation);

    // The registered anchors and the shipped artifact are all V4 and all far
    // above activation -- they must remain acceptable.
    for (const uint32_t h : {52287U, 65300U, 73035U}) {
        EXPECT_EQ(EvaluateSnapshotFormat(SNAPSHOT_VERSION_V4, h, activation),
                  SnapshotFormatVerdict::Accept)
            << "a registered v4 anchor height was rejected: " << h;
        EXPECT_EQ(EvaluateSnapshotFormat(SNAPSHOT_VERSION_V3, h, activation),
                  SnapshotFormatVerdict::RejectV3PostShieldedActivation)
            << "v3 at a post-activation anchor height must be rejected: " << h;
    }
}

}  // namespace
