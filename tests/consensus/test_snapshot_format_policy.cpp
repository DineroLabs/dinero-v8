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
// V4 carries the shielded section but NO coinbase/merkle-branch binding proof:
// acceptable while the state commitment is dormant, rejected once it is
// enforced at the snapshot base (the shielded section cannot be authenticated
// against the chain at load time without the proof; only v5 carries it).
//
// TWO INDEPENDENT POLICIES, ONE DECISION POINT. The v3 rule keys on
// shielded-TRANSACTION activation; the v4 rule keys on snapshot-TRUST
// activation (state_commitment_activation_height). The sweeps below vary the
// two heights INDEPENDENTLY, including the mixed quadrants -- that is where a
// bug coupling the two activations would hide, because any test moving them
// together cannot see it.
//
// These tests cover the PREDICATE exhaustively -- boundaries, dormant
// activation, unknown versions -- without standing up a chainstate. The
// zero-state-mutation property and a genuine V4 import are proven separately
// against the real LoadSnapshot() path.

#include "consensus/chainparams.h"
#include "consensus/utxo_snapshot.h"
#include "consensus/state_commitment.h"

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

// Shorthand: the dormant sentinel for the state-commitment height, so the
// pre-existing v2/v3 assertions read unchanged in meaning.
constexpr uint32_t kCommitmentDormant = kU32Max;

// ---------------------------------------------------------------------------
// V2: unconditional
// ---------------------------------------------------------------------------

TEST(SnapshotFormatPolicy, V2IsRejectedAtEveryHeightAndEveryActivation) {
    // Swept deliberately rather than spot-checked: the claim is that NO
    // combination accepts V2 -- including every state-commitment quadrant.
    for (const uint32_t height : {0U, 1U, 8649U, 8650U, 8651U, 73035U, kU32Max}) {
        for (const uint32_t activation : {0U, 1U, 8650U, 73035U, kU32Max}) {
            for (const uint32_t commitment : {0U, 1U, 8650U, kU32Max}) {
                EXPECT_EQ(EvaluateSnapshotFormat(SNAPSHOT_VERSION_V2, height,
                                                 activation, commitment),
                          SnapshotFormatVerdict::RejectV2Deprecated)
                    << "V2 accepted at height " << height
                    << " with shielded activation " << activation
                    << " and commitment activation " << commitment
                    << " -- V2 is deprecated, not conditionally valid";
            }
        }
    }
}

// ---------------------------------------------------------------------------
// V3: exact boundary (state-commitment height dormant throughout -- the v3
// rule must not depend on it, which the coupling test below pins)
// ---------------------------------------------------------------------------

TEST(SnapshotFormatPolicy, V3BoundaryIsExactAtShieldedActivation) {
    constexpr uint32_t kActivation = 8650;

    // Strictly below activation: allowed.
    EXPECT_EQ(EvaluateSnapshotFormat(SNAPSHOT_VERSION_V3, 0, kActivation,
                                     kCommitmentDormant),
              SnapshotFormatVerdict::Accept);
    EXPECT_EQ(EvaluateSnapshotFormat(SNAPSHOT_VERSION_V3, kActivation - 2,
                                     kActivation, kCommitmentDormant),
              SnapshotFormatVerdict::Accept);
    EXPECT_EQ(EvaluateSnapshotFormat(SNAPSHOT_VERSION_V3, kActivation - 1,
                                     kActivation, kCommitmentDormant),
              SnapshotFormatVerdict::Accept)
        << "the last height below activation must still be allowed";

    // At activation and above: rejected. `>=`, not `>`.
    EXPECT_EQ(EvaluateSnapshotFormat(SNAPSHOT_VERSION_V3, kActivation,
                                     kActivation, kCommitmentDormant),
              SnapshotFormatVerdict::RejectV3PostShieldedActivation)
        << "the activation height ITSELF must be rejected";
    EXPECT_EQ(EvaluateSnapshotFormat(SNAPSHOT_VERSION_V3, kActivation + 1,
                                     kActivation, kCommitmentDormant),
              SnapshotFormatVerdict::RejectV3PostShieldedActivation);
    EXPECT_EQ(EvaluateSnapshotFormat(SNAPSHOT_VERSION_V3, kU32Max, kActivation,
                                     kCommitmentDormant),
              SnapshotFormatVerdict::RejectV3PostShieldedActivation);
}

// Dormant activation must fall out of the plain comparison, with no special
// case. If someone later "fixes" UINT32_MAX by hand, this pins the behaviour.
TEST(SnapshotFormatPolicy, V3IsAllowedWhenShieldedActivationIsDormant) {
    for (const uint32_t height : {0U, 1U, 8650U, 73035U, kU32Max - 1}) {
        EXPECT_EQ(EvaluateSnapshotFormat(SNAPSHOT_VERSION_V3, height, kU32Max,
                                         kCommitmentDormant),
                  SnapshotFormatVerdict::Accept)
            << "V3 rejected at height " << height
            << " while shielded activation is dormant (UINT32_MAX)";
    }
    // The single degenerate point: height == UINT32_MAX == activation. `>=`
    // holds, so it rejects. Recorded rather than special-cased. The height is
    // ATTACKER-CONTROLLED (a snapshot file claims its own base height), so the
    // safety property is the failure DIRECTION -- a spurious `>=` rejects,
    // i.e. fails closed -- not the height's practical unreachability.
    EXPECT_EQ(EvaluateSnapshotFormat(SNAPSHOT_VERSION_V3, kU32Max, kU32Max,
                                     kCommitmentDormant),
              SnapshotFormatVerdict::RejectV3PostShieldedActivation);
}

