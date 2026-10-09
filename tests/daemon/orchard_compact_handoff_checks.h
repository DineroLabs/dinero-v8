#pragma once
#include <tuple>
#ifdef DINERO_TEST_ORCHARD_ORIGIN
namespace dinero {
// Test access invokes the genuine reconstruction and only inspects its result.
// It cannot construct a result, replace the source, or admit service readers.
struct OrchardCompactStartupTestAccess {
    static auto Restore(AnnotatedRecursiveMutex& mutex,ChainDB& source,
        const ChainWriteToken& token,const BlockStorage& files,
        const std::filesystem::path& datadir,const std::filesystem::path& scratch) {
        return OrchardReindexOwner::RestoreCompactPrepared(mutex,source,token,files,datadir,scratch,true);
    }
    static const auto& Headers(const OrchardCompactStartup& result) {return result.selected_headers_;}
    static const auto& Index(const OrchardCompactStartup& result) {return result.selected_index_;}
    static const auto& Undo(const OrchardCompactStartup& result) {return result.selected_undo_;}
    static bool BoundTo(const OrchardCompactStartup& result,ChainDB& source,AnnotatedRecursiveMutex& mutex) {
        return result.source_==&source && result.mutex_==&mutex && bool(result.owner_);
    }
};
}
TEST(OrchardCompactHandoff, SelectedHistoryUsesOriginalLocationsAndSurvivesReopen) {
    CompactStartupFixture fixture;
    using Access=dinero::OrchardCompactStartupTestAccess;
    const auto rows=fixture.Rows();const auto archives=fixture.ArchiveBytes();
    std::vector<BlockHeader> expected;
    for(const auto& block:fixture.f.blocks)expected.push_back(block.header);
    expected.push_back(fixture.first->Header());expected.push_back(fixture.second->Header());
    std::vector<std::tuple<uint32_t,uint32_t,uint32_t>> locations;
    for(unsigned reopen=0;reopen<2;++reopen) {
        const auto scratch=fixture.f.path/("handoff-verified-"+std::to_string(reopen));
        auto result=Access::Restore(fixture.startup_mutex,fixture.reopened,fixture.f.token,
            *fixture.files,fixture.f.path/"flatfiles",scratch);
        ASSERT_TRUE(result);EXPECT_TRUE(Access::BoundTo(*result,fixture.reopened,fixture.startup_mutex));
        AnnotatedRecursiveMutex other_mutex;
        EXPECT_FALSE(Access::BoundTo(*result,fixture.reopened,other_mutex));
        const auto& headers=Access::Headers(*result);ASSERT_EQ(headers.size(),expected.size());
        arith_uint256 work{0};
        for(size_t height=0;height<headers.size();++height) {
            const auto& header=headers[height];const auto& original=expected[height];
            work+=GetBlockProof(original.difficulty);
            EXPECT_EQ(header.height,height);EXPECT_EQ(header.header.SerializeForHash(),original.SerializeForHash());
            EXPECT_EQ(header.work,work);
            const auto metadata=fixture.reopened.getHeaderMetadata(original.GetHash());ASSERT_TRUE(metadata.ok());
            const auto location=std::make_tuple(header.file_number,header.data_pos,header.data_size);
            EXPECT_EQ(location,std::make_tuple(metadata->file_number,metadata->data_pos,metadata->data_size));
            if(!reopen)locations.push_back(location);else EXPECT_EQ(location,locations.at(height));
            const auto wire=fixture.files->readBlockBytes({header.file_number,header.data_pos,header.data_size});
            ASSERT_TRUE(wire.ok());
            if(height<fixture.f.blocks.size()) {
                const auto expected_wire=fixture.f.blocks[height].Serialize();
                EXPECT_EQ(*wire,expected_wire);
            } else {
                const auto source=height==fixture.first->Height()?fixture.first:fixture.second;
                EXPECT_EQ(std::vector<uint8_t>(wire->begin(),wire->end()),source->WireBytes());
            }
        }
        EXPECT_EQ(fixture.Rows(),rows);EXPECT_EQ(fixture.ArchiveBytes(),archives);
        EXPECT_FALSE(std::filesystem::exists(scratch));
        result.reset();
        if(!reopen){fixture.reopened.close();ASSERT_EQ(fixture.reopened.init(fixture.f.path),Status::Ok);}
    }
}
TEST(OrchardCompactHandoff, CorruptSelectedLocationReturnsNoPartialHandoff) {
    CompactStartupFixture fixture;using Access=dinero::OrchardCompactStartupTestAccess;
    const auto hash=fixture.second->Header().GetHash();
    const auto original=fixture.reopened.getHeaderMetadata(hash);ASSERT_TRUE(original.ok());
    const auto earlier=fixture.reopened.getHeaderMetadata(fixture.first->Header().GetHash());ASSERT_TRUE(earlier.ok());
    auto corrupt=*original;corrupt.file_number=earlier->file_number;corrupt.data_pos=earlier->data_pos;corrupt.data_size=earlier->data_size;
    ASSERT_EQ(fixture.reopened.putHeaderMetadata(fixture.f.token,hash,corrupt),Status::Ok);
    const auto rows=fixture.Rows();const auto archives=fixture.ArchiveBytes();
    std::unique_ptr<dinero::OrchardCompactStartup> result;
    EXPECT_THROW(result=Access::Restore(fixture.startup_mutex,fixture.reopened,fixture.f.token,
        *fixture.files,fixture.f.path/"flatfiles",fixture.f.path/"handoff-bad-location"),std::exception);
    EXPECT_FALSE(result);EXPECT_EQ(fixture.Rows(),rows);EXPECT_EQ(fixture.ArchiveBytes(),archives);
    ASSERT_EQ(fixture.reopened.putHeaderMetadata(fixture.f.token,hash,*original),Status::Ok);
    const auto restored_rows=fixture.Rows();
    result=Access::Restore(fixture.startup_mutex,fixture.reopened,fixture.f.token,
        *fixture.files,fixture.f.path/"flatfiles",fixture.f.path/"handoff-location-retry");
    ASSERT_TRUE(result);ASSERT_FALSE(Access::Headers(*result).empty());
    EXPECT_EQ(Access::Headers(*result).back().header.GetHash(),hash);
    EXPECT_EQ(fixture.Rows(),restored_rows);EXPECT_EQ(fixture.ArchiveBytes(),archives);
}

