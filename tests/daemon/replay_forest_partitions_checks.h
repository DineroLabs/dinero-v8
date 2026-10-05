#pragma once
#include <map>
#include <set>
#include <optional>
#include "consensus/utreexo_partitioned_storage.h"

namespace dinero {
namespace {
struct PartitionCopyValue {
    inline static size_t copies=0;
    inline static int remaining=-1;
    uint64_t value=0;
    PartitionCopyValue() noexcept=default;
    explicit PartitionCopyValue(uint64_t n) noexcept:value(n){}
    PartitionCopyValue(const PartitionCopyValue& other):value(other.value) {
        if(remaining==0)throw std::runtime_error("deliberate value copy failure");
        if(remaining>0)--remaining;
        ++copies;
    }
    PartitionCopyValue& operator=(const PartitionCopyValue& other) {
        PartitionCopyValue next(other);value=next.value;return *this;
    }
    PartitionCopyValue(PartitionCopyValue&&) noexcept=default;
    PartitionCopyValue& operator=(PartitionCopyValue&&) noexcept=default;
};
struct PartitionIdentityHash {size_t operator()(uint64_t value) const noexcept{return size_t(value);}};
}

TEST(ReplayForestPartitions, PagesCopyOnlyTouchedValuesAndPreserveTailReuse) {
    using Pages=consensus::detail::ForestPages<PartitionCopyValue>;
    Pages original;original.resize(1025);
    for(size_t i=0;i<original.size();++i)original.Set(i,PartitionCopyValue(i+1));
    PartitionCopyValue::copies=0;
    Pages next=original;EXPECT_EQ(PartitionCopyValue::copies,0u);
    next.Set(511,PartitionCopyValue(9001));
    EXPECT_EQ(PartitionCopyValue::copies,256u);
    EXPECT_EQ(original[511].value,512u);EXPECT_EQ(next[511].value,9001u);
    // A second write in the same detached page does not recopy its values.
    next.Set(510,PartitionCopyValue(9002));EXPECT_EQ(PartitionCopyValue::copies,256u);
    const Pages retained=next;
    next.resize(257);next.resize(1025);
    EXPECT_EQ(next[256].value,257u);
    for(size_t i=257;i<next.size();++i)EXPECT_EQ(next[i].value,0u);
    EXPECT_EQ(retained[511].value,9001u);EXPECT_EQ(retained[1024].value,1025u);
    next.resize(256);next.push_back(PartitionCopyValue(777));
    EXPECT_EQ(next.size(),257u);EXPECT_EQ(next[256].value,777u);
    EXPECT_EQ(original.size(),1025u);EXPECT_EQ(original[256].value,257u);
    static_assert(std::is_same_v<decltype(next[0]),const PartitionCopyValue&>);
    size_t at=0;for(const auto& v:original){EXPECT_EQ(v.value,++at);}EXPECT_EQ(at,1025u);
}

TEST(ReplayForestPartitions, HashPartitionsRetainVersionsAndMatchReferenceInventory) {
    using Map=consensus::detail::ForestMap<uint64_t,PartitionCopyValue,PartitionIdentityHash>;
    Map original;
    for(uint64_t i=0;i<4096;++i)original.Set(i,PartitionCopyValue(i+1));
    PartitionCopyValue::copies=0;
    auto changed=original;EXPECT_EQ(PartitionCopyValue::copies,0u);
    changed.Set(17,PartitionCopyValue(99999));
    EXPECT_EQ(PartitionCopyValue::copies,17u); // 16 retained entries plus the assigned value
    EXPECT_EQ(original.find(17)->second.value,18u);EXPECT_EQ(changed.find(17)->second.value,99999u);
    const auto snapshot=changed;
    changed.erase(17);changed.Set(4096,PartitionCopyValue(6000));
    EXPECT_EQ(snapshot.count(17),1u);EXPECT_EQ(snapshot.count(4096),0u);
    EXPECT_EQ(original.size(),4096u);EXPECT_EQ(changed.size(),4096u);
    // Deterministic reference comparison covers collisions, duplicates, erase,
    // cross-partition iteration and predicate deletion without iterator writes.
    consensus::detail::ForestMap<uint64_t,uint64_t,PartitionIdentityHash,8> map;
    consensus::detail::ForestSet<uint64_t,PartitionIdentityHash,8> set;
    std::map<uint64_t,uint64_t> reference;std::set<uint64_t> set_reference;
    uint64_t rng=7;
    for(unsigned n=0;n<3000;++n) {
        rng=rng*6364136223846793005ULL+1;const uint64_t key=(rng>>32)%519;
        if(n%3){map.Set(key,n);set.insert(key);reference[key]=n;set_reference.insert(key);}
        else {EXPECT_EQ(map.erase(key),reference.erase(key));EXPECT_EQ(set.erase(key),set_reference.erase(key));}
        if(n%101==0) {
            const auto old=map;const auto old_set=set;
            map.EraseIf([](auto k){return k%5==0;});set.EraseIf([](auto k){return k%5==0;});
            for(auto it=reference.begin();it!=reference.end();)if(it->first%5==0)it=reference.erase(it);else ++it;
            for(auto it=set_reference.begin();it!=set_reference.end();)if(*it%5==0)it=set_reference.erase(it);else ++it;
            for(const auto& item:old)EXPECT_EQ(old_set.count(item.first),1u);
        }
        EXPECT_EQ(map.size(),reference.size());EXPECT_EQ(set.size(),set_reference.size());
    }
    const std::map<uint64_t,uint64_t> actual(map.begin(),map.end());
    const std::set<uint64_t> actual_set(set.begin(),set.end());
    EXPECT_EQ(actual,reference);EXPECT_EQ(actual_set,set_reference);
}

TEST(ReplayForestPartitions, FailedCopiesLeaveBothVersionsAvailable) {
    using Pages=consensus::detail::ForestPages<PartitionCopyValue>;
    using Map=consensus::detail::ForestMap<uint64_t,PartitionCopyValue,PartitionIdentityHash>;
    Pages pages;pages.resize(512);pages.Set(299,PartitionCopyValue(11));
    Map map;for(uint64_t i=0;i<1024;++i)map.Set(i,PartitionCopyValue(i));
    const auto old_pages=pages;const auto old_map=map;
    PartitionCopyValue::remaining=3;
    EXPECT_THROW(pages.Set(299,PartitionCopyValue(22)),std::runtime_error);
    PartitionCopyValue::remaining=-1;
    EXPECT_EQ(pages[299].value,11u);EXPECT_EQ(old_pages[299].value,11u);
    PartitionCopyValue::remaining=1;
    EXPECT_THROW(map.Set(299,PartitionCopyValue(88)),std::runtime_error);
    PartitionCopyValue::remaining=-1;
    EXPECT_EQ(map.find(299)->second.value,299u);EXPECT_EQ(old_map.find(299)->second.value,299u);
    EXPECT_EQ(map.size(),1024u);
    pages.Set(299,PartitionCopyValue(22));map.Set(299,PartitionCopyValue(88));
    EXPECT_EQ(pages[299].value,22u);EXPECT_EQ(map.find(299)->second.value,88u);
    EXPECT_EQ(old_pages[299].value,11u);EXPECT_EQ(old_map.find(299)->second.value,299u);
}

TEST(ReplayForestPartitions, RealForestRollbackRetainsSerializedStateAcrossPages) {
    auto set=consensus::ConsensusUTXOSet::CreateForReplay();
    auto& forest=set->GetForest();forest.setCanonicalEmptyRoots(true);
    for(uint32_t i=0;i<1025;++i)ASSERT_EQ(forest.add(SharingLeaf(i)),i);
    const std::vector<std::pair<uint64_t,consensus::UtreexoHash>> removals{
        {0,SharingLeaf(0)},{255,SharingLeaf(255)},{256,SharingLeaf(256)},
        {511,SharingLeaf(511)},{1024,SharingLeaf(1024)}};
    ASSERT_TRUE(forest.removeAtKnownPositions(removals));
    ASSERT_TRUE(set->AddCoin(JournalPoint(1),JournalCoin(1)));
    set->SetBestBlock(JournalPoint(8).txid.AsUint256(),8);
    const auto before=set->Snapshot();const auto captured=forest.clone();
    const auto bytes=forest.serialize();const auto exact=forest.dumpInternalState();
    for(unsigned action=0;action<4;++action) {
        SCOPED_TRACE(action);
        {
            auto rollback=set->BeginBlockConnectRollback();ASSERT_TRUE(rollback);
            ASSERT_TRUE(set->SpendCoin(JournalPoint(1)));
            ASSERT_TRUE(forest.restoreDeletedLeaf(256,SharingLeaf(256)));
            ASSERT_NE(forest.add(SharingLeaf(2000+action)),UINT64_MAX);
            if(action==0)ASSERT_TRUE(forest.removeAtKnownPosition(257,SharingLeaf(257)));
            if(action==1)ASSERT_TRUE(forest.removeLastNLeaves(1));
            if(action==2){forest.setCanonicalEmptyRoots(false);forest.rebuildRoots();}
            if(action==3){auto replacement=SharingForest();set->ReplaceForestGuarded(std::move(replacement));}
            set->SetBestBlock(JournalPoint(9).txid.AsUint256(),9);
        }
        ExpectJournalState(*set,before);
        EXPECT_EQ(forest.serialize(),bytes);EXPECT_EQ(forest.dumpInternalState(),exact);
        EXPECT_EQ(captured.serialize(),bytes);EXPECT_EQ(captured.dumpInternalState(),exact);
        ASSERT_TRUE(forest.prove(257));EXPECT_TRUE(forest.isDeleted(256));
    }
    const auto restored=consensus::UtreexoForest::deserialize(bytes);
    EXPECT_EQ(restored.dumpInternalState(),exact);
}
} // namespace dinero