// Regtest activates shielded at 0, so EVERY V3 snapshot is rejected there.
// Called out because it is surprising and could otherwise look like a bug.
TEST(SnapshotFormatPolicy, V3IsAlwaysRejectedWhenActivationIsZero) {
    for (const uint32_t height : {0U, 1U, 1000U, kU32Max}) {
        EXPECT_EQ(EvaluateSnapshotFormat(SNAPSHOT_VERSION_V3, height, 0,
                                         kCommitmentDormant),
                  SnapshotFormatVerdict::RejectV3PostShieldedActivation);
    }
}

// ---------------------------------------------------------------------------
// V4: exact boundary at the STATE-COMMITMENT activation
// ---------------------------------------------------------------------------

TEST(SnapshotFormatPolicy, V4IsAcceptedEverywhereWhileCommitmentIsDormant) {
    // The pre-gate behaviour, preserved verbatim under the dormant sentinel:
    // shielded activation may take any value (mixed quadrant: shielded active,
    // commitment dormant) and v4 stays acceptable at every claimed height --
    // including the degenerate UINT32_MAX one, which the sentinel guard in
    // IsStateCommitmentActive closes without special-casing.
    for (const uint32_t height : {0U, 8649U, 8650U, 73035U, kU32Max}) {
        for (const uint32_t shielded : {0U, 8650U, kU32Max}) {
            EXPECT_EQ(EvaluateSnapshotFormat(SNAPSHOT_VERSION_V4, height,
                                             shielded, kCommitmentDormant),
                      SnapshotFormatVerdict::Accept)
                << "V4 rejected at height " << height
                << " with shielded activation " << shielded
                << " while the state commitment is dormant";
        }
    }
}

TEST(SnapshotFormatPolicy, V4BoundaryIsExactAtStateCommitmentActivation) {
    constexpr uint32_t kCommitment = 100000;

    // Below: acceptable (no proof needed where nothing is enforced).
    EXPECT_EQ(EvaluateSnapshotFormat(SNAPSHOT_VERSION_V4, 0, kU32Max, kCommitment),
              SnapshotFormatVerdict::Accept);
    EXPECT_EQ(EvaluateSnapshotFormat(SNAPSHOT_VERSION_V4, kCommitment - 1,
                                     kU32Max, kCommitment),
              SnapshotFormatVerdict::Accept)
        << "the last height below commitment activation must still be allowed";

    // At and above: rejected -- a v4 base inside enforcement cannot be
    // authenticated at load (no coinbase/merkle-branch proof).
    EXPECT_EQ(EvaluateSnapshotFormat(SNAPSHOT_VERSION_V4, kCommitment,
                                     kU32Max, kCommitment),
              SnapshotFormatVerdict::RejectV4PostStateCommitmentActivation)
        << "the activation height ITSELF must be rejected";
    EXPECT_EQ(EvaluateSnapshotFormat(SNAPSHOT_VERSION_V4, kCommitment + 1,
                                     kU32Max, kCommitment),
              SnapshotFormatVerdict::RejectV4PostStateCommitmentActivation);
    // Attacker-claimed degenerate height with a real activation: `>=` holds,
    // rejection -- fail-closed by direction.
    EXPECT_EQ(EvaluateSnapshotFormat(SNAPSHOT_VERSION_V4, kU32Max,
                                     kU32Max, kCommitment),
              SnapshotFormatVerdict::RejectV4PostStateCommitmentActivation);
}

TEST(SnapshotFormatPolicy, V4RuleAtRegtestActivationHeightOne) {
    // Regtest wires state_commitment_activation_height = 1 (NOT 0: the genesis
    // coinbase carries no commitment). A v4 snapshot based at genesis stays
    // loadable; based at 1 or later it needs v5.
    EXPECT_EQ(EvaluateSnapshotFormat(SNAPSHOT_VERSION_V4, 0, 0, 1),
              SnapshotFormatVerdict::Accept);
    EXPECT_EQ(EvaluateSnapshotFormat(SNAPSHOT_VERSION_V4, 1, 0, 1),
              SnapshotFormatVerdict::RejectV4PostStateCommitmentActivation);
}