TEST(OrchardCompactIndex, PreparedAncestryUsesVerifiedWorkLocationsAndLinks) {
    CompactStartupFixture fixture;using Access=dinero::OrchardCompactStartupTestAccess;
    const auto rows=fixture.Rows();const auto archives=fixture.ArchiveBytes();
    const auto tip_hash=fixture.second->Header().GetHash();
    const auto* original_tip=dinero::FindBlockIndex(tip_hash);ASSERT_NE(original_tip,nullptr);
    const auto original_status=original_tip->status;
    auto result=Access::Restore(fixture.startup_mutex,fixture.reopened,fixture.f.token,
        *fixture.files,fixture.f.path/"flatfiles",fixture.f.path/"index-prepared");
    ASSERT_TRUE(result);
    const auto& headers=Access::Headers(*result);const auto& nodes=Access::Index(*result);
    ASSERT_EQ(nodes.size(),headers.size());ASSERT_FALSE(nodes.empty());
    arith_uint256 work{0};
    for(size_t height=0;height<nodes.size();++height) {
        ASSERT_TRUE(nodes[height]);const auto& n=*nodes[height];const auto& h=headers[height];
        work+=GetBlockProof(h.header.difficulty);
        EXPECT_EQ(n.hash,h.header.GetHash());EXPECT_EQ(n.prev_hash,h.header.prev_block_hash);
        EXPECT_EQ(n.height,height);EXPECT_EQ(n.chainwork,work.GetHex());
        EXPECT_EQ(n.version,h.header.version);EXPECT_EQ(n.merkle_root,h.header.merkle_root);
        const uint64_t expected_timestamp=h.header.timestamp;
        EXPECT_EQ(n.timestamp,expected_timestamp);EXPECT_EQ(n.bits,h.header.difficulty);
        EXPECT_EQ(n.nonce,h.header.nonce);
        EXPECT_EQ(std::make_tuple(n.file_number,n.data_pos,n.data_size),
            std::make_tuple(h.file_number,h.data_pos,h.data_size));
        EXPECT_EQ(n.pprev,height?nodes[height-1].get():nullptr);
        if(height+1<nodes.size()) {
            ASSERT_EQ(n.children.size(),1u);EXPECT_EQ(n.children.front(),nodes[height+1].get());
        } else EXPECT_TRUE(n.children.empty());
        EXPECT_EQ(n.status,uint32_t(BLOCK_VALID_HEADER|BLOCK_VALID_TREE|BLOCK_VALID_TRANSACTIONS|
            BLOCK_VALID_CHAIN|BLOCK_VALID_SCRIPTS|BLOCK_HAVE_DATA));
        EXPECT_EQ(n.undo_file,0u);EXPECT_EQ(n.undo_pos,0u);EXPECT_EQ(n.undo_size,0u);
    }
    EXPECT_EQ(nodes.back()->hash,tip_hash);
    EXPECT_NE(nodes.back().get(),original_tip);
    EXPECT_EQ(dinero::FindBlockIndex(tip_hash),original_tip);
    EXPECT_EQ(original_tip->status,original_status);
    EXPECT_EQ(fixture.Rows(),rows);EXPECT_EQ(fixture.ArchiveBytes(),archives);
    result.reset();
    EXPECT_EQ(dinero::FindBlockIndex(tip_hash),original_tip);
    EXPECT_EQ(original_tip->status,original_status);
}
TEST(OrchardCompactIndex, RawAvailabilityFlagsDoNotAuthorizePreparedNodes) {
    CompactStartupFixture fixture;using Access=dinero::OrchardCompactStartupTestAccess;
    const auto hash=fixture.second->Header().GetHash();
    auto metadata=fixture.reopened.getHeaderMetadata(hash);ASSERT_TRUE(metadata.ok());
    metadata->status_flags|=BLOCK_IN_FLIGHT|BLOCK_PRUNE_ELIGIBLE;
    ASSERT_EQ(fixture.reopened.putHeaderMetadata(fixture.f.token,hash,*metadata),Status::Ok);
    const auto rows=fixture.Rows();const auto archives=fixture.ArchiveBytes();
    for(unsigned reopen=0;reopen<2;++reopen) {
        auto result=Access::Restore(fixture.startup_mutex,fixture.reopened,fixture.f.token,
            *fixture.files,fixture.f.path/"flatfiles",fixture.f.path/("index-flags-"+std::to_string(reopen)));
        ASSERT_TRUE(result);ASSERT_FALSE(Access::Index(*result).empty());
        const auto& n=*Access::Index(*result).back();
        EXPECT_EQ(n.hash,hash);EXPECT_EQ(n.status&(BLOCK_IN_FLIGHT|BLOCK_PRUNE_ELIGIBLE|BLOCK_HAVE_UNDO),0u);
        EXPECT_EQ(n.undo_file,0u);EXPECT_EQ(n.undo_pos,0u);EXPECT_EQ(n.undo_size,0u);
        EXPECT_EQ(fixture.Rows(),rows);EXPECT_EQ(fixture.ArchiveBytes(),archives);
        result.reset();
        if(!reopen){fixture.reopened.close();ASSERT_EQ(fixture.reopened.init(fixture.f.path),Status::Ok);}
    }
}
#endif
