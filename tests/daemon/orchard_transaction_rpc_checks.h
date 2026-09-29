#pragma once
#include <fstream>
#include <iterator>
namespace dinero {
class ChainDBTransactionReadTestPeer {
public:
    static bool PutLocationRow(ChainDB& db,const uint256& id,const std::string& bytes) {
        return db.db_->Put(rocksdb::WriteOptions(),db.cf_[db.idx_txindex_].get(),db.makeTxIndexKey(id),bytes).ok();
    }
};
namespace {
din::Json ReadTransactionRpc(DaemonContext& daemon,const std::string& id,bool verbose=false) {
    ExecutionContext context;context.daemon=&daemon;din::Json request=din::arr();request.append(id);request.append(verbose);
    return ::rpc_context_wallet_getrawtransaction(context,request);
}
din::Json DecodeTransactionRpc(const std::string& bytes) {
    ExecutionContext context;din::Json request=din::arr();request.append(bytes);
    return ::rpc_context_wallet_decoderawtransaction(context,request);
}
}
#ifdef DINERO_TEST_ORCHARD_ORIGIN
TEST(OrchardTransactionRpc, TypedPoolMinedReopenAndDisconnect) {
    CanonicalPoolFixture f;const auto block=f.Build();ASSERT_TRUE(block);
    const auto& tx=block->Transactions()[1];const auto id=tx.GetTxid().AsUint256();
    const auto expected=util::hex(tx.Serialize(TxSerializationMode::WithWitness));
    const auto pooled=ReadTransactionRpc(f.context,id.GetHex());ASSERT_FALSE(pooled.isMember("error"));
    EXPECT_EQ(pooled["hex"].asString(),expected);
    const auto decoded=ReadTransactionRpc(f.context,id.GetHex(),true);ASSERT_FALSE(decoded.isMember("error"));
    EXPECT_EQ(decoded["format"].asString(),"orchard");EXPECT_EQ(decoded["txid"].asString(),id.GetHex());
    EXPECT_EQ(decoded["wtxid"].asString(),tx.GetWtxid().AsUint256().GetHex());
    EXPECT_EQ(decoded["fee_una"].asUInt64(),tx.Orchard().ExplicitFee());
    EXPECT_EQ(decoded["vin"].size(),tx.Orchard().Inputs().size());
    EXPECT_EQ(decoded["vout"].size(),tx.Orchard().Outputs().size());
    for(size_t i=0;i<tx.Orchard().Outputs().size();++i) {
        const auto& item=decoded["vout"][Json::ArrayIndex(i)];
        EXPECT_EQ(item["value_una"].asUInt64(),tx.Orchard().Outputs()[i].amount_una);
        EXPECT_EQ(item["scriptPubKey"]["hex"].asString(),util::hex(tx.Orchard().Outputs()[i].script_pub_key));
    }
    ASSERT_TRUE(f.Submit(block->WireBytes()).accepted());EXPECT_EQ(f.f.ingress->mempool().size(),0u);
    EXPECT_EQ(ReadTransactionRpc(f.context,id.GetHex()),pooled);
    EXPECT_EQ(ReadTransactionRpc(f.context,id.GetHex(),true),decoded);
    f.f.db.close();ASSERT_EQ(f.f.db.init(f.f.path),Status::Ok);
    EXPECT_EQ(ReadTransactionRpc(f.context,id.GetHex()),pooled);
    const auto captured=f.f.service->getTransactionBody(id);ASSERT_TRUE(captured.ok());
    ASSERT_TRUE(ShieldedStateStartupTestAccess::DisconnectBoundary(*f.f.service,f.f.service->GetActiveTip()));
    EXPECT_EQ(ReadTransactionRpc(f.context,id.GetHex())["error"].asString(),"Transaction not found");
    EXPECT_EQ(captured->Serialize(),tx.Serialize(TxSerializationMode::WithWitness));
    EXPECT_EQ(pooled["hex"].asString(),expected);
}
TEST(OrchardTransactionRpc, IndexIdentityOrdinalAndUnavailableOwnersRefuse) {
    CanonicalPoolFixture f;const auto block=f.Build();ASSERT_TRUE(block);ASSERT_TRUE(f.Submit(block->WireBytes()).accepted());
    const auto id=block->Transactions()[1].GetTxid().AsUint256();const auto hash=block->Header().GetHash();
    const auto original=ReadTransactionRpc(f.context,id.GetHex());ASSERT_FALSE(original.isMember("error"));
    for(uint32_t ordinal:{0u,UINT32_MAX}) {
        ASSERT_EQ(f.f.db.putTxIndex(f.f.token,id,hash,ordinal),Status::Ok);
        EXPECT_EQ(f.f.service->getTransactionBody(id).status(),Status::Corruption);
        EXPECT_EQ(ReadTransactionRpc(f.context,id.GetHex())["error"].asString(),"Transaction data unavailable");
    }
    ASSERT_EQ(f.f.db.putTxIndex(f.f.token,id,hash,1),Status::Ok);
    const auto original_tip=f.f.db.getTip();ASSERT_TRUE(original_tip.ok());
    ASSERT_EQ(f.f.db.setTip(f.f.token,original_tip->hash,101,original_tip->work),Status::Ok);
    EXPECT_EQ(f.f.service->getTransactionBody(id).status(),Status::Corruption);
    ASSERT_EQ(f.f.db.setTip(f.f.token,uint256{},original_tip->height,original_tip->work),Status::Ok);
    EXPECT_EQ(f.f.service->getTransactionBody(id).status(),Status::Internal);
    ASSERT_EQ(f.f.db.setTip(f.f.token,original_tip->hash,original_tip->height,original_tip->work),Status::Ok);
    ASSERT_EQ(f.f.db.putHeightIndex(f.f.token,102,f.f.blocks[2].GetHash()),Status::Ok);
    EXPECT_EQ(f.f.service->getTransactionBody(id).status(),Status::Corruption);
    ASSERT_EQ(f.f.db.putHeightIndex(f.f.token,102,hash),Status::Ok);
    f.f.service->setBlockStorage(nullptr);EXPECT_TRUE(ReadTransactionRpc(f.context,id.GetHex()).isMember("error"));
    f.f.service->setBlockStorage(f.files);EXPECT_EQ(ReadTransactionRpc(f.context,id.GetHex()),original);
    f.f.ingress->Stop();EXPECT_TRUE(ReadTransactionRpc(f.context,id.GetHex()).isMember("error"));
    f.context.mempool.reset();EXPECT_EQ(ReadTransactionRpc(f.context,id.GetHex()),original);
    for(const auto& bad:{std::string("x"),std::string(64,'g'),std::string(65,'0')})
        EXPECT_EQ(ReadTransactionRpc(f.context,bad)["error"].asString(),"Invalid transaction ID");
    EXPECT_EQ(f.f.service->GetActiveTip()->hash,hash);EXPECT_EQ(f.notices->published,1u);
}
TEST(OrchardTransactionRpc, MalformedLocationRowsRefuseWithoutPublication) {
    CanonicalPoolFixture f;const auto block=f.Build();ASSERT_TRUE(block);ASSERT_TRUE(f.Submit(block->WireBytes()).accepted());
    const auto id=block->Transactions()[1].GetTxid().AsUint256();const auto hash=block->Header().GetHash();
    const auto original=ReadTransactionRpc(f.context,id.GetHex());ASSERT_FALSE(original.isMember("error"));
    uint32_t ordinal=1;const auto valid=hash.GetHex()+std::string(reinterpret_cast<const char*>(&ordinal),sizeof(ordinal));
    auto wrong_hex=valid;wrong_hex[32]='g';
    for(const auto& row:std::vector<std::string>{"",valid.substr(0,67),valid+"x",wrong_hex}) {
        ASSERT_TRUE(ChainDBTransactionReadTestPeer::PutLocationRow(f.f.db,id,row));
        EXPECT_EQ(f.f.db.getTxLocation(id).status(),Status::Corruption);
        const auto response=ReadTransactionRpc(f.context,id.GetHex());
        EXPECT_EQ(response["error"].asString(),"Transaction data unavailable");EXPECT_FALSE(response.isMember("hex"));
    }
    ASSERT_EQ(f.f.db.putTxIndex(f.f.token,id,hash,1),Status::Ok);
    EXPECT_EQ(ReadTransactionRpc(f.context,id.GetHex()),original);
    EXPECT_EQ(f.f.service->GetActiveTip()->hash,hash);EXPECT_EQ(f.notices->published,1u);
}
TEST(OrchardTransactionRpc, HistoricalArchiveAndExactDecoder) {
    CanonicalPoolFixture f;const auto& block=f.f.blocks[2];const auto& tx=block.vtx[0];const auto id=tx.GetTxid().AsUint256();
    ASSERT_EQ(f.f.db.putTxIndex(f.f.token,id,block.GetHash(),0),Status::Ok);
    EXPECT_TRUE(ReadTransactionRpc(f.context,id.GetHex()).isMember("error"));
    const auto location=f.files->writeBlock(block.GetHash(),block);ASSERT_TRUE(location.ok());
    const auto work=f.f.db.getBlockWork(block.GetHash());ASSERT_TRUE(work.ok());
    ChainDB::PersistedHeaderMetadata metadata;metadata.height=2;metadata.parent_hash=block.header.prev_block_hash;metadata.chainwork=*work;
    metadata.status_flags=BLOCK_HAVE_DATA|BLOCK_VALID_CHAIN|BLOCK_VALID_SCRIPTS;
    metadata.file_number=location->file_number;metadata.data_pos=location->offset;metadata.data_size=location->size;
    ASSERT_EQ(f.f.db.putHeaderMetadata(f.f.token,block.GetHash(),metadata),Status::Ok);
    const auto raw=ReadTransactionRpc(f.context,id.GetHex());ASSERT_FALSE(raw.isMember("error"));
    EXPECT_EQ(raw["hex"].asString(),tx.SerializeHex(true));
    const auto decoded=ReadTransactionRpc(f.context,id.GetHex(),true);ASSERT_FALSE(decoded.isMember("error"));
    EXPECT_EQ(decoded["txid"].asString(),id.GetHex());EXPECT_EQ(decoded["version"].asInt(),tx.version);
    EXPECT_EQ(decoded["vin"].size(),tx.vin.size());EXPECT_EQ(decoded["vout"].size(),tx.vout.size());
    EXPECT_EQ(DecodeTransactionRpc(tx.SerializeHex(true)),decoded);
    for(const auto& bad:{std::string("g0"),std::string("0"),tx.SerializeHex(true)+"00"})
        EXPECT_TRUE(DecodeTransactionRpc(bad).isMember("error"));
    const auto typed=f.Build();ASSERT_TRUE(typed);
    const auto wire=util::hex(typed->Transactions()[1].Serialize(TxSerializationMode::WithWitness));
    EXPECT_TRUE(DecodeTransactionRpc(wire+"00").isMember("error"));
    EXPECT_FALSE(DecodeTransactionRpc(wire).isMember("error"));
}
#else
TEST(OrchardTransactionRpc, HistoricalDecodeAndMissingOwnerWithoutBackend) {
    DaemonContext absent;EXPECT_TRUE(ReadTransactionRpc(absent,std::string(64,'0')).isMember("error"));
    Transaction tx;tx.version=2;tx.vin.emplace_back();tx.vin[0].prevout.vout=UINT32_MAX;tx.vin[0].scriptSig={1,1};
    tx.vout.emplace_back(AmountUna::Una(100),std::vector<uint8_t>{0x51});
    const auto raw=tx.SerializeHex(true);const auto decoded=DecodeTransactionRpc(raw);
    ASSERT_FALSE(decoded.isMember("error"));EXPECT_EQ(decoded["txid"].asString(),tx.GetTxid().AsUint256().GetHex());
    EXPECT_TRUE(DecodeTransactionRpc(raw+"00").isMember("error"));
    EXPECT_TRUE(DecodeTransactionRpc("gg").isMember("error"));
    std::ifstream fixture(std::filesystem::path(DINERO_WALLET_RAW_FIXTURES)/"candidate-envelope.bin",std::ios::binary);
    ASSERT_TRUE(fixture.good());const std::vector<uint8_t> wire{std::istreambuf_iterator<char>(fixture),{}};
    ASSERT_FALSE(wire.empty());EXPECT_TRUE(DecodeTransactionRpc(util::hex(wire)).isMember("error"));
}
#endif

} // namespace dinero
