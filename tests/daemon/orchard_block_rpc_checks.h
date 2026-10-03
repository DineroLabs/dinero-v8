#pragma once
namespace {
din::Json ReadBlockRpc(DaemonContext& daemon,const std::string& hash,int verbosity) {
    ExecutionContext execution;execution.daemon=&daemon;din::Json params=din::arr();
    params.append(hash);params.append(verbosity);return ::rpc_context_getblock(execution,params);
}
}
#ifdef DINERO_TEST_ORCHARD_ORIGIN
TEST(OrchardBlockRpc, ExactTypedRawAndVerboseReopen) {
    CanonicalPoolFixture f;const auto block=f.Build();ASSERT_TRUE(block);
    const auto accepted=f.Submit(block->WireBytes());ASSERT_TRUE(accepted.accepted())<<accepted.reason;
    const auto hash=accepted.block_hash.GetHex();
    const auto raw=ReadBlockRpc(f.context,hash,0);ASSERT_TRUE(raw.is<std::string>());
    EXPECT_EQ(raw.asString(),util::hex(block->WireBytes()));
    const auto details=ReadBlockRpc(f.context,hash,1);ASSERT_FALSE(details.isMember("error"));
    EXPECT_EQ(details["height"].asUInt64(),102u);EXPECT_EQ(details["hash"].asString(),hash);
    ASSERT_EQ(details["tx"].size(),block->Transactions().size());
    for(size_t i=0;i<block->Transactions().size();++i)
        EXPECT_EQ(details["tx"][static_cast<Json::ArrayIndex>(i)].asString(),block->Transactions()[i].GetTxid().AsUint256().GetHex());
    EXPECT_EQ(details["utreexocommitment"].asString(),block->Header().utreexo_root.GetHex());
    EXPECT_EQ(details["nTx"].asUInt64(),block->Transactions().size());
    f.f.db.close();ASSERT_EQ(f.f.db.init(f.f.path),Status::Ok);
    EXPECT_EQ(ReadBlockRpc(f.context,hash,0).asString(),raw.asString());
    // Retained disconnected bodies are readable without claiming canonicality.
    ASSERT_TRUE(ShieldedStateStartupTestAccess::DisconnectBoundary(*f.f.service,f.f.service->GetActiveTip()));
    EXPECT_EQ(ReadBlockRpc(f.context,hash,0).asString(),raw.asString());
    EXPECT_EQ(ReadBlockRpc(f.context,hash,1)["height"].asUInt64(),102u);
}
TEST(OrchardBlockRpc, MissingAndInconsistentTypedStorageRefuse) {
    CanonicalPoolFixture f;const auto block=f.Build();ASSERT_TRUE(block);
    ASSERT_TRUE(f.Submit(block->WireBytes()).accepted());const auto hash=block->Header().GetHash();
    const auto metadata=f.f.db.getHeaderMetadata(hash);ASSERT_TRUE(metadata.ok());
    auto inconsistent=*metadata;inconsistent.parent_hash=uint256{};
    ASSERT_EQ(f.f.db.putHeaderMetadata(f.f.token,hash,inconsistent),Status::Ok);
    const auto bad=ReadBlockRpc(f.context,hash.GetHex(),0);EXPECT_TRUE(bad.isMember("error"));
    EXPECT_EQ(ReadBlockRpc(f.context,hash.GetHex(),1)["error"].asString(),"Block data unavailable");
    ASSERT_EQ(f.f.db.putHeaderMetadata(f.f.token,hash,*metadata),Status::Ok);
    const auto work=f.f.db.getBlockWork(hash);ASSERT_TRUE(work.ok());
    ASSERT_EQ(f.f.db.putHeader(f.f.token,hash,block->Header(),103,*work),Status::Ok);
    EXPECT_TRUE(ReadBlockRpc(f.context,hash.GetHex(),0).isMember("error"));
    ASSERT_EQ(f.f.db.putHeader(f.f.token,hash,block->Header(),102,*work),Status::Ok);
    f.f.service->setBlockStorage(nullptr);
    EXPECT_TRUE(ReadBlockRpc(f.context,hash.GetHex(),0).isMember("error"));
    f.f.service->setBlockStorage(f.files);
    EXPECT_EQ(ReadBlockRpc(f.context,hash.GetHex(),0).asString(),util::hex(block->WireBytes()));
    const auto branch=Params().orchard_branch_id;MutableParams().orchard_branch_id=0;
    const auto invalid_profile=ReadBlockRpc(f.context,hash.GetHex(),0);
    MutableParams().orchard_branch_id=branch;
    EXPECT_TRUE(invalid_profile.isMember("error"));
    EXPECT_EQ(f.f.service->GetActiveTip()->hash,hash);
    EXPECT_EQ(f.notices->published,1u);
}
TEST(OrchardBlockRpc, HistoricalBytesAndInputRefusals) {
    CanonicalPoolFixture f;const auto& block=f.f.blocks[2];const auto bytes=block.Serialize();
    // The base fixture has legacy DB bodies, while the service requires flatfiles.
    EXPECT_TRUE(ReadBlockRpc(f.context,block.GetHash().GetHex(),0).isMember("error"));
    const auto location=f.files->writeBlock(block.GetHash(),block);ASSERT_TRUE(location.ok());
    const auto work=f.f.db.getBlockWork(block.GetHash());ASSERT_TRUE(work.ok());
    ChainDB::PersistedHeaderMetadata metadata;metadata.height=2;metadata.parent_hash=block.header.prev_block_hash;
    metadata.chainwork=*work;metadata.status_flags=BLOCK_HAVE_DATA|BLOCK_VALID_CHAIN|BLOCK_VALID_SCRIPTS;
    metadata.file_number=location->file_number;metadata.data_pos=location->offset;metadata.data_size=location->size;
    ASSERT_EQ(f.f.db.putHeaderMetadata(f.f.token,block.GetHash(),metadata),Status::Ok);
    const auto raw=ReadBlockRpc(f.context,block.GetHash().GetHex(),0);ASSERT_TRUE(raw.is<std::string>());
    EXPECT_EQ(raw.asString(),util::hex(std::vector<uint8_t>(bytes.begin(),bytes.end())));
    const auto details=ReadBlockRpc(f.context,block.GetHash().GetHex(),1);
    ASSERT_FALSE(details.isMember("error"));EXPECT_EQ(details["height"].asUInt64(),2u);
    EXPECT_EQ(details["tx"][0].asString(),block.vtx.front().GetTxid().AsUint256().GetHex());
    EXPECT_EQ(ReadBlockRpc(f.context,uint256{}.GetHex(),0)["error"].asString(),"Block not found");
    for(const auto& invalid:{std::string("xyz"),std::string(64,'g'),std::string(65,'0')})
        EXPECT_EQ(ReadBlockRpc(f.context,invalid,0)["error"].asString(),"Invalid block hash");
    DaemonContext absent;EXPECT_TRUE(ReadBlockRpc(absent,block.GetHash().GetHex(),0).isMember("error"));
    f.f.CheckUnpublished();
}
#else
TEST(OrchardBlockRpc, MissingServiceRefusesWithoutBackend) {
    DaemonContext absent;EXPECT_TRUE(ReadBlockRpc(absent,std::string(64,'0'),0).isMember("error"));
    absent.chainstate=std::make_shared<ChainstateService>();
    EXPECT_TRUE(ReadBlockRpc(absent,std::string(64,'0'),0).isMember("error"));
    const auto saved=Params();
    struct RestoreProfile {ChainParams saved;~RestoreProfile(){MutableParams()=saved;}} restore{saved};
    MutableParams().orchard_activation_height=10;MutableParams().orchard_branch_id=1;
    auto name=(std::filesystem::temp_directory_path()/"orchard_block_rpc_off_XXXXXX").string();
    ASSERT_NE(mkdtemp(name.data()),nullptr);
    struct RemoveFixture {std::filesystem::path path;~RemoveFixture(){std::error_code ec;std::filesystem::remove_all(path,ec);}} cleanup{name};
    ChainDB db;ASSERT_EQ(db.init(name),Status::Ok);
    auto service=std::make_shared<ChainstateService>();service->setChainDB(&db);absent.chainstate=service;
    struct ReleaseDatabase {std::shared_ptr<ChainstateService> service;~ReleaseDatabase(){service->setChainDB(nullptr);}} release{service};
    BlockHeader header{};header.version=1;const auto hash=header.GetHash();
    const auto token=ChainWriteToken::CreateForTesting();
    ASSERT_EQ(db.putHeader(token,hash,header,10,arith_uint256(1)),Status::Ok);
    EXPECT_EQ(service->getBlockRpcSnapshot(hash).status(),Status::Internal);
    EXPECT_EQ(ReadBlockRpc(absent,hash.GetHex(),0)["error"].asString(),"Block data unavailable");
    ASSERT_EQ(db.putHeader(token,hash,header,-1,arith_uint256(1)),Status::Ok);
    EXPECT_EQ(service->getBlockRpcSnapshot(hash).status(),Status::Corruption);
    EXPECT_EQ(ReadBlockRpc(absent,hash.GetHex(),0)["error"].asString(),"Block data unavailable");
    service->setChainDB(nullptr);absent.chainstate.reset();service.reset();db.close();
}
#endif
