#pragma once
#include <fstream>
#include <iterator>
din::Json rpc_context_gettransaction(const ExecutionContext& ctx, const din::Json& params);  // methods_blockchain_context.cpp
din::Json rpc_context_getblockfilters(const ExecutionContext& ctx, const din::Json& params);
namespace dinero {
class ChainDBTransactionReadTestPeer {
public:
    static rocksdb::ColumnFamilyHandle* HandoffMeta(const ChainDB& db) {
        return db.cf_[db.idx_meta_].get();
    }
    static Status HandoffReadOnly(ChainDB& db,const std::filesystem::path& path){return db.initReadOnlyForHandoff(path);}
    // Serialized detached-handoff fixtures only. Capture all durable logical
    // rows so a refusal must leave the actual store unchanged, including the
    // deliberately altered before-image under test.
    static auto HandoffRows(const ChainDB& db) {
        std::map<std::pair<std::string,std::string>,std::string> rows;
        for(const auto& family:db.cf_) {
            std::unique_ptr<rocksdb::Iterator> it(db.db_->NewIterator(rocksdb::ReadOptions(),family.get()));
            for(it->SeekToFirst();it->Valid();it->Next())
                rows.emplace(std::make_pair(family->GetName(),it->key().ToString()),it->value().ToString());
            if(!it->status().ok())throw std::runtime_error("handoff fixture row read failed");
        }
        return rows;
    }
    static std::string HandoffOrchardRow(const ChainDB& db,const std::string& key) {
        std::string bytes;
        if(!db.db_->Get(rocksdb::ReadOptions(),db.shieldedStateHandle(),key,&bytes).ok())
            throw std::runtime_error("handoff fixture Orchard row absent");
        return bytes;
    }
    static void HandoffPutOrchardRow(ChainDB& db,const ChainWriteToken& token,
        const std::string& key,const std::string& bytes) {
        rocksdb::WriteBatch batch;batch.Put(db.shieldedStateHandle(),key,bytes);
        if(db.writeBatch(token,std::move(batch),true)!=Status::Ok)
            throw std::runtime_error("handoff fixture Orchard row write failed");
    }
    static void HandoffDeleteOrchardRow(ChainDB& db,const ChainWriteToken& token,const std::string& key){
        rocksdb::WriteBatch batch;batch.Delete(db.shieldedStateHandle(),key);
        if(db.writeBatch(token,std::move(batch),true)!=Status::Ok)throw std::runtime_error("handoff fixture row delete failed");
    }
    // Serialized fault injection in disposable snapshot startup fixtures.
    static void SnapshotCatalogFault(ChainDB& db,const ChainWriteToken& token,
        const uint256& id,const std::optional<std::string>& bytes) {
        const auto key=std::string("Morchard_catalog_v1/node/")+
            std::string(reinterpret_cast<const char*>(id.data),32);
        rocksdb::WriteBatch batch;
        if(bytes)batch.Put(db.cf_[db.idx_utreexo_].get(),key,*bytes);
        else batch.Delete(db.cf_[db.idx_utreexo_].get(),key);
        if(db.writeBatch(token,std::move(batch),true)!=Status::Ok)
            throw std::runtime_error("snapshot fixture catalog fault failed");
    }
    static auto OrchardComparisonIterator(const ChainDB& db) {
        return std::unique_ptr<rocksdb::Iterator>(db.db_->NewIterator(
            rocksdb::ReadOptions(), db.shieldedStateHandle()));
    }
    static Status CompareOrchardIterators(const ChainDB& db,
        rocksdb::Iterator& actual, rocksdb::Iterator& expected) {
        return db.compareOrchardStorageIterators(actual, expected);
    }
    static bool PutLocationRow(ChainDB& db,const uint256& id,const std::string& bytes) {
        return db.db_->Put(rocksdb::WriteOptions(),db.cf_[db.idx_txindex_].get(),db.makeTxIndexKey(id),bytes).ok();
    }
};
namespace {
din::Json ReadTransactionRpc(DaemonContext& daemon,const std::string& id,bool verbose=false) {
    ExecutionContext context;context.daemon=&daemon;din::Json request=din::arr();request.append(id);request.append(verbose);
    return ::rpc_context_wallet_getrawtransaction(context,request);
}
// The explorer's lookup (blockchain.gettransaction), as the desktop Chain
// Explorer and the public block explorer call it.
din::Json ExplorerTransactionRpc(DaemonContext& daemon,const std::string& id) {
    ExecutionContext context;context.daemon=&daemon;din::Json request=din::arr();request.append(id);
    return ::rpc_context_gettransaction(context,request);
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
// Mainnet block 165,213: the first block with an Orchard transaction. The
// explorer answered "Transaction not found in blockchain" for both of its
// transactions, the miner's reward included, because it read blocks with a
// reader that cannot read such a block and took "unreadable" for "not here".
TEST(OrchardTransactionRpc, ExplorerLookupFindsEveryTransactionOfABlockWithAnOrchardTransaction) {
    CanonicalPoolFixture f;const auto block=f.Build();ASSERT_TRUE(block);ASSERT_TRUE(f.Submit(block->WireBytes()).accepted());
    const auto hash=block->Header().GetHash().GetHex();
    const auto tip=f.f.db.getTip();ASSERT_TRUE(tip.ok());
    const auto& reward=block->Transactions()[0];const auto& shielded=block->Transactions()[1];
    ASSERT_FALSE(reward.IsOrchard());ASSERT_TRUE(shielded.IsOrchard());

    const auto first=ExplorerTransactionRpc(f.context,reward.GetTxid().AsUint256().GetHex());
    ASSERT_FALSE(first.isMember("error"))<<first.toStyledString();
    EXPECT_EQ(first["status"].asString(),"confirmed");EXPECT_EQ(first["blockhash"].asString(),hash);
    EXPECT_EQ(first["blockheight"].asUInt(),tip->height);EXPECT_EQ(first["confirmations"].asInt(),1);
    EXPECT_TRUE(first["is_coinbase"].asBool());

    // The fixture's early blocks are not kept on disk; keep the one holding
    // the coin this transaction spends, as a node does, so the input resolves.
    const auto spent=ReadTransactionRpc(f.context,shielded.GetTxid().AsUint256().GetHex(),true)["vin"][0]["txid"].asString();
    bool funded=false;
    for(size_t height=0;height<f.f.blocks.size()&&!funded;++height) {
        const auto& early=f.f.blocks[height];
        for(size_t i=0;i<early.vtx.size();++i) {
            if(early.vtx[i].GetTxid().AsUint256().GetHex()!=spent)continue;
            ASSERT_EQ(f.f.db.putTxIndex(f.f.token,early.vtx[i].GetTxid().AsUint256(),early.GetHash(),uint32_t(i)),Status::Ok);
            const auto location=f.files->writeBlock(early.GetHash(),early);ASSERT_TRUE(location.ok());
            const auto work=f.f.db.getBlockWork(early.GetHash());ASSERT_TRUE(work.ok());
            ChainDB::PersistedHeaderMetadata metadata;metadata.height=uint32_t(height);
            metadata.parent_hash=early.header.prev_block_hash;metadata.chainwork=*work;
            metadata.status_flags=BLOCK_HAVE_DATA|BLOCK_VALID_CHAIN|BLOCK_VALID_SCRIPTS;
            metadata.file_number=location->file_number;metadata.data_pos=location->offset;metadata.data_size=location->size;
            ASSERT_EQ(f.f.db.putHeaderMetadata(f.f.token,early.GetHash(),metadata),Status::Ok);
            funded=true;break;
        }
    }
    ASSERT_TRUE(funded);

    const auto id=shielded.GetTxid().AsUint256();
    const auto second=ExplorerTransactionRpc(f.context,id.GetHex());
    ASSERT_FALSE(second.isMember("error"))<<second.toStyledString();
    EXPECT_EQ(second["txid"].asString(),id.GetHex());EXPECT_EQ(second["status"].asString(),"confirmed");
    EXPECT_EQ(second["blockhash"].asString(),hash);EXPECT_EQ(second["blockheight"].asUInt(),tip->height);
    EXPECT_EQ(second["confirmations"].asInt(),1);EXPECT_EQ(second["classification"].asString(),"orchard");
    EXPECT_FALSE(second["is_coinbase"].asBool());
    EXPECT_EQ(second["fee_una"].asUInt64(),shielded.Orchard().ExplicitFee());
    ASSERT_EQ(second["inputs"].size(),shielded.Orchard().Inputs().size());
    ASSERT_EQ(second["outputs"].size(),shielded.Orchard().Outputs().size());
    uint64_t out=0;
    for(size_t i=0;i<shielded.Orchard().Outputs().size();++i) {
        const auto& item=second["outputs"][Json::ArrayIndex(i)];
        EXPECT_EQ(item["value_una"].asUInt64(),shielded.Orchard().Outputs()[i].amount_una);
        EXPECT_EQ(item["scriptPubKey"].asString(),util::hex(shielded.Orchard().Outputs()[i].script_pub_key));
        out+=shielded.Orchard().Outputs()[i].amount_una;
    }
    EXPECT_EQ(second["total_output_value_una"].asUInt64(),out);
    // When its inputs resolve, what went into or came out of the shielded
    // pool is the public difference, and the three parts must add up.
    ASSERT_TRUE(second.isMember("total_input_value_una"))<<second.toStyledString();
    {
        const uint64_t in=second["total_input_value_una"].asUInt64(),moved=second["orchard_value_una"].asUInt64();
        const auto way=second["orchard_value_direction"].asString();
        if(way=="into_orchard")EXPECT_EQ(in,out+shielded.Orchard().ExplicitFee()+moved);
        else if(way=="out_of_orchard")EXPECT_EQ(in+moved,out+shielded.Orchard().ExplicitFee());
        else EXPECT_EQ(in,out+shielded.Orchard().ExplicitFee());
    }

    // Block filters: the block this reader cannot serve is named as missing,
    // not left out as though the range had no such block.
    {
        ExecutionContext context;context.daemon=&f.context;din::Json request=din::arr();
        request.append(static_cast<int>(tip->height));request.append(1);
        const auto filters=::rpc_context_getblockfilters(context,request);
        ASSERT_FALSE(filters.isMember("error"))<<filters.toStyledString();
        ASSERT_TRUE(filters.isMember("unavailable_heights"))<<filters.toStyledString();
        EXPECT_EQ(filters["filters"].size()+filters["unavailable_heights"].size(),1u)<<filters.toStyledString();
        for(const auto& height:filters["unavailable_heights"])EXPECT_EQ(height.asUInt(),tip->height);
    }

    // A transaction that is on the chain but cannot be read is "unavailable",
    // never "not found"; one that is not on the chain is "not found".
    f.f.service->setBlockStorage(nullptr);
    for(const auto* tx:{&reward,&shielded}) {
        const auto refused=ExplorerTransactionRpc(f.context,tx->GetTxid().AsUint256().GetHex());
        EXPECT_EQ(refused["error"].asString(),"Transaction data unavailable")<<refused.toStyledString();
        EXPECT_EQ(refused["status"].asString(),"unavailable");
    }
    f.f.service->setBlockStorage(f.files);
    EXPECT_EQ(ExplorerTransactionRpc(f.context,id.GetHex()),second);
    const auto absent=ExplorerTransactionRpc(f.context,std::string(63,'0')+"7");
    EXPECT_EQ(absent["error"].asString(),"Transaction not found in blockchain");EXPECT_EQ(absent["status"].asString(),"not_found");
}
// Measured on the v8.2.6 Mac pilot (2026-11-24): the selected-tip audit, which
// stages and abandons a full disconnect of the tip on the live forest, ran
// five times for every accepted block (about 4 s each, under the selected
// lock) and once per mining template. A pass is now reused while nothing the
// audit reads has changed, and only then.
TEST(OrchardSelectedTipAudit, APassIsReusedOnlyWhileNothingItReadsHasChanged) {
    CanonicalPoolFixture f;const auto block=f.Build();ASSERT_TRUE(block);
    auto& service=*f.f.service;std::string why;
    ASSERT_TRUE(f.Submit(block->WireBytes()).accepted());
    ASSERT_TRUE(service.IsCanonicalStateAligned(&why))<<why;
    const auto first=service.SelectedTipAuditCounts();
    // The same tip, no write, the same coin state: five more callers, no audit.
    for(int i=0;i<5;++i)ASSERT_TRUE(service.IsCanonicalStateAligned(&why))<<why;
    auto counts=service.SelectedTipAuditCounts();
    EXPECT_EQ(counts.first,first.first);EXPECT_EQ(counts.second,first.second+5);

    // Any write to the store, even one the audit does not read, ends the reuse.
    const auto id=block->Transactions()[1].GetTxid().AsUint256();const auto hash=block->Header().GetHash();
    ASSERT_EQ(f.f.db.putTxIndex(f.f.token,id,hash,1),Status::Ok);
    ASSERT_TRUE(service.IsCanonicalStateAligned(&why))<<why;
    counts=service.SelectedTipAuditCounts();
    EXPECT_EQ(counts.first,first.first+1);
    ASSERT_TRUE(service.IsCanonicalStateAligned(&why))<<why;
    EXPECT_EQ(service.SelectedTipAuditCounts().first,first.first+1);

    // Damage is still found: the tip's durable locator no longer matches.
    const auto metadata=f.f.db.getHeaderMetadata(hash);ASSERT_TRUE(metadata.ok());
    auto damaged=*metadata;++damaged.undo_size;
    ASSERT_EQ(f.f.db.putHeaderMetadata(f.f.token,hash,damaged),Status::Ok);
    EXPECT_FALSE(service.IsCanonicalStateAligned(&why));
    EXPECT_FALSE(service.IsCanonicalStateAligned(&why));  // a failure is never remembered as anything
    const auto failed=service.SelectedTipAuditCounts();
    EXPECT_EQ(failed.first,first.first+3);
    ASSERT_EQ(f.f.db.putHeaderMetadata(f.f.token,hash,*metadata),Status::Ok);
    ASSERT_TRUE(service.IsCanonicalStateAligned(&why))<<why;
    EXPECT_EQ(service.SelectedTipAuditCounts().first,first.first+4);
    ASSERT_TRUE(service.IsCanonicalStateAligned(&why))<<why;
    EXPECT_EQ(service.SelectedTipAuditCounts().first,first.first+4);
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
