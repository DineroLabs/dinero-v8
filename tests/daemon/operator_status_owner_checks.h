#pragma once
#include "consensus/block_index.h"
#include "consensus/block_lifecycle.h"
#include <rocksdb/write_batch.h>
namespace dinero {
namespace {
struct OperatorStatusGraph {
    decltype(g_block_index) previous_index;
    decltype(g_candidates) previous_candidates;
    decltype(g_orphan_pool) previous_orphans;
    OperatorStatusGraph() {
        std::lock_guard<std::recursive_mutex> guard(g_block_index_mutex);
        previous_index=std::move(g_block_index);previous_candidates=std::move(g_candidates);
        previous_orphans=std::move(g_orphan_pool);
        g_block_index.clear();g_candidates.clear();g_orphan_pool.clear();InvalidateAncestryCache();
    }
    ~OperatorStatusGraph() {
        std::lock_guard<std::recursive_mutex> guard(g_block_index_mutex);
        g_candidates.clear();g_orphan_pool.clear();g_block_index.clear();
        g_block_index=std::move(previous_index);g_candidates=std::move(previous_candidates);
        g_orphan_pool=std::move(previous_orphans);InvalidateAncestryCache();
    }
};
struct OperatorStatusFixture {
    OperatorStatusGraph graph;
    std::filesystem::path path;
    ChainDB db;
    ChainstateService service;
    ChainWriteToken token=ChainWriteToken::CreateForTesting();
    CBlockIndex* target=nullptr;
    CBlockIndex* child=nullptr;
    static void Require(bool ok){if(!ok)throw std::runtime_error("operator status fixture setup");}
    explicit OperatorStatusFixture(bool store_metadata=true) {
        auto name=(std::filesystem::temp_directory_path()/"operator_status_XXXXXX").string();
        Require(mkdtemp(name.data())!=nullptr);path=name;
        Require(db.init(path/"chain")==Status::Ok);service.setChainDB(&db);
        auto header=SelectedGenesis().header;
        header.nonce=991;header.prev_block_hash=SelectedGenesis().GetHash();
        target=AddBlockIndex(header,1);Require(target!=nullptr);
        header.prev_block_hash=target->hash;header.nonce=992;
        child=AddBlockIndex(header,2);Require(child!=nullptr);
        if (store_metadata) for(auto* node:{target,child}) {
            ChainDB::PersistedHeaderMetadata metadata;
            metadata.height=node->height;metadata.parent_hash=node->prev_hash;
            metadata.chainwork=ChainworkFromHex(node->chainwork);metadata.status_flags=node->status;
            Require(db.putHeaderMetadata(token,node->hash,metadata)==Status::Ok);
        }
    }
    ~OperatorStatusFixture(){service.setChainDB(nullptr);db.close();std::filesystem::remove_all(path);}
    void Counter(const std::string& value) {
        Require(db.putUtreexoMeta(token,consensus::kBlockStatusGenerationKey,value)==Status::Ok);
    }
};
}
TEST(OperatorStatusOwner, CheckedCounterAndStagedCommit) {
    OperatorStatusFixture f;
    const auto guard=f.service.AcquireBlockIngressActivationLock();
    EXPECT_EQ(f.service.ReadBlockStatusGeneration().state,consensus::GenerationReadState::Absent);
    rocksdb::WriteBatch batch;
    const auto next=f.service.StageNextBlockStatusGeneration(f.token,batch);ASSERT_TRUE(next);EXPECT_EQ(*next,1u);
    EXPECT_EQ(f.service.ReadBlockStatusGeneration().state,consensus::GenerationReadState::Absent);
    ASSERT_EQ(f.db.writeBatch(f.token,std::move(batch),true),Status::Ok);
    EXPECT_EQ(f.service.ReadBlockStatusGeneration().value,1u);
    for(const auto& bad:std::vector<std::string>{"","x","-1"," 1","18446744073709551616",std::string("1\0x",3)}) {
        f.Counter(bad);rocksdb::WriteBatch refused;
        EXPECT_EQ(f.service.ReadBlockStatusGeneration().state,consensus::GenerationReadState::Error);
        EXPECT_FALSE(f.service.StageNextBlockStatusGeneration(f.token,refused));EXPECT_EQ(refused.Count(),0u);
        ASSERT_TRUE(f.db.getUtreexoMeta(consensus::kBlockStatusGenerationKey).ok());
        EXPECT_EQ(*f.db.getUtreexoMeta(consensus::kBlockStatusGenerationKey),bad);
    }
    f.Counter("18446744073709551615");rocksdb::WriteBatch exhausted;
    EXPECT_TRUE(f.service.ReadBlockStatusGeneration().usable());
    EXPECT_FALSE(f.service.StageNextBlockStatusGeneration(f.token,exhausted));EXPECT_EQ(exhausted.Count(),0u);
    f.Counter("00017");rocksdb::WriteBatch normal;
    ASSERT_TRUE(f.service.StageNextBlockStatusGeneration(f.token,normal));
    EXPECT_EQ(f.service.ReadBlockStatusGeneration().value,17u);
    ASSERT_EQ(f.db.writeBatch(f.token,std::move(normal),true),Status::Ok);
    EXPECT_EQ(f.service.ReadBlockStatusGeneration().value,18u);
    f.db.close();rocksdb::WriteBatch unavailable;
    EXPECT_FALSE(f.service.StageNextBlockStatusGeneration(f.token,unavailable));EXPECT_EQ(unavailable.Count(),0u);
    ASSERT_EQ(f.db.init(f.path/"chain"),Status::Ok);EXPECT_EQ(f.service.ReadBlockStatusGeneration().value,18u);
}
TEST(OperatorStatusOwner, InvalidateRefusalAndDurableDecision) {
    OperatorStatusFixture f;std::string error;
    const auto target_status=f.target->status,child_status=f.child->status;
    for(const auto& bad:{"unavailable","18446744073709551616","18446744073709551615"}) {
        f.Counter(bad);EXPECT_FALSE(f.service.InvalidateBlock(f.target->hash,error));
        EXPECT_EQ(error,"Operator status generation unavailable");
        EXPECT_EQ(f.target->status,target_status);EXPECT_EQ(f.child->status,child_status);
        ASSERT_TRUE(f.db.getHeaderMetadata(f.target->hash).ok());
        EXPECT_EQ(f.db.getHeaderMetadata(f.target->hash)->status_flags,target_status);
        EXPECT_EQ(f.db.getHeaderMetadata(f.child->hash)->status_flags,child_status);
    }
    f.Counter("7");ASSERT_TRUE(f.service.InvalidateBlock(f.target->hash,error))<<error;
    EXPECT_NE(f.target->status&BLOCK_FAILED_VALID,0u);EXPECT_NE(f.child->status&BLOCK_FAILED_CHILD,0u);
    EXPECT_EQ(f.service.ReadBlockStatusGeneration().value,8u);
    f.db.close();ASSERT_EQ(f.db.init(f.path/"chain"),Status::Ok);
    EXPECT_EQ(f.db.getHeaderMetadata(f.target->hash)->status_flags,f.target->status);
    EXPECT_EQ(f.db.getHeaderMetadata(f.child->hash)->status_flags,f.child->status);
    EXPECT_EQ(f.service.ReadBlockStatusGeneration().value,8u);
}
TEST(OperatorStatusOwner, ReconsiderRefusalPreservesDecisionThenCommits) {
    OperatorStatusFixture f;std::string error;f.Counter("3");
    ASSERT_TRUE(f.service.InvalidateBlock(f.target->hash,error))<<error;
    const auto target_status=f.target->status,child_status=f.child->status;
    for(const auto& bad:{"unavailable","18446744073709551616","18446744073709551615"}) {
        f.Counter(bad);EXPECT_FALSE(f.service.ReconsiderBlock(f.target->hash,error));
        EXPECT_EQ(error,"Operator status generation unavailable");
        EXPECT_EQ(f.target->status,target_status);EXPECT_EQ(f.child->status,child_status);
        EXPECT_EQ(f.db.getHeaderMetadata(f.target->hash)->status_flags,target_status);
        EXPECT_EQ(f.db.getHeaderMetadata(f.child->hash)->status_flags,child_status);
    }
    f.Counter("4");f.db.close();EXPECT_FALSE(f.service.ReconsiderBlock(f.target->hash,error));
    EXPECT_EQ(f.target->status,target_status);EXPECT_EQ(f.child->status,child_status);
    ASSERT_EQ(f.db.init(f.path/"chain"),Status::Ok);
    ASSERT_TRUE(f.service.ReconsiderBlock(f.target->hash,error))<<error;
    EXPECT_EQ(f.target->status&(BLOCK_FAILED_VALID|BLOCK_FAILED_CHILD),0u);
    EXPECT_EQ(f.child->status&(BLOCK_FAILED_VALID|BLOCK_FAILED_CHILD),0u);
    EXPECT_EQ(f.service.ReadBlockStatusGeneration().value,5u);
    f.db.close();ASSERT_EQ(f.db.init(f.path/"chain"),Status::Ok);
    EXPECT_EQ(f.db.getHeaderMetadata(f.target->hash)->status_flags,f.target->status);
    EXPECT_EQ(f.db.getHeaderMetadata(f.child->hash)->status_flags,f.child->status);
    EXPECT_EQ(f.service.ReadBlockStatusGeneration().value,5u);
}
TEST(OperatorStatusOwner, HeaderOnlyEnrollmentAndMissingBodyMetadataRefusal) {
    {
        OperatorStatusFixture f(false);std::string error;
        ASSERT_EQ(f.db.getHeaderMetadata(f.target->hash).status(),Status::NotFound);
        ASSERT_EQ(f.db.getHeaderMetadata(f.child->hash).status(),Status::NotFound);
        ASSERT_TRUE(f.service.InvalidateBlock(f.target->hash,error))<<error;
        ASSERT_TRUE(f.db.getHeaderMetadata(f.target->hash).ok());
        ASSERT_TRUE(f.db.getHeaderMetadata(f.child->hash).ok());
        EXPECT_EQ(f.db.getHeaderMetadata(f.target->hash)->height,1);
        EXPECT_EQ(f.db.getHeaderMetadata(f.child->hash)->parent_hash,f.target->hash);
        EXPECT_EQ(f.db.getHeaderMetadata(f.target->hash)->status_flags,f.target->status);
        EXPECT_EQ(f.db.getHeaderMetadata(f.child->hash)->status_flags,f.child->status);
        EXPECT_EQ(f.service.ReadBlockStatusGeneration().value,1u);
        ASSERT_TRUE(f.service.ReconsiderBlock(f.target->hash,error))<<error;
        EXPECT_EQ(f.target->status&(BLOCK_FAILED_VALID|BLOCK_FAILED_CHILD),0u);
        EXPECT_EQ(f.child->status&(BLOCK_FAILED_VALID|BLOCK_FAILED_CHILD),0u);
        EXPECT_EQ(f.service.ReadBlockStatusGeneration().value,2u);
    }
    {
        OperatorStatusFixture f(false);std::string error;
        // A data-bearing index with absent durable metadata must not receive
        // fabricated file locations or a successful operator decision.
        f.target->status|=BLOCK_HAVE_DATA;
        const auto original=f.target->status;
        EXPECT_FALSE(f.service.InvalidateBlock(f.target->hash,error));
        EXPECT_EQ(error,"Failed to persist invalid target decision");
        EXPECT_EQ(f.target->status,original);
        EXPECT_EQ(f.child->status&(BLOCK_FAILED_VALID|BLOCK_FAILED_CHILD),0u);
        EXPECT_EQ(f.db.getHeaderMetadata(f.target->hash).status(),Status::NotFound);
        EXPECT_EQ(f.service.ReadBlockStatusGeneration().state,consensus::GenerationReadState::Absent);
    }
}
} // namespace dinero