// THE COUPLING PIN. Snapshot-TRUST activation and shielded-TRANSACTION
// activation are separate policies; wiring either rule to the other's height
// would compile and pass any test that moves the heights together. These two
// mixed quadrants are the only place that bug is visible.
TEST(SnapshotFormatPolicy, PoliciesAreNotCoupled) {
    // Quadrant 1: shielded ACTIVE, commitment DORMANT. The v4 rule must not
    // borrow the shielded height: v4 above shielded activation stays accepted.
    EXPECT_EQ(EvaluateSnapshotFormat(SNAPSHOT_VERSION_V4, 8650, 8650,
                                     kCommitmentDormant),
              SnapshotFormatVerdict::Accept)
        << "v4 was rejected using the SHIELDED height -- the two activation "
           "policies have been coupled";

    // Quadrant 2: shielded DORMANT, commitment ACTIVE. The v3 rule must not
    // borrow the commitment height: v3 above commitment activation stays
    // accepted (v3's own gate is shielded activation, which is dormant here),
    // while v4 at the same coordinates is rejected by ITS OWN rule.
    EXPECT_EQ(EvaluateSnapshotFormat(SNAPSHOT_VERSION_V3, 5000, kU32Max, 100U),
              SnapshotFormatVerdict::Accept)
        << "v3 was rejected using the COMMITMENT height -- the two activation "
           "policies have been coupled";
    EXPECT_EQ(EvaluateSnapshotFormat(SNAPSHOT_VERSION_V4, 5000, kU32Max, 100U),
              SnapshotFormatVerdict::RejectV4PostStateCommitmentActivation);
}

TEST(SnapshotFormatPolicy, V5IsAcceptedInBothDormantAndEnforcedRegimes) {
    // v5 carries the binding proof, so the FORMAT is acceptable everywhere —
    // below activation the proof is opportunistic, at/above it the proof's own
    // verification chain (snapshot_binding.h) decides, not the format policy.
    using dinero::consensus::SNAPSHOT_VERSION_V5;
    for (const uint32_t height : {0U, 1U, 100000U, kU32Max}) {
        for (const uint32_t commitment : {0U, 1U, 100000U, kU32Max}) {
            EXPECT_EQ(EvaluateSnapshotFormat(SNAPSHOT_VERSION_V5, height,
                                             kU32Max, commitment),
                      SnapshotFormatVerdict::Accept)
                << "V5 rejected at height " << height
                << " with commitment activation " << commitment;
        }
    }
}

TEST(SnapshotFormatPolicy, UnknownVersionsAreRejected) {
    for (const uint32_t version : {0U, 1U, 6U, 7U, 99U, kU32Max}) {
        EXPECT_EQ(EvaluateSnapshotFormat(version, 1000, 8650, kCommitmentDormant),
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
    const uint32_t commitment = dinero::Params().state_commitment_activation_height;
    ASSERT_EQ(commitment, 111000U);
    EXPECT_FALSE(dinero::consensus::IsStateCommitmentActive(110999, commitment));
    EXPECT_TRUE(dinero::consensus::IsStateCommitmentActive(111000, commitment));
    EXPECT_TRUE(dinero::consensus::IsStateCommitmentActive(111001, commitment));
    using dinero::consensus::SNAPSHOT_VERSION_V5;
    EXPECT_EQ(EvaluateSnapshotFormat(SNAPSHOT_VERSION_V4, 110999, activation, commitment),
              SnapshotFormatVerdict::Accept);
    EXPECT_EQ(EvaluateSnapshotFormat(SNAPSHOT_VERSION_V4, 111000, activation, commitment),
              SnapshotFormatVerdict::RejectV4PostStateCommitmentActivation);
    EXPECT_EQ(EvaluateSnapshotFormat(SNAPSHOT_VERSION_V4, 111001, activation, commitment),
              SnapshotFormatVerdict::RejectV4PostStateCommitmentActivation);
    EXPECT_EQ(EvaluateSnapshotFormat(SNAPSHOT_VERSION_V5, 111000, activation, commitment),
              SnapshotFormatVerdict::Accept);

    EXPECT_EQ(EvaluateSnapshotFormat(SNAPSHOT_VERSION_V3, activation - 1,
                                     activation, commitment),
              SnapshotFormatVerdict::Accept);
    EXPECT_EQ(EvaluateSnapshotFormat(SNAPSHOT_VERSION_V3, activation,
                                     activation, commitment),
              SnapshotFormatVerdict::RejectV3PostShieldedActivation);

    // The registered anchors and the shipped artifact are all V4 and all far
    // above shielded activation but below DNRS activation: retain compatibility.
    for (const uint32_t h : {52287U, 65300U, 73035U}) {
        EXPECT_EQ(EvaluateSnapshotFormat(SNAPSHOT_VERSION_V4, h, activation,
                                         commitment),
                  SnapshotFormatVerdict::Accept)
            << "a registered v4 anchor height was rejected: " << h;
        EXPECT_EQ(EvaluateSnapshotFormat(SNAPSHOT_VERSION_V3, h, activation,
                                         commitment),
                  SnapshotFormatVerdict::RejectV3PostShieldedActivation)
            << "v3 at a post-activation anchor height must be rejected: " << h;
    }
}

}  // namespace
