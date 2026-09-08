// Copyright (c) 2026 Dinero Labs.
//
// Block-acceptance classification must key on the ACTIVE CONSENSUS TIP, not
// ChainDB's durable tip.
//
// The defect this locks out: during AssumeUTXO background replay, active_tip_
// is deliberately restored to the snapshot base while the DURABLE ChainDB tip
// still trails behind it. Comparing against the database tip therefore
// misclassified the base's own child as a side chain -- and that
// misclassification is what forced the deferred base-child precheck exemption,
// which in turn DISABLED accept-time INVALID_UTREEXO_ROOT rejection for exactly
// that block. A PoW-valid base+1 carrying a forged header root was accepted,
// stored, indexed and announced, with the invalidity surfacing only at
// promotion.
//
// The rule is expressed as a pure function so every case below -- including the
// ones that only exist mid-promotion -- is reachable without a daemon.

#include <gtest/gtest.h>

#include <optional>

#include "daemon/active_tip_classification.h"

using dinero::daemon::ClassifyExtension;
using dinero::daemon::ExtensionClass;

namespace {
dinero::uint256 H(uint8_t fill) {
    dinero::uint256 h;
    for (int i = 0; i < 32; ++i) h.data[i] = fill;
    return h;
}
const dinero::uint256 kBase       = H(0xB0);   // snapshot base
const dinero::uint256 kDurableTip = H(0xD0);   // ChainDB tip, TRAILING the base
const dinero::uint256 kSibling    = H(0x51);   // a genuine fork off the base
}  // namespace

// THE case. Deferred replay: active tip is the base, durable tip trails.
// base+1 must be a MAIN-CHAIN EXTENSION.
TEST(ActiveTipClassification, BaseChildExtendsTheActiveTip) {
    EXPECT_EQ(ClassifyExtension(/*active_tip=*/kBase, /*parent=*/kBase),
              ExtensionClass::ExtendsActiveTip)
        << "the base's own child is a genuine extension of the active chain; "
           "classifying it as a side chain is what forced the exemption that "
           "disabled accept-time root rejection";
}

// The discriminating control. Classifying against the DURABLE tip -- which
// trails the base during replay -- gets this wrong, and that wrongness is the
// entire bug.
TEST(ActiveTipClassification, DurableTipWouldMisclassifyTheBaseChild) {
    ASSERT_NE(kDurableTip, kBase) << "precondition: the durable tip trails";
    EXPECT_EQ(ClassifyExtension(/*active_tip=*/kDurableTip, /*parent=*/kBase),
              ExtensionClass::SideChain)
        << "this is what the OLD code computed. If ClassifyExtension is ever "
           "wired back to ChainDB::getTip(), the base child reads as a side "
           "chain again and the root check is skipped again.";
}

// A genuine sibling is still a side chain. The fix must not turn the
// classification into "everything is an extension".
TEST(ActiveTipClassification, GenuineSiblingRemainsSideChain) {
    EXPECT_EQ(ClassifyExtension(/*active_tip=*/kBase, /*parent=*/kSibling),
              ExtensionClass::SideChain)
        << "a block whose parent is not the active tip is a side chain, "
           "deferred mode or not";
}

// No active chain yet: nothing extends it. Must not read as an extension.
TEST(ActiveTipClassification, NoActiveTipExtendsNothing) {
    EXPECT_EQ(ClassifyExtension(/*active_tip=*/std::nullopt, /*parent=*/kBase),
              ExtensionClass::SideChain)
        << "with no active tip there is nothing to extend; defaulting to "
           "'extension' would skip the side-chain precheck on every block";
}

// Promotion moves the active tip. The classification of a given parent must
// follow it exactly -- never a blend of before and after.
TEST(ActiveTipClassification, FollowsTheTipAcrossPromotion) {
    const dinero::uint256 promoted = H(0xC1);   // base+1, now the active tip

    // Before promotion: base+1's parent is the base -> extension.
    EXPECT_EQ(ClassifyExtension(kBase, kBase), ExtensionClass::ExtendsActiveTip);
    // After promotion: a child of base+1 extends; the old base child does not.
    EXPECT_EQ(ClassifyExtension(promoted, promoted), ExtensionClass::ExtendsActiveTip);
    EXPECT_EQ(ClassifyExtension(promoted, kBase), ExtensionClass::SideChain)
        << "once the tip has moved, the previous extension is no longer one; a "
           "torn read that mixed the two would classify both as extensions";
}

// Every observation must come from ONE tip value. This asserts the property
// the activation lock exists to guarantee: classification is a pure function of
// the tip it was given, so two calls with the same tip cannot disagree.
TEST(ActiveTipClassification, IsDeterministicForAGivenTip) {
    for (int i = 0; i < 64; ++i) {
        EXPECT_EQ(ClassifyExtension(kBase, kBase), ExtensionClass::ExtendsActiveTip);
        EXPECT_EQ(ClassifyExtension(kBase, kSibling), ExtensionClass::SideChain);
    }
}
