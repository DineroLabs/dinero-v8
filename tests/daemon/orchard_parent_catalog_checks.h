#pragma once
#ifdef DINERO_TEST_ORCHARD_ORIGIN
#include "daemon/services/orchard_parent_catalog.h"
namespace dinero {
struct OrchardParentCatalogTestAccess {
    static auto Roots(const PreparedOrchardCatalog& p){return std::pair{p.transactions_,p.legacy_};}
    static auto Create(ChainDB& db,const ChainWriteToken& token,const OrchardParentReplay& replay,
        const OrchardHistoryCapture& history){return PreparedOrchardCatalog::Create(db,token,replay,history);}
};
namespace {
void CatalogReplay(OrchardParentReplay& replay,OrchardHistoryCapture& history,const std::vector<Block>& blocks) {
    CaptureHeaders(history,blocks);arith_uint256 work{0};
    for(uint32_t h=0;h<blocks.size();++h){work+=GetBlockProof(blocks[h].header.difficulty);replay.Append(blocks[h],h,work);history.RecordBody(h,blocks[h]);}
    replay.Finish();history.Finish();
}
std::string CatalogDiskKey(const uint256& id) {
    return std::string("Morchard_catalog_v1/node/")+std::string(reinterpret_cast<const char*>(id.data),32);
}
}
TEST(OrchardParentCatalog, CompletedServiceOwnerMatchesHistoryLegacyBoundaryAndReopen) {
    OwnedSelectedParentFixture f(21);
    auto prepared=f.service.PrepareSelectedOrchardParent();ASSERT_TRUE(prepared);
    const auto& catalog=prepared->Catalog();size_t transactions=0,legacy=0,modern=0;
    for(const auto& block:f.blocks)for(const auto& tx:block.vtx){EXPECT_TRUE(catalog.ContainsTransaction(tx.GetTxid()));++transactions;}
    EXPECT_EQ(catalog.TransactionCount(),transactions);
    for(const auto& [point,coin]:f.replay->ProvenUtxos()) {
        const auto entry=catalog.LegacyCoin(point);
        if(coin.height<consensus::GetUtreexoMaturityLeafActivationHeight()) {
            ASSERT_TRUE(entry);EXPECT_EQ(entry->height,coin.height);EXPECT_EQ(entry->coinbase,coin.isCoinbase);++legacy;
        } else {EXPECT_FALSE(entry);++modern;}
    }
    EXPECT_GT(legacy,0u);EXPECT_GT(modern,0u);EXPECT_EQ(catalog.LegacyCoinCount(),legacy);
    EXPECT_FALSE(catalog.ContainsTransaction(TxId(uint256{})));
    EXPECT_FALSE(catalog.LegacyCoin(OutPoint(TxId(uint256{}),99)));
    const auto stump=consensus::UtreexoStump::deserialize(catalog.VerificationStump());
    EXPECT_EQ(stump.getCommitment(),f.replay->Forest()->getCommitment());EXPECT_EQ(stump.getNumLeaves(),f.replay->Forest()->getNumLeaves());
    const auto roots=OrchardParentCatalogTestAccess::Roots(catalog);prepared.reset();f.CheckUnpublished();
    f.db.close();ASSERT_EQ(f.db.init(f.path),Status::Ok);
    // Structural lookup from retained roots proves durable data, not permission
    // to install these roots or bypass a completed replay on restart.
    storage::catalog::Tree tree(storage::catalog::Kind::Transactions,[&](const uint256& id){auto s=f.db.getOrchardCatalogNode(id);if(!s.ok())throw std::runtime_error("missing test node");return *s;});
    for(const auto& block:f.blocks)for(const auto& tx:block.vtx)EXPECT_TRUE(tree.Find(roots.first,storage::catalog::TransactionKey(tx.GetTxid())));
    auto again=f.service.PrepareSelectedOrchardParent();ASSERT_TRUE(again);EXPECT_EQ(OrchardParentCatalogTestAccess::Roots(again->Catalog()),roots);f.CheckUnpublished();
}
TEST(OrchardParentCatalog, IncompleteOwnerDeniedHistoryAndUnavailableStorageRefuse) {
    OwnedSelectedParentFixture f;OrchardParentReplay replay({f.tip.height,f.tip.hash,ChainworkFromHex(f.tip.chainwork)},parent_replay_limits);
    OrchardHistoryCapture history(f.tip.height,f.tip.hash);
    EXPECT_THROW(OrchardParentCatalogTestAccess::Create(f.db,f.token,replay,history),std::runtime_error);
    CatalogReplay(replay,history,f.blocks);auto* sql=OrchardHistoryCaptureTestAccess::Handle(history);
    sqlite3_set_authorizer(sql,[](void*,int action,const char*,const char*,const char*,const char*){return action==SQLITE_READ?SQLITE_DENY:SQLITE_OK;},nullptr);
    EXPECT_THROW(OrchardParentCatalogTestAccess::Create(f.db,f.token,replay,history),std::runtime_error);
    sqlite3_set_authorizer(sql,nullptr,nullptr);f.CheckUnpublished();f.db.close();
    EXPECT_THROW(OrchardParentCatalogTestAccess::Create(f.db,f.token,replay,history),std::runtime_error);
    ASSERT_EQ(f.db.init(f.path),Status::Ok);
    auto result=OrchardParentCatalogTestAccess::Create(f.db,f.token,replay,history);ASSERT_TRUE(result);EXPECT_EQ(result->TransactionCount(),4u);f.CheckUnpublished();
}
TEST(OrchardParentCatalog, InterruptedTraversalRetainsOnlyUnreachableImmutableNodes) {
    OwnedSelectedParentFixture f(300);
    OrchardParentReplay replay({f.tip.height,f.tip.hash,ChainworkFromHex(f.tip.chainwork)},parent_replay_limits);
    OrchardHistoryCapture history(f.tip.height,f.tip.hash);CatalogReplay(replay,history,f.blocks);
    f.db.close();const auto before=shielded_store_fixture::Inspect(f.path);ASSERT_EQ(f.db.init(f.path),Status::Ok);
    auto* sql=OrchardHistoryCaptureTestAccess::Handle(history);struct Reads{sqlite3* sql;unsigned rows=0;} reads{sql};
    sqlite3_trace_v2(sql,SQLITE_TRACE_ROW,[](unsigned event,void* p,void* statement,void*){
        auto& reads=*static_cast<Reads*>(p);const char* query=sqlite3_sql(static_cast<sqlite3_stmt*>(statement));
        if(event==SQLITE_TRACE_ROW&&query&&std::string(query)=="SELECT v,tag FROM records WHERE k=?1"&&++reads.rows==500)sqlite3_interrupt(reads.sql);
        return 0;
    },&reads);
    EXPECT_THROW(OrchardParentCatalogTestAccess::Create(f.db,f.token,replay,history),std::runtime_error);
    sqlite3_trace_v2(sql,0,nullptr,nullptr);EXPECT_EQ(reads.rows,500u);f.CheckUnpublished();
    f.db.close();auto after=shielded_store_fixture::Inspect(f.path);size_t nodes=0;
    auto& records=after["utreexo"];
    for(auto it=records.begin();it!=records.end();) {
        if(it->first.starts_with("Morchard_catalog_v1/node/")) {
            const auto prefix=std::string("Morchard_catalog_v1/node/");ASSERT_EQ(it->first.size(),prefix.size()+32);uint256 id;std::copy_n(reinterpret_cast<const uint8_t*>(it->first.data()+prefix.size()),32,id.begin());
            EXPECT_TRUE(storage::catalog::ValidNode(id,it->second));++nodes;it=records.erase(it);
        } else ++it;
    }
    EXPECT_GT(nodes,0u);EXPECT_EQ(after,before);ASSERT_EQ(f.db.init(f.path),Status::Ok);
    auto result=OrchardParentCatalogTestAccess::Create(f.db,f.token,replay,history);ASSERT_TRUE(result);EXPECT_EQ(result->TransactionCount(),301u);f.CheckUnpublished();
}
TEST(OrchardParentCatalog, CorruptReferencedNodeRefusesWithoutReplacement) {
    OwnedSelectedParentFixture f;auto prepared=f.service.PrepareSelectedOrchardParent();ASSERT_TRUE(prepared);
    const auto roots=OrchardParentCatalogTestAccess::Roots(prepared->Catalog());const auto original=f.db.getOrchardCatalogNode(roots.first);ASSERT_TRUE(original.ok());
    prepared.reset();f.db.close();auto names=shielded_store_fixture::legacy;names.push_back(shielded_store_fixture::shielded);
    {shielded_store_fixture::Raw raw(f.path,names);raw.put("utreexo",CatalogDiskKey(roots.first),"corrupt");}
    ASSERT_EQ(f.db.init(f.path),Status::Ok);EXPECT_EQ(f.db.getOrchardCatalogNode(roots.first).status(),Status::Corruption);
    EXPECT_FALSE(f.service.PrepareSelectedOrchardParent());EXPECT_EQ(f.db.getOrchardCatalogNode(roots.first).status(),Status::Corruption);f.CheckUnpublished();
    f.db.close();{shielded_store_fixture::Raw raw(f.path,names);raw.put("utreexo",CatalogDiskKey(roots.first),*original);}
    ASSERT_EQ(f.db.init(f.path),Status::Ok);auto retry=f.service.PrepareSelectedOrchardParent();ASSERT_TRUE(retry);EXPECT_EQ(OrchardParentCatalogTestAccess::Roots(retry->Catalog()),roots);f.CheckUnpublished();
}
TEST(OrchardParentCatalog, TypedStorageRejectsWrongHashEncodingAndCrossDomainReads) {
    namespace c=storage::catalog;OwnedSelectedParentFixture f;c::Node node{c::Kind::Transactions};node.key[0]=9;
    const auto bytes=node.Encode();const auto id=c::Hash(bytes);rocksdb::WriteBatch batch;
    EXPECT_EQ(f.db.stageOrchardCatalogNode(f.token,id,bytes+"extra",batch),Status::Invalid);EXPECT_EQ(batch.Count(),0u);
    EXPECT_EQ(f.db.stageOrchardCatalogNode(f.token,uint256{},bytes,batch),Status::Invalid);EXPECT_EQ(batch.Count(),0u);
    EXPECT_EQ(f.db.putUtreexoMeta(f.token,"orchard_catalog_v1/node/anything",bytes,&batch),Status::Invalid);EXPECT_EQ(batch.Count(),0u);
    EXPECT_EQ(f.db.getUtreexoMeta("orchard_catalog_v1/node/anything").status(),Status::Invalid);
    ASSERT_EQ(f.db.stageOrchardCatalogNode(f.token,id,bytes,batch),Status::Ok);EXPECT_EQ(f.db.getOrchardCatalogNode(id).status(),Status::NotFound);
    ASSERT_EQ(f.db.writeBatch(f.token,std::move(batch),true),Status::Ok);ASSERT_TRUE(f.db.getOrchardCatalogNode(id).ok());
    const auto read=[&](const uint256& key){auto value=f.db.getOrchardCatalogNode(key);if(!value.ok())throw std::runtime_error("test unavailable");return *value;};
    c::Tree transactions(c::Kind::Transactions,read);EXPECT_TRUE(transactions.Find(id,node.key));
    c::Tree legacy(c::Kind::LegacyCoins,read);EXPECT_THROW(legacy.Find(id,node.key),std::runtime_error);
    auto unknown=id;unknown.data[0]^=1;EXPECT_THROW(transactions.Find(unknown,node.key),std::runtime_error);
    c::Node invalid{c::Kind::LegacyCoins};invalid.value=std::string(41,'\0');EXPECT_THROW(c::Node::Decode(invalid.Encode()),std::runtime_error);f.CheckUnpublished();
}
TEST(OrchardParentCatalog, PersistentTreeMaximumDepthOldRootsAndDuplicateRefusal) {
    namespace c=storage::catalog;OwnedSelectedParentFixture f;std::map<uint256,std::string> pending;rocksdb::WriteBatch batch;
    const auto read=[&](const uint256& id){if(const auto p=pending.find(id);p!=pending.end())return p->second;auto value=f.db.getOrchardCatalogNode(id);if(!value.ok())throw std::runtime_error("missing tree node");return *value;};
    c::Tree tree(c::Kind::Transactions,read,[&](const auto& id,const auto& bytes){ASSERT_EQ(f.db.stageOrchardCatalogNode(f.token,id,bytes,batch),Status::Ok);pending.emplace(id,bytes);});
    uint256 root;std::vector<c::Key> keys(1);root=tree.Insert(root,keys.front(),{});const auto first=root;
    for(unsigned bit=0;bit<256;++bit){c::Key key{};key[bit/8]=uint8_t(1u<<(7-bit%8));root=tree.Insert(root,key,{});keys.push_back(key);}
    EXPECT_THROW(tree.Insert(root,keys.front(),{}),std::runtime_error);ASSERT_EQ(f.db.writeBatch(f.token,std::move(batch),true),Status::Ok);pending.clear();
    for(const auto& key:keys)EXPECT_TRUE(tree.Find(root,key));EXPECT_TRUE(tree.Find(first,keys.front()));EXPECT_FALSE(tree.Find(first,keys.back()));
    c::Key absent{};absent.fill(0xff);EXPECT_FALSE(tree.Find(root,absent));f.CheckUnpublished();
}
} // namespace dinero
#else
TEST(OrchardParentCatalog, UnavailableWithoutBackend) {dinero::ChainstateService service;EXPECT_FALSE(service.PrepareSelectedOrchardParent());}
#endif
