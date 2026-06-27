// src/test/shielded_consistency_tests.cpp
#include <algorithm>
#include <gtest/gtest.h>
#include "consensus/shielded/shielded_consistency.h"
using namespace dinero::consensus::shielded;

static dinero::uint256 R(uint8_t b){ dinero::uint256 r; std::fill(r.data, r.data+32, b); return r; }

static ShieldedConsistencyInputs base() {
    ShieldedConsistencyInputs in;
    in.observed = {R(1), 10, 4};
    in.marker_present = true;
    in.marker = {R(1), 10, 4};
    in.marker_height = 52066; in.marker_hash = R(9);
    in.active_height = 52066; in.active_hash = R(9);
    in.activity_below_tip = true;
    return in;
}

TEST(ShieldedConsistency, AlignedWhenAllMatch) {
    EXPECT_EQ(ClassifyShieldedConsistency(base()).status, ShieldedConsistency::Aligned);
}
TEST(ShieldedConsistency, EmptyTreeUnderPopulatedMarkerIsSizeMismatch) {
    auto in = base(); in.observed = {R(0), 0, 0};
    auto rep = ClassifyShieldedConsistency(in);
    EXPECT_EQ(rep.status, ShieldedConsistency::SizeMismatch);
    EXPECT_NE(rep.detail.find("tree_size"), std::string::npos);
    EXPECT_NE(rep.detail.find("52066"), std::string::npos);   // names height
}
TEST(ShieldedConsistency, RootMismatchDetected) {
    auto in = base(); in.observed.root = R(2);
    EXPECT_EQ(ClassifyShieldedConsistency(in).status, ShieldedConsistency::RootMismatch);
}
TEST(ShieldedConsistency, NullifierCountMismatchDetected) {
    auto in = base(); in.observed.nullifier_count = 3;
    EXPECT_EQ(ClassifyShieldedConsistency(in).status, ShieldedConsistency::NullifierCountMismatch);
}
TEST(ShieldedConsistency, MarkerMissingWithActivityIsDesync) {
    auto in = base(); in.marker_present = false;
    EXPECT_EQ(ClassifyShieldedConsistency(in).status, ShieldedConsistency::MarkerMissingButActivityExists);
}
TEST(ShieldedConsistency, MarkerMissingNoActivityIsBenign) {
    auto in = base(); in.marker_present = false; in.activity_below_tip = false; in.observed = {R(0),0,0};
    EXPECT_EQ(ClassifyShieldedConsistency(in).status, ShieldedConsistency::MarkerMissingNoActivity);
}
TEST(ShieldedConsistency, LaggingMarkerIsTipHeightMismatch) {
    auto in = base(); in.marker_height = 52000;   // marker behind active tip
    EXPECT_EQ(ClassifyShieldedConsistency(in).status, ShieldedConsistency::TipHeightMismatch);
}
