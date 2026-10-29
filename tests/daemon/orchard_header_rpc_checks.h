#pragma once
namespace {
din::Json ReadHeaderRpc(DaemonContext& daemon, const std::string& hash) {
    ExecutionContext execution; execution.daemon = &daemon;
    din::Json params = din::arr(); params.append(hash);
    return ::rpc_context_getblockheader(execution, params);
}
}
#ifdef DINERO_TEST_ORCHARD_ORIGIN
TEST(OrchardHeaderRpc, TypedHeaderWorkFlagsAndReopen) {
    CanonicalPoolFixture f;
    const auto block = f.Build(); ASSERT_TRUE(block);
    ASSERT_TRUE(f.Submit(block->WireBytes()).accepted());
    const auto hash = block->Header().GetHash();
    const auto work = f.f.db.getBlockWork(hash); ASSERT_TRUE(work.ok());
    const auto response = ReadHeaderRpc(f.context, hash.GetHex());
    ASSERT_FALSE(response.isMember("error"));
    EXPECT_EQ(response["hash"].asString(), hash.GetHex());
    EXPECT_EQ(response["height"].asUInt64(), 102u);
    EXPECT_EQ(response["previousblockhash"].asString(), block->Header().prev_block_hash.GetHex());
    EXPECT_EQ(response["merkleroot"].asString(), block->Header().merkle_root.GetHex());
    EXPECT_EQ(response["utreexo_root"].asString(), block->Header().utreexo_root.GetHex());
    EXPECT_EQ(response["utreexo_root_raw"].asString(),
        util::hex(std::vector<uint8_t>(block->Header().utreexo_root.begin(),block->Header().utreexo_root.end())));
    EXPECT_EQ(response["chainwork"].asString(), "0x" + work->GetHex());
    EXPECT_EQ(response["status_flags"].asUInt64(), f.f.service->GetActiveTip()->status);
    EXPECT_FALSE(response["failed_valid"].asBool()); EXPECT_FALSE(response["failed_child"].asBool());
    f.f.db.close(); ASSERT_EQ(f.f.db.init(f.f.path), Status::Ok);
    EXPECT_EQ(ReadHeaderRpc(f.context, hash.GetHex()), response);
    ASSERT_TRUE(ShieldedStateStartupTestAccess::DisconnectBoundary(*f.f.service,f.f.service->GetActiveTip()));
    const auto retained = ReadHeaderRpc(f.context, hash.GetHex());
    ASSERT_FALSE(retained.isMember("error"));
    EXPECT_EQ(retained["height"], response["height"]);
    EXPECT_EQ(retained["chainwork"], response["chainwork"]);
    EXPECT_EQ(retained["utreexo_root_raw"], response["utreexo_root_raw"]);
}
TEST(OrchardHeaderRpc, HistoricalArchiveAndInputRefusals) {
    CanonicalPoolFixture f; const auto& block = f.f.blocks[2];
    EXPECT_TRUE(ReadHeaderRpc(f.context,block.GetHash().GetHex()).isMember("error"));
    const auto location = f.files->writeBlock(block.GetHash(),block); ASSERT_TRUE(location.ok());
    const auto work = f.f.db.getBlockWork(block.GetHash()); ASSERT_TRUE(work.ok());
    ChainDB::PersistedHeaderMetadata metadata;
    metadata.height=2; metadata.parent_hash=block.header.prev_block_hash; metadata.chainwork=*work;
    metadata.status_flags=BLOCK_HAVE_DATA|BLOCK_VALID_CHAIN|BLOCK_VALID_SCRIPTS;
    metadata.file_number=location->file_number; metadata.data_pos=location->offset; metadata.data_size=location->size;
    ASSERT_EQ(f.f.db.putHeaderMetadata(f.f.token,block.GetHash(),metadata),Status::Ok);
    const auto response=ReadHeaderRpc(f.context,block.GetHash().GetHex());
    ASSERT_FALSE(response.isMember("error")); EXPECT_EQ(response["height"].asUInt64(),2u);
    EXPECT_EQ(response["chainwork"].asString(),"0x"+work->GetHex());
    EXPECT_EQ(response["merkleroot"].asString(),block.header.merkle_root.GetHex());
    EXPECT_EQ(ReadHeaderRpc(f.context,uint256{}.GetHex())["error"].asString(),"Block not found");
    for (const auto& invalid : {std::string("x"),std::string(64,'g'),std::string(65,'0')})
        EXPECT_EQ(ReadHeaderRpc(f.context,invalid)["error"].asString(),"Invalid block hash");
    DaemonContext absent; EXPECT_TRUE(ReadHeaderRpc(absent,block.GetHash().GetHex()).isMember("error"));
    f.f.CheckUnpublished();
}
TEST(OrchardHeaderRpc, TypedStorageAndProfileRefusals) {
    CanonicalPoolFixture f; const auto block=f.Build(); ASSERT_TRUE(block);
    ASSERT_TRUE(f.Submit(block->WireBytes()).accepted()); const auto hash=block->Header().GetHash();
    f.f.service->setBlockStorage(nullptr);
    EXPECT_EQ(ReadHeaderRpc(f.context,hash.GetHex())["error"].asString(),"Block data unavailable");
    f.f.service->setBlockStorage(f.files);
    const auto original=ReadHeaderRpc(f.context,hash.GetHex()); ASSERT_FALSE(original.isMember("error"));
    const auto branch=Params().orchard_branch_id; MutableParams().orchard_branch_id=0;
    const auto invalid=ReadHeaderRpc(f.context,hash.GetHex()); MutableParams().orchard_branch_id=branch;
    EXPECT_EQ(invalid["error"].asString(),"Block data unavailable");
    EXPECT_EQ(ReadHeaderRpc(f.context,hash.GetHex()),original);
    EXPECT_EQ(f.f.service->GetActiveTip()->hash,hash); EXPECT_EQ(f.notices->published,1u);
}
#else
TEST(OrchardHeaderRpc, MissingOwnerRefusesWithoutBackend) {
    DaemonContext absent;
    EXPECT_TRUE(ReadHeaderRpc(absent,std::string(64,'0')).isMember("error"));
    absent.chainstate=std::make_shared<ChainstateService>();
    EXPECT_TRUE(ReadHeaderRpc(absent,std::string(64,'0')).isMember("error"));
    struct RestoreProfile { ChainParams saved; ~RestoreProfile(){MutableParams()=saved;} } restore{Params()};
    MutableParams().orchard_activation_height=10; MutableParams().orchard_branch_id=1;
    auto name=(std::filesystem::temp_directory_path()/"orchard_header_rpc_off_XXXXXX").string();
    ASSERT_NE(mkdtemp(name.data()),nullptr);
    struct RemoveFixture {
        std::filesystem::path path;
        ~RemoveFixture(){std::error_code ec;std::filesystem::remove_all(path,ec);}
    } cleanup{name};
    ChainDB db; ASSERT_EQ(db.init(name),Status::Ok);
    auto service=std::make_shared<ChainstateService>(); service->setChainDB(&db); absent.chainstate=service;
    struct ReleaseDatabase {
        std::shared_ptr<ChainstateService> service;
        ~ReleaseDatabase(){service->setChainDB(nullptr);}
    } release{service};
    BlockHeader header{}; header.version=1; const auto hash=header.GetHash();
    const auto token=ChainWriteToken::CreateForTesting();
    ASSERT_EQ(db.putHeader(token,hash,header,10,arith_uint256(1)),Status::Ok);
    EXPECT_EQ(service->getBlockHeaderRpcSnapshot(hash).status(),Status::Internal);
    EXPECT_EQ(ReadHeaderRpc(absent,hash.GetHex())["error"].asString(),"Block data unavailable");
}
#endif
