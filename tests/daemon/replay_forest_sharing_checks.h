#pragma once
#include <functional>
#include <type_traits>
#include "consensus/utreexo_canonical_roots_activation.h"
namespace dinero {
namespace {
consensus::UtreexoHash SharingLeaf(uint32_t n) {
    consensus::UtreexoHash h(32,0x59);
    for(unsigned i=0;i<4;++i) h[i]=uint8_t(n>>(8*i));
    return h;
}
consensus::UtreexoForest SharingForest() {
    consensus::UtreexoForest f;f.setCanonicalEmptyRoots(true);
    for(uint32_t i=0;i<64;++i)
        if(f.add(SharingLeaf(i))!=i)throw std::runtime_error("sharing fixture add");
    if(!f.removeAtKnownPosition(0,SharingLeaf(0)))throw std::runtime_error("sharing fixture delete");
    return f;
}
}
TEST(ReplayForestSharing, CopyCloneAndEveryPublicMutationPreserveCapturedState) {
    using F=consensus::UtreexoForest;
    static_assert(std::is_nothrow_move_constructible_v<F> && std::is_nothrow_move_assignable_v<F>);
    const F original=SharingForest();const auto exact=original.dumpInternalState();
    const auto bytes=original.serialize();
    const std::vector<std::function<void(F&)>> mutations{
        [](F& f){EXPECT_NE(f.add(SharingLeaf(100)),UINT64_MAX);},
        [](F& f){auto p=f.prove(3);ASSERT_TRUE(p);EXPECT_TRUE(f.remove(SharingLeaf(3),*p));},
        [](F& f){EXPECT_TRUE(f.removeAtKnownPosition(3,SharingLeaf(3)));},
        [](F& f){EXPECT_TRUE(f.removeAtKnownPositions({{3,SharingLeaf(3)},{7,SharingLeaf(7)}}));},
        [](F& f){EXPECT_TRUE(f.restoreDeletedLeaf(0,SharingLeaf(0)));},
        [](F& f){EXPECT_TRUE(f.removeLastNLeaves(1));},
        [](F& f){f.setCanonicalEmptyRoots(false);},
        [](F& f){f.rebuildRoots();}
    };
    for(size_t i=0;i<mutations.size();++i) {
        SCOPED_TRACE(i);F copy=original;F clone=copy.clone();F assigned;assigned=clone;
        EXPECT_EQ(&copy.getIndexedRoots(),&original.getIndexedRoots());
        EXPECT_EQ(&clone.getIndexedRoots(),&original.getIndexedRoots());
        EXPECT_EQ(&assigned.getIndexedRoots(),&original.getIndexedRoots());
        mutations[i](copy);
        EXPECT_NE(&copy.getIndexedRoots(),&original.getIndexedRoots());
        EXPECT_EQ(original.dumpInternalState(),exact);EXPECT_EQ(original.serialize(),bytes);
        EXPECT_EQ(clone.dumpInternalState(),exact);EXPECT_EQ(assigned.dumpInternalState(),exact);
        EXPECT_TRUE(original.isDeleted(0));EXPECT_TRUE(original.findLeafPosition(SharingLeaf(3)));
        EXPECT_TRUE(original.findLeafPosition(SharingLeaf(7)));
        F moved=std::move(copy);copy=F{};EXPECT_EQ(copy.getNumLeaves(),0u);
        F destination;destination=std::move(moved);moved=F{};EXPECT_EQ(moved.getNumLeaves(),0u);
        EXPECT_EQ(original.dumpInternalState(),exact);
    }
}
TEST(ReplayForestSharing, HeightPromotionAndRootRebuildDoNotChangeOldVersion) {
    consensus::UtreexoForest legacy;
    for(uint32_t i=0;i<8;++i)ASSERT_EQ(legacy.add(SharingLeaf(i)),i);
    std::vector<std::pair<uint64_t,consensus::UtreexoHash>> removals;
    for(uint32_t i=0;i<8;++i)removals.emplace_back(i,SharingLeaf(i));
    ASSERT_TRUE(legacy.removeAtKnownPositions(removals));
    const auto exact=legacy.dumpInternalState();const auto bytes=legacy.serialize();
    const uint32_t height=consensus::GetUtreexoCanonicalRootsActivationHeight();
    ASSERT_TRUE(consensus::IsUtreexoCanonicalRootsActive(height));
    auto promoted=legacy.cloneForHeight(height);
    EXPECT_TRUE(promoted.isCanonicalEmptyRoots());EXPECT_FALSE(legacy.isCanonicalEmptyRoots());
    EXPECT_EQ(legacy.dumpInternalState(),exact);EXPECT_EQ(legacy.serialize(),bytes);
    EXPECT_NE(promoted.dumpInternalState(),exact);
    // The public fork API permits the flag change before a separate rebuild.
    // Retain that exact intermediate state; do not normalize old snapshots.
    legacy.setCanonicalEmptyRoots(true);const auto intermediate=legacy.dumpInternalState();
    auto next=legacy.clone();EXPECT_EQ(&next.getIndexedRoots(),&legacy.getIndexedRoots());
    next.rebuildRoots();EXPECT_NE(&next.getIndexedRoots(),&legacy.getIndexedRoots());
    EXPECT_EQ(legacy.dumpInternalState(),intermediate);EXPECT_NE(next.dumpInternalState(),intermediate);
}
TEST(ReplayForestSharing, PreexistingRawReferenceAndGuardedWritesRollbackExactly) {
    auto set=consensus::ConsensusUTXOSet::CreateForReplay();
    ASSERT_TRUE(set->AddCoin(JournalPoint(1),JournalCoin(1)));
    set->ReplaceForestGuarded(SharingForest());set->SetBestBlock(JournalPoint(8).txid.AsUint256(),8);
    auto& prior_reference=set->GetForest();const auto before=set->Snapshot();
    const auto exact=prior_reference.dumpInternalState();
    for(unsigned action=0;action<3;++action) {
        SCOPED_TRACE(action);
        {
            auto rollback=set->BeginBlockConnectRollback();ASSERT_TRUE(rollback);
            ASSERT_TRUE(set->SpendCoin(JournalPoint(1)));
            if(action==0)ASSERT_NE(prior_reference.add(SharingLeaf(100)),UINT64_MAX);
            if(action==1)EXPECT_THROW(set->MutateForestGuarded([](auto& forest) {
                if(!forest.removeAtKnownPosition(3,SharingLeaf(3)))throw std::logic_error("sharing fixture removal");
                forest.setCanonicalEmptyRoots(false);throw std::runtime_error("after real mutation");
            }),std::runtime_error);
            if(action==2) {
                ASSERT_TRUE(set->RemoveLastNLeavesGuarded(1));
                auto replacement=SharingForest();ASSERT_NE(replacement.add(SharingLeaf(200)),UINT64_MAX);
                set->ReplaceForestGuarded(std::move(replacement));
            }
            set->SetBestBlock(JournalPoint(9).txid.AsUint256(),9);
        }
        ExpectJournalState(*set,before);EXPECT_EQ(prior_reference.dumpInternalState(),exact);
    }
}
TEST(ReplayForestSharing, CommittedReplayAndLaterMutationsKeepIndependentCaptures) {
    auto set=consensus::ConsensusUTXOSet::CreateForReplay();set->ReplaceForestGuarded(SharingForest());
    ASSERT_TRUE(set->AddCoin(JournalPoint(1),JournalCoin(1)));
    auto& raw=set->GetForest();const auto retained=raw.clone();const auto old=retained.dumpInternalState();
    {
        auto rollback=set->BeginBlockConnectRollback();ASSERT_TRUE(rollback);
        ASSERT_NE(raw.add(SharingLeaf(100)),UINT64_MAX);rollback->Commit();
    }
    const auto committed=raw.clone();const auto exact=committed.dumpInternalState();
    EXPECT_NE(exact,old);EXPECT_EQ(retained.dumpInternalState(),old);
    {
        auto rollback=set->BeginBlockConnectRollback();ASSERT_TRUE(rollback);
        ASSERT_TRUE(raw.removeAtKnownPosition(3,SharingLeaf(3)));
        ASSERT_TRUE(set->SpendCoin(JournalPoint(1)));
    }
    EXPECT_EQ(raw.dumpInternalState(),exact);EXPECT_EQ(committed.dumpInternalState(),exact);
    EXPECT_EQ(retained.dumpInternalState(),old);EXPECT_TRUE(set->HaveCoin(JournalPoint(1)));
}
} // namespace dinero
