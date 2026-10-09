#pragma once
#include <limits>
#include <tuple>
#ifdef DINERO_TEST_ORCHARD_ORIGIN
TEST(OrchardCompactUndoHandoff, SelectedLocationsBindActualReplayAndReopen) {
    CompactStartupFixture fixture;using Access=dinero::OrchardCompactStartupTestAccess;
    const auto rows=fixture.Rows();const auto archives=fixture.ArchiveBytes();
    for(unsigned reopen=0;reopen<2;++reopen) {
        auto result=Access::Restore(fixture.startup_mutex,fixture.reopened,fixture.f.token,
            *fixture.files,fixture.f.path/"flatfiles",fixture.f.path/("undo-handoff-"+std::to_string(reopen)));
        ASSERT_TRUE(result);const auto& headers=Access::Headers(*result);const auto& undo=Access::Undo(*result);
        ASSERT_EQ(undo.size(),headers.size());size_t retained=0;
        for(size_t height=0;height<headers.size();++height) {
            if(height<Params().orchard_activation_height) {EXPECT_FALSE(undo[height]);continue;}
            ASSERT_TRUE(undo[height]);++retained;const auto& loc=*undo[height];
            const auto hash=headers[height].header.GetHash();
            const auto metadata=fixture.reopened.getHeaderMetadata(hash);ASSERT_TRUE(metadata.ok());
            EXPECT_EQ(std::make_tuple(loc.file_number,loc.data_pos,loc.data_size),
                std::make_tuple(metadata->undo_file,metadata->undo_pos,metadata->undo_size));
            const auto actual=fixture.files->readUndo({loc.file_number,loc.data_pos,loc.data_size});
            const auto expected=fixture.reopened.getUndo(hash);ASSERT_TRUE(actual.ok());ASSERT_TRUE(expected.ok());
            EXPECT_EQ(*actual,expected->Serialize());
            // Separate proof values do not mutate the unpublished body-only index.
            EXPECT_EQ(Access::Index(*result)[height]->status&BLOCK_HAVE_UNDO,0u);
        }
        EXPECT_EQ(retained,size_t(fixture.second->Height()-Params().orchard_activation_height+1));
        EXPECT_EQ(fixture.Rows(),rows);EXPECT_EQ(fixture.ArchiveBytes(),archives);
        result.reset();
        if(!reopen) {fixture.reopened.close();ASSERT_EQ(fixture.reopened.init(fixture.f.path),Status::Ok);}
    }
}
TEST(OrchardCompactUndoHandoff, WrongOriginalLocationRefusesBeforeHandoffAndRetries) {
    CompactStartupFixture fixture;using Access=dinero::OrchardCompactStartupTestAccess;
    const auto hash=fixture.second->Header().GetHash();const auto original=fixture.reopened.getHeaderMetadata(hash);
    ASSERT_TRUE(original.ok());ASSERT_LT(original->undo_pos,std::numeric_limits<uint32_t>::max());
    auto bad=*original;++bad.undo_pos;
    ASSERT_EQ(fixture.reopened.putHeaderMetadata(fixture.f.token,hash,bad),Status::Ok);
    const auto rows=fixture.Rows();const auto archives=fixture.ArchiveBytes();
    EXPECT_THROW((void)Access::Restore(fixture.startup_mutex,fixture.reopened,fixture.f.token,
        *fixture.files,fixture.f.path/"flatfiles",fixture.f.path/"undo-handoff-bad"),std::exception);
    EXPECT_EQ(fixture.Rows(),rows);EXPECT_EQ(fixture.ArchiveBytes(),archives);
    ASSERT_EQ(fixture.reopened.putHeaderMetadata(fixture.f.token,hash,*original),Status::Ok);
    auto result=Access::Restore(fixture.startup_mutex,fixture.reopened,fixture.f.token,
        *fixture.files,fixture.f.path/"flatfiles",fixture.f.path/"undo-handoff-retry");
    ASSERT_TRUE(result);ASSERT_FALSE(Access::Undo(*result).empty());ASSERT_TRUE(Access::Undo(*result).back());
    const auto& loc=*Access::Undo(*result).back();
    EXPECT_EQ(std::make_tuple(loc.file_number,loc.data_pos,loc.data_size),
        std::make_tuple(original->undo_file,original->undo_pos,original->undo_size));
    EXPECT_EQ(fixture.ArchiveBytes(),archives);
}
#endif
