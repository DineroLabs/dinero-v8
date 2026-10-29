#include "wallet/runtime_replay_membership.h"
#include <gtest/gtest.h>
#include <set>
#include <vector>

namespace {
using Membership=dinero::wallet::detail::RuntimeReplayMembership;
using Key=Membership::Key;
Key SingleBit(unsigned bit) {
    Key key{};key[bit/8]=uint8_t(1u<<(7-bit%8));return key;
}
Key RandomKey(uint64_t& state) {
    Key key{};
    for(auto& byte:key){state^=state<<13;state^=state>>7;state^=state<<17;byte=uint8_t(state);}
    return key;
}
TEST(RuntimeReplayMembership,EmptyDuplicateAndIndependentForkLifetimes) {
    const Key zero{};const auto left=SingleBit(0),right=SingleBit(255);
    Membership base;EXPECT_FALSE(base.Contains(zero));
    auto common=base.With(zero);auto first=common.With(left);auto sibling=common.With(right);
    EXPECT_FALSE(base.Contains(zero));EXPECT_TRUE(common.Contains(zero));
    EXPECT_FALSE(common.Contains(left));EXPECT_FALSE(common.Contains(right));
    common=Membership{};base=Membership{};
    EXPECT_TRUE(first.Contains(zero));EXPECT_TRUE(first.Contains(left));EXPECT_FALSE(first.Contains(right));
    EXPECT_TRUE(sibling.Contains(zero));EXPECT_TRUE(sibling.Contains(right));EXPECT_FALSE(sibling.Contains(left));
    auto duplicate=first.With(left);first=Membership{};
    EXPECT_TRUE(duplicate.Contains(zero));EXPECT_TRUE(duplicate.Contains(left));EXPECT_FALSE(duplicate.Contains(right));
}
TEST(RuntimeReplayMembership,EverySplitPositionAndReverseInsertion) {
    const Key zero{};
    for(bool reverse:{false,true}) {
        Membership value=Membership{}.With(zero);
        std::vector<Membership> snapshots{value};std::set<Key> expected{zero};
        for(unsigned step=0;step<256;++step) {
            const auto key=SingleBit(reverse?255-step:step);value=value.With(key);expected.insert(key);snapshots.push_back(value);
            for(unsigned bit=0;bit<256;++bit)EXPECT_EQ(value.Contains(SingleBit(bit)),expected.contains(SingleBit(bit)));
        }
        for(unsigned step=0;step<257;++step) {
            EXPECT_TRUE(snapshots[step].Contains(zero));
            for(unsigned bit=0;bit<256;++bit)EXPECT_EQ(snapshots[step].Contains(SingleBit(reverse?255-bit:bit)),bit<step);
        }
    }
}
TEST(RuntimeReplayMembership,DeterministicReferenceSetsAcrossRetainedBranches) {
    uint64_t random=0x57a83d9e2461bc05ULL;std::vector<Key> keys;
    for(unsigned i=0;i<1024;++i)keys.push_back(RandomKey(random));
    std::vector<Membership> branches(8);std::vector<std::set<Key>> expected(8);
    Membership common;for(unsigned i=0;i<128;++i)common=common.With(keys[i]);
    for(unsigned branch=0;branch<8;++branch) {
        branches[branch]=common;expected[branch].insert(keys.begin(),keys.begin()+128);
        for(unsigned i=128;i<1024;++i)if(i%8==branch) {
            branches[branch]=branches[branch].With(keys[i]);expected[branch].insert(keys[i]);
        }
    }
    common=Membership{};
    for(unsigned branch=0;branch<8;++branch)for(const auto& key:keys)
        EXPECT_EQ(branches[branch].Contains(key),expected[branch].contains(key));
    for(unsigned i=0;i<128;++i) {
        const auto absent=RandomKey(random);
        for(unsigned branch=0;branch<8;++branch)EXPECT_EQ(branches[branch].Contains(absent),expected[branch].contains(absent));
    }
}
} // namespace
