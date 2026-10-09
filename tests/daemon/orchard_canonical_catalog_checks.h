#pragma once
#include <fstream>
#include "daemon/orchard_reindex.h"
#include "storage/orchard_storage_mode.h"
#include "consensus/orchard_catalog_coin_view.h"
#ifdef DINERO_TEST_ORCHARD_ORIGIN
namespace dinero {
namespace {
struct CanonicalCatalogFixture:OwnedSelectedParentFixture {
    using Access=ShieldedStateStartupTestAccess;
    std::unique_ptr<PreparedOrchardParent> prepared;
    storage::LegacyRetirementRecord retirement;
    OrchardBlockCandidate block;
    consensus::OrchardBlockContext context;
    CanonicalCatalogFixture():prepared(service.PrepareSelectedOrchardParent()),
        retirement(prepared?prepared->Record():throw std::runtime_error("catalog fixture owner")),
        block(BoundaryCoinbaseBlock(*this,retirement)),context(*consensus::SelectedOrchardBlockContext(block.Header(),4)) {
        Require(bool(service.CheckPreparedOrchardParent(*prepared)));
        const auto work=ChainworkFromHex(tip.chainwork)+GetBlockProof(block.Header().difficulty);
        Require(db.putHeader(token,block.Header().GetHash(),block.Header(),4,work)==Status::Ok);
    }
    auto Connect(const PreparedOrchardCatalog* initial,bool required=true) {
        auto& live=Access::CatalogWriterCoins(service);const auto forest=live.GetForest();
        return PreparedOrchardChainstateWrite::Connect(Access::CatalogWriterMutex(service),db,token,live,
            context,block,blocks.back().header,forest,{},true,true,retirement,nullptr,initial,required);
    }
    auto Disconnect() {
        auto& live=Access::CatalogWriterCoins(service);const auto forest=live.GetForest();
        return PreparedOrchardChainstateWrite::Disconnect(Access::CatalogWriterMutex(service),db,token,live,
            context,block,blocks.back().header,forest,true,true);
    }
};
}
TEST(OrchardCanonicalCatalog, AtomicEnrollmentAbandonDisconnectReopenReconnect) {
    namespace c=storage::catalog;CanonicalCatalogFixture f;
    const auto& catalog=f.prepared->Catalog();
    EXPECT_THROW(f.Connect(nullptr),std::exception);f.CheckUnpublished();
    {auto abandoned=f.Connect(&catalog);EXPECT_EQ(f.db.getOrchardCatalogState(f.tip.hash).status(),Status::NotFound);
        EXPECT_EQ(f.db.getOrchardCatalogState(f.context.block_hash).status(),Status::NotFound);}
    f.CheckUnpublished();
    const auto tip=f.db.getTip();ASSERT_TRUE(tip.ok());
    {
        auto stale=f.Connect(&catalog);auto wrong=tip->hash;wrong.data[0]^=1;
        ASSERT_EQ(f.db.setTip(f.token,wrong,tip->height,tip->work),Status::Ok);
        ASSERT_THROW(stale->Commit(),consensus::OrchardStateLookupError);
        EXPECT_THROW(stale->Commit(),std::logic_error);
        EXPECT_EQ(f.db.getOrchardCatalogState(f.tip.hash).status(),Status::NotFound);
        EXPECT_EQ(f.db.getOrchardCatalogState(f.context.block_hash).status(),Status::NotFound);
        EXPECT_EQ(ShieldedStateStartupTestAccess::CatalogWriterCoins(f.service).GetBestBlock(),f.tip.hash);
    }
    ASSERT_EQ(f.db.setTip(f.token,tip->hash,tip->height,tip->work),Status::Ok);f.CheckUnpublished();
    {auto write=f.Connect(&catalog);write->Commit();}
    const auto parent=f.db.getOrchardCatalogState(f.tip.hash),child=f.db.getOrchardCatalogState(f.context.block_hash);
    ASSERT_TRUE(parent.ok());ASSERT_TRUE(child.ok());const auto before=c::State::Decode(*parent),after=c::State::Decode(*child);
    EXPECT_EQ(after.previous_record,c::Hash(*parent));EXPECT_EQ(before.transaction_count,catalog.TransactionCount());
    EXPECT_EQ(after.transaction_count,before.transaction_count+1);
    // All four outputs are actual canonical coins, including the witness,
    // filter and state commitments. Height four precedes leaf cutoff twenty.
    const auto& coinbase=f.block.Transactions().front().Historical();
    ASSERT_EQ(coinbase.vout.size(),4u);
    EXPECT_EQ(after.legacy_count,before.legacy_count+coinbase.vout.size());
    EXPECT_TRUE(before.nontransparent.IsNull());EXPECT_EQ(before.nontransparent_count,0u);
    EXPECT_EQ(after.nontransparent,before.nontransparent);EXPECT_EQ(after.nontransparent_count,before.nontransparent_count);
    const auto read=[&](const uint256& id){auto value=f.db.getOrchardCatalogNode(id);if(!value.ok())throw std::runtime_error("missing canonical node");return *value;};
    c::Tree ids(c::Kind::Transactions,read),coins(c::Kind::LegacyCoins,read);
    for(const auto& b:f.blocks)for(const auto& tx:b.vtx)EXPECT_TRUE(ids.Find(after.transactions,c::TransactionKey(tx.GetTxid())));
    for(uint32_t n=0;n<coinbase.vout.size();++n) {
        const auto point=c::OutpointBytes(OutPoint(coinbase.GetTxid(),n));
        const auto coin=coins.Find(after.legacy,c::LegacyKey(point));ASSERT_TRUE(coin);
        ASSERT_EQ(coin->size(),41u);EXPECT_EQ(coin->substr(0,36),point);
        EXPECT_EQ(c::Number(*coin,36,4),4u);EXPECT_EQ(uint8_t((*coin)[40]),1u);
        EXPECT_FALSE(coins.Find(before.legacy,c::LegacyKey(point)));
        const auto durable=f.db.getCoin(coinbase.GetTxid().AsUint256(),n);ASSERT_TRUE(durable.ok());
        EXPECT_EQ(durable->height,4);EXPECT_TRUE(durable->coinbase);
        EXPECT_EQ(durable->amount,coinbase.vout[n].value.GetUna());
        std::string script;constexpr char hex[]="0123456789abcdef";
        for(const auto byte:coinbase.vout[n].scriptPubKey){script.push_back(hex[byte>>4]);script.push_back(hex[byte&15]);}
        EXPECT_EQ(durable->script_pubkey,script);
    }
    {auto undo=f.Disconnect();undo->Commit();}f.CheckUnpublished();
    EXPECT_EQ(*f.db.getOrchardCatalogState(f.context.block_hash),*child);
    f.prepared.reset();f.db.close();ASSERT_EQ(f.db.init(f.path),Status::Ok);
    {auto retry=f.Connect(nullptr);retry->Commit();}
    EXPECT_EQ(*f.db.getOrchardCatalogState(f.tip.hash),*parent);EXPECT_EQ(*f.db.getOrchardCatalogState(f.context.block_hash),*child);
    {auto undo=f.Disconnect();undo->Commit();}f.CheckUnpublished();
}
namespace {
struct CompactCatalogWriterFixture:OwnedSelectedParentFixture {
    using Access=ShieldedStateStartupTestAccess;
    std::unique_ptr<PreparedOrchardParent> prepared;
    storage::LegacyRetirementRecord retirement;
    OrchardBlockCandidate block;
    consensus::OrchardBlockContext context;
    BlockStorage files;
    CBlockIndex index;
    OrchardCompactChainstate compact;
    CompactCatalogWriterFixture():OwnedSelectedParentFixture(21),prepared(service.PrepareSelectedOrchardParent()),
        retirement(prepared?prepared->Record():throw std::runtime_error("compact fixture parent")),
        block(BoundaryCoinbaseBlock(*this,retirement)),context(*consensus::SelectedOrchardBlockContext(block.Header(),22)),
        index(block.Header(),context.height) {
        const auto work=ChainworkFromHex(tip.chainwork)+GetBlockProof(block.Header().difficulty);
        Require(db.putHeader(token,context.block_hash,block.Header(),context.height,work)==Status::Ok);
        index.chainwork=work.GetHex();
        Require(db.updateBlockIndex(token,&index)==Status::Ok);
        Require(files.init(path/"compact-files")==Status::Ok);
        const auto metadata=db.getHeaderMetadata(context.block_hash);Require(metadata.ok());
        index.chainwork=metadata->chainwork.GetHex();index.status=metadata->status_flags;
        Require(metadata->data_size==0&&metadata->undo_size==0);
    }
    auto Write(const PreparedOrchardCatalog* initial) {
        return PreparedOrchardChainstateWrite::ConnectCompactIndexed(Access::CatalogWriterMutex(service),
            db,token,files,index,compact,context,block,blocks.back().header,{},true,
            retirement,false,initial);
    }
    auto Undo() {
        return PreparedOrchardChainstateWrite::DisconnectCompactIndexed(Access::CatalogWriterMutex(service),
            db,token,files,index,compact,context,block,blocks.back().header,{},true);
    }
    auto Reconnect(const std::optional<storage::LegacyRetirementRecord>& record=std::nullopt) {
        return PreparedOrchardChainstateWrite::ConnectCompactIndexed(Access::CatalogWriterMutex(service),
            db,token,files,index,compact,context,block,blocks.back().header,{},true,record,false,nullptr);
    }
    std::optional<std::string> Live() {
        auto& mutex=Access::CatalogWriterMutex(service);std::lock_guard<AnnotatedRecursiveMutex> held(mutex);
        const auto* selected=compact.SelectedUnderLock(mutex);
        return selected?std::optional<std::string>(selected->Encode()):std::nullopt;
    }
};
}
TEST(OrchardCanonicalCatalog, CompactStorageModeSharesCommitAndSurvivesBoundaryUndo) {
    CompactCatalogWriterFixture f;
    EXPECT_FALSE(storage::ReadOrchardCompactStorageBinding(f.db));
    {auto abandoned=f.Write(&f.prepared->Catalog());EXPECT_FALSE(storage::ReadOrchardCompactStorageBinding(f.db));}
    EXPECT_FALSE(storage::ReadOrchardCompactStorageBinding(f.db));
    {auto write=f.Write(&f.prepared->Catalog());write->Commit();}
    const auto binding=storage::ReadOrchardCompactStorageBinding(f.db);ASSERT_TRUE(binding);
    EXPECT_EQ(*binding,storage::EncodeOrchardCompactStorageBinding(storage::catalog::State::Decode(*f.Live())));
    EXPECT_THROW(storage::RequireFullOrchardStorage(f.db),std::exception);
    ChainDB candidate;const auto candidate_path=f.path/"mode-reindex-candidate";
    ASSERT_EQ(candidate.init(candidate_path),Status::Ok);
    consensus::BlockReindexer::Config config;config.mode=consensus::BlockReindexer::Mode::FULL;config.use_assumevalid=false;
    const auto refused=OrchardReindexOwner::Run(f.db,candidate,f.token,f.files,
        f.path/"compact-files",candidate_path,config);
    ASSERT_TRUE(refused.ok());EXPECT_FALSE(refused->success);
    EXPECT_EQ(refused->error,"Orchard compact storage requires authenticated compact startup");
    EXPECT_EQ(storage::ReadOrchardCompactStorageBinding(f.db),binding);
    EXPECT_EQ(f.db.getTip()->hash,f.context.block_hash);
    auto& full=ShieldedStateStartupTestAccess::CatalogWriterCoins(f.service);
    try {
        auto wrong=PreparedOrchardChainstateWrite::Connect(
            ShieldedStateStartupTestAccess::CatalogWriterMutex(f.service),f.db,f.token,
            full,f.context,f.block,f.blocks.back().header,full.GetForest(),{},true,false,f.retirement);
        FAIL()<<"full writer accepted compact storage";
    } catch(const std::exception& error) {
        EXPECT_STREQ(error.what(),"Orchard compact storage requires authenticated compact startup");
    }
    {auto undo=f.Undo();undo->Commit();}
    f.CheckUnpublished();EXPECT_EQ(storage::ReadOrchardCompactStorageBinding(f.db),binding);
    EXPECT_THROW(storage::RequireFullOrchardStorage(f.db),std::exception);
    {auto reconnect=f.Reconnect();reconnect->Commit();}
    EXPECT_EQ(storage::ReadOrchardCompactStorageBinding(f.db),binding);
}
TEST(OrchardCanonicalCatalog, CompactStorageModeMutationRefusesBeforePublication) {
    CompactCatalogWriterFixture f;
    const auto put=[&](const std::optional<std::string>& bytes) {
        rocksdb::WriteBatch batch;
        if(bytes)ASSERT_TRUE(batch.Put(storage::OrchardCompactStorageKey,*bytes).ok());
        else ASSERT_TRUE(batch.Delete(storage::OrchardCompactStorageKey).ok());
        ASSERT_EQ(f.db.writeBatch(f.token,std::move(batch),true),Status::Ok);
    };
    {auto write=f.Write(&f.prepared->Catalog());put("invalid");
        EXPECT_THROW(write->Commit(),std::exception);EXPECT_FALSE(f.Live());f.CheckUnpublished();}
    EXPECT_EQ(storage::ReadOrchardCompactStorageBinding(f.db),std::optional<std::string>("invalid"));
    EXPECT_THROW(f.Write(&f.prepared->Catalog()),std::exception);put(std::nullopt);
    {auto write=f.Write(&f.prepared->Catalog());write->Commit();}
    const auto binding=storage::ReadOrchardCompactStorageBinding(f.db),child=f.Live();ASSERT_TRUE(binding);ASSERT_TRUE(child);
    for(const auto& bad:{std::optional<std::string>{},std::optional<std::string>{"invalid"}}) {
        {auto undo=f.Undo();put(bad);EXPECT_THROW(undo->Commit(),std::exception);}
        EXPECT_THROW(f.Undo(),std::exception);EXPECT_EQ(f.Live(),child);
        EXPECT_EQ(f.db.getTip()->hash,f.context.block_hash);put(binding);
    }
    {auto undo=f.Undo();undo->Commit();}f.CheckUnpublished();
    EXPECT_EQ(storage::ReadOrchardCompactStorageBinding(f.db),binding);
}
TEST(OrchardCanonicalCatalog, CompactWriterEnrollsAtomicallyWithoutFullCoinStorage) {
    CompactCatalogWriterFixture f;
    const auto old_forest=ShieldedStateStartupTestAccess::CatalogWriterCoins(f.service).GetForest().serialize();
    // Independent full-node oracle, abandoned before persistence. The compact
    // writer below receives no full forest or full coin interface.
    rocksdb::WriteBatch oracle_batch;
    const auto oracle=consensus::StageOrchardChainstateConnectUnderLock(f.db,f.token,f.context,f.block,
        f.blocks.back().header,*f.replay->Forest(),{},true,false,oracle_batch,f.retirement);
    for(const auto& [point,coin]:f.replay->ProvenUtxos())
        ASSERT_EQ(f.db.deleteCoin(f.token,point.txid.AsUint256(),point.vout),Status::Ok);
    EXPECT_FALSE(f.Live());EXPECT_THROW(f.Write(nullptr),std::exception);
    {auto abandoned=f.Write(&f.prepared->Catalog());EXPECT_FALSE(f.Live());}
    EXPECT_FALSE(f.Live());f.CheckUnpublished();
    EXPECT_EQ(f.db.getOrchardCatalogState(f.context.block_hash).status(),Status::NotFound);
    {auto write=f.Write(&f.prepared->Catalog());write->Commit();}
    const auto durable=f.db.getOrchardCatalogState(f.context.block_hash);ASSERT_TRUE(durable.ok());
    ASSERT_TRUE(f.Live());EXPECT_EQ(*f.Live(),*durable);
    const auto state=storage::catalog::State::Decode(*durable);
    EXPECT_EQ(state.stump,consensus::UtreexoStump::fromForest(oracle.forest.After()).serialize());
    EXPECT_EQ(f.db.getUndo(f.context.block_hash)->Serialize(),oracle.block.undo.Serialize());
    EXPECT_EQ(f.db.getOrchardState()->block_hash,oracle.block.orchard.Next().block_hash);
    EXPECT_EQ(f.db.getTip()->hash,f.context.block_hash);
    EXPECT_EQ(f.db.getValidatedTip()->hash,f.context.block_hash);
    EXPECT_EQ(f.db.getForestTipMarker()->forest_root,f.block.Header().utreexo_root);
    EXPECT_EQ(f.db.getLegacyRetirementState()->block_hash,f.context.block_hash);
    EXPECT_TRUE(f.index.status&BLOCK_HAVE_DATA);EXPECT_TRUE(f.index.status&BLOCK_HAVE_UNDO);
    const auto actual=f.files.readBlockBytes({f.index.file_number,f.index.data_pos,f.index.data_size});ASSERT_TRUE(actual.ok());
    EXPECT_EQ(*actual,std::string(f.block.WireBytes().begin(),f.block.WireBytes().end()));
    for(const auto& tx:oracle.block.coins.Transactions())for(const auto& [point,coin]:tx.created)
        EXPECT_EQ(f.db.getCoin(point.txid.AsUint256(),point.vout).status(),Status::NotFound);
    EXPECT_EQ(ShieldedStateStartupTestAccess::CatalogWriterCoins(f.service).GetForest().serialize(),old_forest);
    // This is the canonical writer component, not configured CSN service
    // publication. The unrelated full-node service remains at its old tip.
    EXPECT_EQ(ShieldedStateStartupTestAccess::CatalogWriterCoins(f.service).GetBestBlock(),f.tip.hash);
    EXPECT_THROW(f.Write(&f.prepared->Catalog()),std::exception);
}
TEST(OrchardCanonicalCatalog, CompactWriterLateRefusalLeavesLiveOwnerUnenrolled) {
    CompactCatalogWriterFixture f;
    const auto tip=f.db.getTip();ASSERT_TRUE(tip.ok());
    const auto prior_status=f.index.status;
    {
        auto write=f.Write(&f.prepared->Catalog());auto wrong=tip->hash;wrong.data[0]^=1;
        ASSERT_EQ(f.db.setTip(f.token,wrong,tip->height,tip->work),Status::Ok);
        EXPECT_THROW(write->Commit(),consensus::OrchardStateLookupError);
        EXPECT_THROW(write->Commit(),std::logic_error);
        EXPECT_FALSE(f.Live());EXPECT_EQ(f.index.status,prior_status);
        EXPECT_EQ(f.db.getOrchardCatalogState(f.context.block_hash).status(),Status::NotFound);
        EXPECT_EQ(f.db.getOrchardState().status(),Status::NotFound);
        EXPECT_EQ(f.db.getUndo(f.context.block_hash).status(),Status::NotFound);
    }
    ASSERT_EQ(f.db.setTip(f.token,tip->hash,tip->height,tip->work),Status::Ok);
    {auto retry=f.Write(&f.prepared->Catalog());retry->Commit();}
    ASSERT_TRUE(f.Live());EXPECT_EQ(*f.Live(),*f.db.getOrchardCatalogState(f.context.block_hash));
}
TEST(OrchardCanonicalCatalog, CompactUndoBoundaryAbandonCommitAndReconnect) {
    CompactCatalogWriterFixture f;
    for(const auto& [point,coin]:f.replay->ProvenUtxos())
        ASSERT_EQ(f.db.deleteCoin(f.token,point.txid.AsUint256(),point.vout),Status::Ok);
    {auto write=f.Write(&f.prepared->Catalog());write->Commit();}
    const auto child=f.db.getOrchardCatalogState(f.context.block_hash);
    const auto parent=f.db.getOrchardCatalogState(f.context.parent_hash);
    ASSERT_TRUE(child.ok());ASSERT_TRUE(parent.ok());
    const auto undo=f.db.getUndo(f.context.block_hash);ASSERT_TRUE(undo.ok());
    const auto index_status=f.index.status;
    {auto abandoned=f.Undo();ASSERT_TRUE(f.Live());EXPECT_EQ(*f.Live(),*child);}
    EXPECT_EQ(f.db.getTip()->hash,f.context.block_hash);EXPECT_EQ(f.index.status,index_status);
    {auto rollback=f.Undo();rollback->Commit();}
    f.CheckUnpublished();ASSERT_TRUE(f.Live());EXPECT_EQ(*f.Live(),*parent);
    EXPECT_EQ(f.db.getValidatedTip()->hash,f.context.parent_hash);
    EXPECT_EQ(f.db.getForestTipMarker()->forest_root,f.blocks.back().header.utreexo_root);
    EXPECT_EQ(*f.db.getOrchardCatalogState(f.context.block_hash),*child);
    EXPECT_EQ(f.db.getUndo(f.context.block_hash)->Serialize(),undo->Serialize());
    EXPECT_THROW(f.Undo(),std::exception);
    // Reconnection must retain the genuinely authenticated retirement owner
    // across the boundary undo; no caller record is needed or regenerated.
    f.prepared.reset();
    auto foreign=f.retirement;foreign.boundary_parent.data[0]^=1;
    EXPECT_THROW(f.Reconnect(foreign),std::exception);
    f.CheckUnpublished();EXPECT_EQ(*f.Live(),*parent);
    {auto reconnect=f.Reconnect();reconnect->Commit();}
    EXPECT_EQ(*f.Live(),*child);EXPECT_EQ(f.db.getTip()->hash,f.context.block_hash);
    EXPECT_EQ(f.db.getLegacyRetirementState()->record,f.retirement);
    EXPECT_EQ(f.db.getUndo(f.context.block_hash)->Serialize(),undo->Serialize());
    for(const auto& [point,coin]:f.replay->ProvenUtxos())
        EXPECT_EQ(f.db.getCoin(point.txid.AsUint256(),point.vout).status(),Status::NotFound);
    for(const auto& tx:f.block.Transactions())for(uint32_t n=0;n<tx.Historical().vout.size();++n)
        EXPECT_EQ(f.db.getCoin(tx.GetTxid().AsUint256(),n).status(),Status::NotFound);
}
TEST(OrchardCanonicalCatalog, CompactUndoLateRefusalPreservesSelectedOwner) {
    CompactCatalogWriterFixture f;
    {auto write=f.Write(&f.prepared->Catalog());write->Commit();}
    const auto child=f.db.getOrchardCatalogState(f.context.block_hash);ASSERT_TRUE(child.ok());
    const auto tip=f.db.getTip();ASSERT_TRUE(tip.ok());const auto index_status=f.index.status;
    {
        auto rollback=f.Undo();auto wrong=tip->hash;wrong.data[0]^=1;
        ASSERT_EQ(f.db.setTip(f.token,wrong,tip->height,tip->work),Status::Ok);
        EXPECT_THROW(rollback->Commit(),consensus::OrchardStateLookupError);
        EXPECT_THROW(rollback->Commit(),std::logic_error);
        ASSERT_TRUE(f.Live());EXPECT_EQ(*f.Live(),*child);EXPECT_EQ(f.index.status,index_status);
        EXPECT_EQ(f.db.getOrchardState()->block_hash,f.context.block_hash);
        EXPECT_EQ(f.db.getLegacyRetirementState()->block_hash,f.context.block_hash);
        EXPECT_EQ(*f.db.getOrchardCatalogState(f.context.block_hash),*child);
    }
    ASSERT_EQ(f.db.setTip(f.token,tip->hash,tip->height,tip->work),Status::Ok);
    {auto retry=f.Undo();retry->Commit();}
    f.CheckUnpublished();ASSERT_TRUE(f.Live());
    EXPECT_EQ(*f.Live(),*f.db.getOrchardCatalogState(f.context.parent_hash));
}
TEST(OrchardCanonicalCatalog, CompactUndoRefusesCorruptPredecessorAndMissingChildNode) {
    namespace c=storage::catalog;CompactCatalogWriterFixture f;
    {auto write=f.Write(&f.prepared->Catalog());write->Commit();}
    const auto child=f.db.getOrchardCatalogState(f.context.block_hash);
    const auto parent=f.db.getOrchardCatalogState(f.context.parent_hash);
    ASSERT_TRUE(child.ok());ASSERT_TRUE(parent.ok());
    const auto state=c::State::Decode(*child);
    ASSERT_NE(state.transactions,c::State::Decode(*parent).transactions);
    const auto node=f.db.getOrchardCatalogNode(state.transactions);ASSERT_TRUE(node.ok());
    auto families=shielded_store_fixture::legacy;families.push_back(shielded_store_fixture::shielded);
    const auto key=[](const char* kind,const uint256& id) {
        return std::string("Morchard_catalog_v1/")+kind+"/"+std::string(reinterpret_cast<const char*>(id.data),32);
    };
    const auto parent_key=key("state",f.context.parent_hash),node_key=key("node",state.transactions);
    f.db.close();
    {shielded_store_fixture::Raw raw(f.path,families);raw.put("utreexo",parent_key,"corrupt");}
    ASSERT_EQ(f.db.init(f.path),Status::Ok);
    EXPECT_THROW(f.Undo(),std::exception);ASSERT_TRUE(f.Live());EXPECT_EQ(*f.Live(),*child);
    EXPECT_EQ(f.db.getTip()->hash,f.context.block_hash);
    f.db.close();
    {shielded_store_fixture::Raw raw(f.path,families);raw.put("utreexo",parent_key,*parent);
        rocksdb::WriteOptions options;options.sync=true;
        ASSERT_TRUE(raw.db->Delete(options,raw.cf("utreexo"),node_key).ok());}
    ASSERT_EQ(f.db.init(f.path),Status::Ok);
    EXPECT_THROW(f.Undo(),std::exception);EXPECT_EQ(*f.Live(),*child);
    EXPECT_EQ(f.db.getTip()->hash,f.context.block_hash);
    EXPECT_EQ(f.db.getOrchardCatalogNode(state.transactions).status(),Status::NotFound);
    // Missing immutable nodes must be refused, never reconstructed into the
    // database by disconnect preparation. Restore exactly the original bytes.
    f.db.close();{shielded_store_fixture::Raw raw(f.path,families);raw.put("utreexo",node_key,*node);}
    ASSERT_EQ(f.db.init(f.path),Status::Ok);
    {auto rollback=f.Undo();rollback->Commit();}f.CheckUnpublished();
    EXPECT_EQ(*f.Live(),*parent);
    // The live owner is deliberately retained here. This does not test fresh
    // process startup or authorize enrollment from decoded database records.
}
TEST(OrchardCanonicalCatalog, CompactUndoRequiresExactPersistedConventionalUndo) {
    CompactCatalogWriterFixture f;
    for(const auto& [point,coin]:f.replay->ProvenUtxos())
        ASSERT_EQ(f.db.deleteCoin(f.token,point.txid.AsUint256(),point.vout),Status::Ok);
    {auto write=f.Write(&f.prepared->Catalog());write->Commit();}
    const auto child=f.db.getOrchardCatalogState(f.context.block_hash);ASSERT_TRUE(child.ok());
    const auto original=f.db.getUndo(f.context.block_hash);ASSERT_TRUE(original.ok());
    ASSERT_FALSE(original->created.empty());auto changed=*original;changed.created.pop_back();
    const auto index_status=f.index.status;
    ASSERT_EQ(f.db.putUndo(f.token,f.context.block_hash,changed),Status::Ok);
    EXPECT_THROW(f.Undo(),std::exception);ASSERT_TRUE(f.Live());EXPECT_EQ(*f.Live(),*child);
    EXPECT_EQ(f.db.getTip()->hash,f.context.block_hash);EXPECT_EQ(f.index.status,index_status);
    EXPECT_EQ(f.db.getOrchardState()->block_hash,f.context.block_hash);
    EXPECT_EQ(f.db.getLegacyRetirementState()->block_hash,f.context.block_hash);
    EXPECT_EQ(f.db.getUndo(f.context.block_hash)->Serialize(),changed.Serialize());
    // Restore the exact retained bytes; rejection must not normalize the undo
    // or replace any catalog, tip, live owner, or full-coin rows.
    ASSERT_EQ(f.db.putUndo(f.token,f.context.block_hash,*original),Status::Ok);
    {auto rollback=f.Undo();rollback->Commit();}f.CheckUnpublished();
    EXPECT_EQ(*f.Live(),*f.db.getOrchardCatalogState(f.context.parent_hash));
    EXPECT_EQ(f.db.getUndo(f.context.block_hash)->Serialize(),original->Serialize());
    for(const auto& [point,coin]:f.replay->ProvenUtxos())
        EXPECT_EQ(f.db.getCoin(point.txid.AsUint256(),point.vout).status(),Status::NotFound);
}
TEST(OrchardCanonicalCatalog, CorruptRecordAndMissingOwnerRefuseWithoutEffects) {
    CanonicalCatalogFixture f;{auto write=f.Connect(&f.prepared->Catalog());write->Commit();}
    const auto original=f.db.getOrchardCatalogState(f.context.block_hash);ASSERT_TRUE(original.ok());
    f.prepared.reset();f.db.close();auto families=shielded_store_fixture::legacy;families.push_back(shielded_store_fixture::shielded);
    const auto key=std::string("Morchard_catalog_v1/state/")+std::string(reinterpret_cast<const char*>(f.context.block_hash.data),32);
    {shielded_store_fixture::Raw raw(f.path,families);auto bad=*original;bad.back()^=1;raw.put("utreexo",key,bad);}
    ASSERT_EQ(f.db.init(f.path),Status::Ok);EXPECT_EQ(f.db.getOrchardCatalogState(f.context.block_hash).status(),Status::Corruption);
    EXPECT_THROW(f.Disconnect(),std::exception);ASSERT_TRUE(f.db.getTip().ok());EXPECT_EQ(f.db.getTip()->hash,f.context.block_hash);
    EXPECT_EQ(ShieldedStateStartupTestAccess::CatalogWriterCoins(f.service).GetBestBlock(),f.context.block_hash);
    f.db.close();{shielded_store_fixture::Raw raw(f.path,families);raw.put("utreexo",key,*original);}
    ASSERT_EQ(f.db.init(f.path),Status::Ok);{auto undo=f.Disconnect();undo->Commit();}f.CheckUnpublished();
}
TEST(OrchardCanonicalCatalog, PersistentRemovalKeepsOldRootsAndRejectsMissingAndCorruptSibling) {
    namespace c=storage::catalog;std::map<uint256,std::string> nodes;unsigned writes=0;
    c::Tree tree(c::Kind::Transactions,[&](const auto& id){return nodes.at(id);},[&](const auto& id,const auto& value){++writes;nodes.emplace(id,value);});
    std::vector<c::Key> keys(1);uint256 root=tree.Insert({},keys.front(),{});
    for(unsigned bit=0;bit<256;++bit){c::Key key{};key[bit/8]=uint8_t(1u<<(7-bit%8));keys.push_back(key);root=tree.Insert(root,key,{});}
    const auto original=root;const auto removed=tree.Erase(root,keys.front());EXPECT_FALSE(tree.Find(removed,keys.front()));EXPECT_TRUE(tree.Find(original,keys.front()));
    const auto before=writes;EXPECT_THROW(tree.Erase(removed,keys.front()),std::exception);EXPECT_EQ(writes,before);
    root=removed;for(size_t i=1;i<keys.size();++i)root=tree.Erase(root,keys[i]);EXPECT_TRUE(root.IsNull());
    for(const auto& key:keys)EXPECT_TRUE(tree.Find(original,key));
    c::Key one{};one[0]=0x80;auto pair=tree.Insert(tree.Insert({},keys.front(),{}),one,{});
    const auto node=c::Node::Decode(nodes.at(pair));const auto intact=nodes.at(node.one);nodes[node.one]="bad";
    const auto count=writes;EXPECT_THROW(tree.Erase(pair,keys.front()),std::exception);EXPECT_EQ(writes,count);nodes[node.one]=intact;
    EXPECT_TRUE(tree.Find(tree.Erase(pair,keys.front()),one));
}
TEST(OrchardCanonicalCatalog, StateCodecRequiresCompleteCanonicalVerificationStump) {
    namespace c=storage::catalog;CanonicalCatalogFixture f;{auto write=f.Connect(&f.prepared->Catalog());write->Commit();}
    const auto bytes=f.db.getOrchardCatalogState(f.context.block_hash);ASSERT_TRUE(bytes.ok());auto state=c::State::Decode(*bytes);
    EXPECT_EQ(state.Encode(),*bytes);EXPECT_THROW(c::State::Decode(*bytes+"extra"),std::exception);
    EXPECT_EQ(bytes->substr(0,7),"DNOCS02");
    // A syntactically intact v1 record lacks complete metadata membership.
    // No empty-set default or implicit migration is permitted.
    auto old=*bytes;old.replace(0,7,"DNOCS01");EXPECT_THROW(c::State::Decode(old),std::exception);
    state.nontransparent_count=1;EXPECT_THROW(state.Encode(),std::exception);state=c::State::Decode(*bytes);
    state.nontransparent.data[0]=1;EXPECT_THROW(state.Encode(),std::exception);state=c::State::Decode(*bytes);
    auto changed=*bytes;changed[20]^=1;EXPECT_THROW(c::State::Decode(changed),std::exception);
    state.stump.push_back(0);EXPECT_THROW(state.Encode(),std::exception);state=c::State::Decode(*bytes);
    state.stump[8]=0;EXPECT_THROW(state.Encode(),std::exception);state=c::State::Decode(*bytes);
    state.previous_record={};EXPECT_THROW(state.Encode(),std::exception);
}
namespace {
OrchardBlockCandidate CatalogProofReplacement(const OrchardBlockCandidate& block,
    const consensus::BlockUtreexoData& proof) {
    OrchardAdmissionFixture::Require(block.Utreexo().has_value());
    const auto original=block.Utreexo()->serialize();auto wire=block.WireBytes();
    OrchardAdmissionFixture::Require(wire.size()>original.size()&&
        std::equal(original.rbegin(),original.rend(),wire.rbegin()));
    wire.resize(wire.size()-original.size());const auto replacement=proof.serialize();
    wire.insert(wire.end(),replacement.begin(),replacement.end());
    return OrchardBlockCandidate::DecodeExact(wire);
}
}
TEST(OrchardCanonicalCatalog, CatalogProofCaptureLegacyModernAndSameBlock) {
    ProofHandoffFixture f;
    const auto read=[&](const uint256& id) {
        const auto value=f.f.db.getOrchardCatalogNode(id);
        if(!value.ok())throw consensus::OrchardStateLookupError(value.status());return *value;
    };
    for(const auto& source:{f.first,f.second}) {
        const auto block=OrchardBlockCandidate::DecodeExact(source->WireBytes());
        const auto context=consensus::SelectedOrchardBlockContext(block.Header(),source->Height());ASSERT_TRUE(context);
        const auto header=f.f.db.getHeader(context->parent_hash);ASSERT_TRUE(header.ok());
        const auto raw=f.f.db.getOrchardCatalogState(context->parent_hash);ASSERT_TRUE(raw.ok());
        const auto state=storage::catalog::State::Decode(*raw);
        const auto before=f.Rows();
        const auto captured=consensus::CaptureOrchardCatalogCoins(block,*context,*header,state,read);
        EXPECT_EQ(captured.ParentHash(),context->parent_hash);EXPECT_EQ(captured.BlockHash(),context->block_hash);
        EXPECT_EQ(captured.CapturedInputs(),1u);
        EXPECT_EQ(f.Rows(),before);
        ASSERT_FALSE(block.Utreexo()->spent_outputs.empty());
        if(source==f.first)EXPECT_LT(block.Utreexo()->spent_outputs.front().created_height,state.leaf_activation);
        else {EXPECT_GE(block.Utreexo()->spent_outputs.front().created_height,state.leaf_activation);
            EXPECT_EQ(block.Utreexo()->spent_outputs.size(),2u);}
        auto bad=*block.Utreexo();bad.spent_outputs.front().is_coinbase=!bad.spent_outputs.front().is_coinbase;
        EXPECT_THROW(consensus::CaptureOrchardCatalogCoins(CatalogProofReplacement(block,bad),*context,*header,state,read),std::exception);
        bad=*block.Utreexo();++bad.spent_outputs.front().created_height;
        EXPECT_THROW(consensus::CaptureOrchardCatalogCoins(CatalogProofReplacement(block,bad),*context,*header,state,read),std::exception);
        bad=*block.Utreexo();bad.spend_proof.proof_hashes.push_back(consensus::UtreexoHash(32,0));
        EXPECT_THROW(consensus::CaptureOrchardCatalogCoins(CatalogProofReplacement(block,bad),*context,*header,state,read),std::exception);
        EXPECT_THROW(consensus::CaptureOrchardCatalogCoins(block,*context,*header,state,
            [](const uint256&)->std::string{throw std::runtime_error("denied catalog read");}),std::exception);
        EXPECT_EQ(f.Rows(),before);
    }
    // The actual service consumes the same adapter, retains full-store
    // before-image checks, and connects the valid parent/child block normally.
    const auto accepted=f.Submit(f.second->WireBytes());ASSERT_TRUE(accepted.connected)<<accepted.reason;
    EXPECT_EQ(f.f.service->GetActiveTip()->hash,f.second->Header().GetHash());
}
TEST(OrchardCanonicalCatalog, CompactWriterMatchesLegacyModernAndSameBlockFullHistory) {
    namespace c=storage::catalog;ProofHandoffFixture f;
    ASSERT_TRUE(f.Submit(f.second->WireBytes()).connected);
    const auto first_raw=f.f.db.getOrchardCatalogState(f.first->Header().GetHash());
    const auto second_raw=f.f.db.getOrchardCatalogState(f.second->Header().GetHash());
    ASSERT_TRUE(first_raw.ok());ASSERT_TRUE(second_raw.ok());
    const auto first_undo=f.f.db.getUndo(f.first->Header().GetHash());
    const auto second_undo=f.f.db.getUndo(f.second->Header().GetHash());
    ASSERT_TRUE(first_undo.ok());ASSERT_TRUE(second_undo.ok());
    // Actual canonical full writer undoes both transitions. The fixture stops
    // using service selection thereafter: the component under test has its
    // own compact owner, and production CSN service routing remains guarded.
    for(const auto& source:{f.second,f.first}) {
        const auto block=OrchardBlockCandidate::DecodeExact(source->WireBytes());
        const auto context=*consensus::SelectedOrchardBlockContext(block.Header(),source->Height());
        const auto parent=*f.f.db.getHeader(context.parent_hash);
        auto* index=dinero::FindBlockIndex(context.block_hash);ASSERT_NE(index,nullptr);
        auto& full=ShieldedStateStartupTestAccess::CatalogWriterCoins(*f.f.service);
        const auto forest=full.GetForest();
        auto undo=PreparedOrchardChainstateWrite::DisconnectIndexed(
            ShieldedStateStartupTestAccess::CatalogWriterMutex(*f.f.service),f.f.db,f.f.token,*f.files,
            *index,full,context,block,parent,forest,true,true);
        undo->Commit();
    }
    ASSERT_EQ(f.f.db.getTip()->hash,f.parent->hash);
    OrchardHistoryCapture history(f.parent->height,f.parent->hash);
    CaptureHeaders(history,f.f.blocks);
    for(uint32_t h=0;h<f.f.blocks.size();++h)history.RecordBody(h,f.f.blocks[h]);
    history.Finish();
    const auto initial=OrchardParentCatalogTestAccess::Create(f.f.db,f.f.token,*f.replay,history);
    ASSERT_TRUE(initial);
    for(const auto& [point,coin]:f.replay->ProvenState().ProvenUtxos())
        ASSERT_EQ(f.f.db.deleteCoin(f.f.token,point.txid.AsUint256(),point.vout),Status::Ok);
    OrchardCompactChainstate compact;
    const consensus::OrchardBranchMtpLookup mtp=[&](uint32_t h) {return f.Mtp(h);};
    for(const auto& source:{f.first,f.second}) {
        const auto block=OrchardBlockCandidate::DecodeExact(source->WireBytes());
        const auto context=*consensus::SelectedOrchardBlockContext(block.Header(),source->Height());
        const auto parent=*f.f.db.getHeader(context.parent_hash);
        auto* index=dinero::FindBlockIndex(context.block_hash);ASSERT_NE(index,nullptr);
        const bool boundary=source==f.first;
        auto write=PreparedOrchardChainstateWrite::ConnectCompactIndexed(
            ShieldedStateStartupTestAccess::CatalogWriterMutex(*f.f.service),f.f.db,f.f.token,*f.files,
            *index,compact,context,block,parent,mtp,true,
            boundary?std::optional<storage::LegacyRetirementRecord>(f.replay->Record()):std::nullopt,
            false,boundary?initial.get():nullptr);
        write->Commit();
        const auto actual=f.f.db.getOrchardCatalogState(context.block_hash);ASSERT_TRUE(actual.ok());
        EXPECT_EQ(*actual,boundary?*first_raw:*second_raw);
        EXPECT_EQ(f.f.db.getUndo(context.block_hash)->Serialize(),boundary?first_undo->Serialize():second_undo->Serialize());
        const auto* live=compact.SelectedUnderLock(ShieldedStateStartupTestAccess::CatalogWriterMutex(*f.f.service));
        ASSERT_NE(live,nullptr);EXPECT_EQ(live->Encode(),*actual);
        for(const auto& tx:block.Transactions())for(uint32_t n=0;n<(tx.IsOrchard()?tx.Orchard().Outputs().size():tx.Historical().vout.size());++n)
            EXPECT_EQ(f.f.db.getCoin(tx.GetTxid().AsUint256(),n).status(),Status::NotFound);
    }
    EXPECT_EQ(f.f.db.getTip()->hash,f.second->Header().GetHash());
    EXPECT_EQ(ShieldedStateStartupTestAccess::CatalogWriterCoins(*f.f.service).GetBestBlock(),f.parent->hash);
}
TEST(OrchardCanonicalCatalog, CompactUndoMatchesLegacyModernAndSameBlockFullHistory) {
    namespace c=storage::catalog;ProofHandoffFixture f;
    ASSERT_TRUE(f.Submit(f.second->WireBytes()).connected);
    const auto first_raw=f.f.db.getOrchardCatalogState(f.first->Header().GetHash());
    const auto second_raw=f.f.db.getOrchardCatalogState(f.second->Header().GetHash());
    ASSERT_TRUE(first_raw.ok());ASSERT_TRUE(second_raw.ok());
    const auto first_undo=f.f.db.getUndo(f.first->Header().GetHash());
    const auto second_undo=f.f.db.getUndo(f.second->Header().GetHash());
    ASSERT_TRUE(first_undo.ok());ASSERT_TRUE(second_undo.ok());
    // Actual canonical full writer undoes both transitions. The fixture stops
    // using service selection thereafter: the component under test has its
    // own compact owner, and production CSN service routing remains guarded.
    for(const auto& source:{f.second,f.first}) {
        const auto block=OrchardBlockCandidate::DecodeExact(source->WireBytes());
        const auto context=*consensus::SelectedOrchardBlockContext(block.Header(),source->Height());
        const auto parent=*f.f.db.getHeader(context.parent_hash);
        auto* index=dinero::FindBlockIndex(context.block_hash);ASSERT_NE(index,nullptr);
        auto& full=ShieldedStateStartupTestAccess::CatalogWriterCoins(*f.f.service);
        const auto forest=full.GetForest();
        auto undo=PreparedOrchardChainstateWrite::DisconnectIndexed(
            ShieldedStateStartupTestAccess::CatalogWriterMutex(*f.f.service),f.f.db,f.f.token,*f.files,
            *index,full,context,block,parent,forest,true,true);
        undo->Commit();
    }
    ASSERT_EQ(f.f.db.getTip()->hash,f.parent->hash);
    OrchardHistoryCapture history(f.parent->height,f.parent->hash);
    CaptureHeaders(history,f.f.blocks);
    for(uint32_t h=0;h<f.f.blocks.size();++h)history.RecordBody(h,f.f.blocks[h]);
    history.Finish();
    const auto initial=OrchardParentCatalogTestAccess::Create(f.f.db,f.f.token,*f.replay,history);
    ASSERT_TRUE(initial);
    for(const auto& [point,coin]:f.replay->ProvenState().ProvenUtxos())
        ASSERT_EQ(f.f.db.deleteCoin(f.f.token,point.txid.AsUint256(),point.vout),Status::Ok);
    OrchardCompactChainstate compact;
    const consensus::OrchardBranchMtpLookup mtp=[&](uint32_t h) {return f.Mtp(h);};
    for(const auto& source:{f.first,f.second}) {
        const auto block=OrchardBlockCandidate::DecodeExact(source->WireBytes());
        const auto context=*consensus::SelectedOrchardBlockContext(block.Header(),source->Height());
        const auto parent=*f.f.db.getHeader(context.parent_hash);
        auto* index=dinero::FindBlockIndex(context.block_hash);ASSERT_NE(index,nullptr);
        const bool boundary=source==f.first;
        auto write=PreparedOrchardChainstateWrite::ConnectCompactIndexed(
            ShieldedStateStartupTestAccess::CatalogWriterMutex(*f.f.service),f.f.db,f.f.token,*f.files,
            *index,compact,context,block,parent,mtp,true,
            boundary?std::optional<storage::LegacyRetirementRecord>(f.replay->Record()):std::nullopt,
            false,boundary?initial.get():nullptr);
        write->Commit();
        const auto actual=f.f.db.getOrchardCatalogState(context.block_hash);ASSERT_TRUE(actual.ok());
        EXPECT_EQ(*actual,boundary?*first_raw:*second_raw);
        EXPECT_EQ(f.f.db.getUndo(context.block_hash)->Serialize(),boundary?first_undo->Serialize():second_undo->Serialize());
        const auto* live=compact.SelectedUnderLock(ShieldedStateStartupTestAccess::CatalogWriterMutex(*f.f.service));
        ASSERT_NE(live,nullptr);EXPECT_EQ(live->Encode(),*actual);
        for(const auto& tx:block.Transactions())for(uint32_t n=0;n<(tx.IsOrchard()?tx.Orchard().Outputs().size():tx.Historical().vout.size());++n)
            EXPECT_EQ(f.f.db.getCoin(tx.GetTxid().AsUint256(),n).status(),Status::NotFound);
    }
    EXPECT_EQ(f.f.db.getTip()->hash,f.second->Header().GetHash());
    EXPECT_EQ(ShieldedStateStartupTestAccess::CatalogWriterCoins(*f.f.service).GetBestBlock(),f.parent->hash);
    // Undo descendants and the activation boundary using only the selected
    // compact owner, authenticated retained catalogs and candidate proofs.
    for(const auto& source:{f.second,f.first}) {
        const auto block=OrchardBlockCandidate::DecodeExact(source->WireBytes());
        const auto context=*consensus::SelectedOrchardBlockContext(block.Header(),source->Height());
        const auto parent=*f.f.db.getHeader(context.parent_hash);
        const auto expected=f.f.db.getOrchardCatalogState(context.parent_hash);ASSERT_TRUE(expected.ok());
        auto* index=dinero::FindBlockIndex(context.block_hash);ASSERT_NE(index,nullptr);
        auto rollback=PreparedOrchardChainstateWrite::DisconnectCompactIndexed(
            ShieldedStateStartupTestAccess::CatalogWriterMutex(*f.f.service),f.f.db,f.f.token,*f.files,
            *index,compact,context,block,parent,mtp,true);
        rollback->Commit();
        const auto* live=compact.SelectedUnderLock(ShieldedStateStartupTestAccess::CatalogWriterMutex(*f.f.service));
        ASSERT_NE(live,nullptr);EXPECT_EQ(live->Encode(),*expected);
        EXPECT_EQ(f.f.db.getTip()->hash,context.parent_hash);
        EXPECT_EQ(f.f.db.getValidatedTip()->hash,context.parent_hash);
        EXPECT_EQ(f.f.db.getForestTipMarker()->forest_root,parent.utreexo_root);
    }
    EXPECT_EQ(f.f.db.getLegacyRetirementState().status(),Status::NotFound);
    for(const auto& [point,coin]:f.replay->ProvenState().ProvenUtxos())
        EXPECT_EQ(f.f.db.getCoin(point.txid.AsUint256(),point.vout).status(),Status::NotFound);
    for(const auto& source:{f.first,f.second}) {
        const auto block=OrchardBlockCandidate::DecodeExact(source->WireBytes());
        const auto context=*consensus::SelectedOrchardBlockContext(block.Header(),source->Height());
        const auto parent=*f.f.db.getHeader(context.parent_hash);
        auto* index=dinero::FindBlockIndex(context.block_hash);ASSERT_NE(index,nullptr);
        auto reconnect=PreparedOrchardChainstateWrite::ConnectCompactIndexed(
            ShieldedStateStartupTestAccess::CatalogWriterMutex(*f.f.service),f.f.db,f.f.token,*f.files,
            *index,compact,context,block,parent,mtp,true,std::nullopt,false,nullptr);
        reconnect->Commit();
        const bool boundary=source==f.first;
        EXPECT_EQ(*f.f.db.getOrchardCatalogState(context.block_hash),boundary?*first_raw:*second_raw);
        EXPECT_EQ(f.f.db.getUndo(context.block_hash)->Serialize(),boundary?first_undo->Serialize():second_undo->Serialize());
        const auto* live=compact.SelectedUnderLock(ShieldedStateStartupTestAccess::CatalogWriterMutex(*f.f.service));
        ASSERT_NE(live,nullptr);EXPECT_EQ(live->Encode(),boundary?*first_raw:*second_raw);
        for(const auto& tx:block.Transactions())for(uint32_t n=0;n<(tx.IsOrchard()?tx.Orchard().Outputs().size():tx.Historical().vout.size());++n)
            EXPECT_EQ(f.f.db.getCoin(tx.GetTxid().AsUint256(),n).status(),Status::NotFound);
    }
    EXPECT_EQ(f.f.db.getTip()->hash,f.second->Header().GetHash());
}
TEST(OrchardCanonicalCatalog, CatalogProofCaptureRejectsSpecialAndHistoricalMembership) {
    namespace c=storage::catalog;ProofHandoffFixture f;
    const auto block=OrchardBlockCandidate::DecodeExact(f.second->WireBytes());
    const auto context=*consensus::SelectedOrchardBlockContext(block.Header(),103);
    const auto header=*f.f.db.getHeader(context.parent_hash);
    const auto raw=*f.f.db.getOrchardCatalogState(context.parent_hash);const auto original=c::State::Decode(raw);
    std::map<uint256,std::string> overlay;
    const auto read=[&](const uint256& id) {
        if(const auto i=overlay.find(id);i!=overlay.end())return i->second;
        const auto value=f.f.db.getOrchardCatalogNode(id);
        if(!value.ok())throw consensus::OrchardStateLookupError(value.status());return *value;
    };
    const auto write=[&](const uint256& id,const std::string& bytes){overlay.emplace(id,bytes);};
    const auto before=f.Rows();auto state=original;
    // Typed in-memory catalog mutations test adapter refusal only; these roots
    // are never installed as canonical authority or inserted into the database.
    const OutPoint point(f.first->Transactions().at(1).GetTxid(),0);
    const auto bytes=c::OutpointBytes(point);c::Tree special(c::Kind::NonTransparentCoins,read,write);
    state.nontransparent=special.Insert(state.nontransparent,c::NonTransparentKey(bytes),bytes);
    ++state.nontransparent_count;
    ASSERT_FALSE(block.Utreexo()->spent_outputs.front().is_confidential);
    ASSERT_TRUE(block.Utreexo()->spent_outputs.front().commitment.empty());
    EXPECT_THROW(consensus::CaptureOrchardCatalogCoins(block,context,header,state,read),std::exception);
    state=original;c::Tree transactions(c::Kind::Transactions,read,write);
    state.transactions=transactions.Insert(state.transactions,c::TransactionKey(block.Transactions().at(1).GetTxid()),{});
    ++state.transaction_count;
    EXPECT_THROW(consensus::CaptureOrchardCatalogCoins(block,context,header,state,read),std::exception);
    state=original;state.stump.push_back(0);
    EXPECT_THROW(consensus::CaptureOrchardCatalogCoins(block,context,header,state,read),std::exception);
    EXPECT_NO_THROW(consensus::CaptureOrchardCatalogCoins(block,context,header,original,read));
    EXPECT_EQ(f.Rows(),before);
}
namespace {
// Preserve the actual assembler's body and every commitment; only solve its
// header nonce. The submitted bytes and all expected identities use this header.
struct CatalogSolvedTemplate {
    std::shared_ptr<const OrchardMiningTemplate> source;
    BlockHeader header;
    std::vector<uint8_t> bytes;
    explicit CatalogSolvedTemplate(std::shared_ptr<const OrchardMiningTemplate> value):source(std::move(value)) {
        OrchardAdmissionFixture::Require(bool(source));
        header=OrchardAdmissionFixture::SolveHeader(source->Header());bytes=source->WireBytes();
        const auto prefix=header.SerializeForHash();
        OrchardAdmissionFixture::Require(bytes.size()>=prefix.size());
        std::copy(prefix.begin(),prefix.end(),bytes.begin());
        OrchardAdmissionFixture::Require(OrchardBlockCandidate::DecodeExact(bytes).Header().GetHash()==header.GetHash());
    }
    const auto& Header()const{return header;}
    const auto& WireBytes()const{return bytes;}
    const auto& Transactions()const{return source->Transactions();}
    auto Height()const{return source->Height();}
};
auto SolveCatalogTemplate(std::shared_ptr<const OrchardMiningTemplate> source) {
    return std::make_unique<CatalogSolvedTemplate>(std::move(source));
}
}
namespace {
struct CompactStartupFixture:CanonicalPoolFixture {
    AnnotatedRecursiveMutex startup_mutex;
    ChainDB reopened;
    std::shared_ptr<const CatalogSolvedTemplate> first,second;
    std::map<uint256,std::string> expected_catalog;
    std::unique_ptr<OrchardParentReplay> independent;
    consensus::HeaderChainSelector headers;
    unsigned attempts=0;
    explicit CompactStartupFixture(const std::function<void(CompactStartupFixture&)>& capture={},
        OrchardAdmissionFixture::HistoricalSpend historical_spend=OrchardAdmissionFixture::HistoricalSpend::None):CanonicalPoolFixture(true,historical_spend) {
        const auto require=[](bool ok){OrchardAdmissionFixture::Require(ok);};
        // Persist the actual independently validated historical bodies and
        // locators. The source store is then produced solely by real writers.
        for(uint32_t h=0;h<f.blocks.size();++h) {
            const auto& b=f.blocks[h];auto* index=dinero::FindBlockIndex(b.GetHash());require(index!=nullptr);
            const auto pos=files->writeBlock(b.GetHash(),b);require(pos.ok());
            index->file_number=pos->file_number;index->data_pos=pos->offset;index->data_size=pos->size;
            require(f.db.updateBlockIndex(f.token,index)==Status::Ok);require(headers.AddHeader(b.header));
        }
        f.service->setRuntimeBlockNotifications(std::make_shared<TypedForkNotices>());
        first=SolveCatalogTemplate(Build());require(bool(first));require(Submit(first->WireBytes()).connected);
        require(headers.AddHeader(first->Header()));
        const auto& shield=first->Transactions().at(1);
        const auto spend=SelectionSpend(f,OutPoint(shield.GetTxid(),0),shield.OutputCoin(0,102),100000);
        const MempoolTransaction spent(spend);
        const auto child=SelectionSpend(f,OutPoint(spend.GetTxid(),0),spent.OutputCoin(0,103),200000);
        require(f.ingress->SubmitBody(spent,TxOrigin::INTERNAL).accepted());
        require(f.ingress->SubmitBody(MempoolTransaction(child),TxOrigin::INTERNAL).accepted());
        BlockAssembler assembler(&f.db);WireOrchardAssembler(assembler,f);
        second=SolveCatalogTemplate(assembler.CreateOrchardBlock(OrchardMiningPayout));
        require(bool(second));require(Submit(second->WireBytes()).connected);
        if(capture)capture(*this); // Optional synchronous capture before removing full storage.
        for(const auto& hash:{parent->hash,first->Header().GetHash(),second->Header().GetHash()}) {
            const auto raw=f.db.getOrchardCatalogState(hash);require(raw.ok());expected_catalog.emplace(hash,*raw);
        }
        for(const auto& source:{second,first}) {
            const auto body=OrchardBlockCandidate::DecodeExact(source->WireBytes());
            const auto c=*consensus::SelectedOrchardBlockContext(body.Header(),source->Height());
            auto& full=ShieldedStateStartupTestAccess::CatalogWriterCoins(*f.service);
            auto undo=PreparedOrchardChainstateWrite::DisconnectIndexed(
                ShieldedStateStartupTestAccess::CatalogWriterMutex(*f.service),f.db,f.token,*files,
                *dinero::FindBlockIndex(c.block_hash),full,c,body,*f.db.getHeader(c.parent_hash),full.GetForest(),true,true);
            undo->Commit();
        }
        independent=std::make_unique<OrchardParentReplay>(OrchardParentReplay::Target{
            parent->height,parent->hash,ChainworkFromHex(parent->chainwork)},SelectedParentReplayWorkLimits());
        OrchardHistoryCapture history(parent->height,parent->hash);CaptureHeaders(history,f.blocks);
        arith_uint256 work{0};
        for(uint32_t h=0;h<f.blocks.size();++h) {
            work+=GetBlockProof(f.blocks[h].header.difficulty);
            independent->Append(f.blocks[h],h,work);history.RecordBody(h,f.blocks[h]);
        }
        independent->Finish();history.Finish();
        const auto initial=OrchardParentCatalogTestAccess::Create(f.db,f.token,*independent,history);
        for(const auto& [point,coin]:independent->ProvenState().ProvenUtxos())
            require(f.db.deleteCoin(f.token,point.txid.AsUint256(),point.vout)==Status::Ok);
        {
            OrchardCompactChainstate original;
            for(const auto& source:{first,second}) {
                const auto body=OrchardBlockCandidate::DecodeExact(source->WireBytes());
                const auto c=*consensus::SelectedOrchardBlockContext(body.Header(),source->Height());
                const bool boundary=source==first;
                auto write=PreparedOrchardChainstateWrite::ConnectCompactIndexed(startup_mutex,f.db,f.token,*files,
                    *dinero::FindBlockIndex(c.block_hash),original,c,body,*f.db.getHeader(c.parent_hash),
                    [this](uint32_t h){return Mtp(h);},true,
                    boundary?std::optional<storage::LegacyRetirementRecord>(independent->Record()):std::nullopt,
                    true,boundary?initial.get():nullptr);
                write->Commit();
            }
        } // Original live compact owner is destroyed before closing its DB.
        f.ingress->Stop();f.db.close();require(reopened.init(f.path)==Status::Ok);
    }
    ~CompactStartupFixture(){reopened.close();}
    std::optional<uint64_t> Mtp(uint32_t h) {
        const auto hash=h<f.blocks.size()?f.blocks[h].GetHash():first->Header().GetHash();
        uint32_t time=0,found=0;
        if(!headers.GetMedianTimePastByHash(hash,time,found)||found!=h)return {};return time;
    }
    auto Restore() {
        return OrchardReindexOwner::RestoreCompact(startup_mutex,reopened,f.token,*files,
            f.path/"flatfiles",f.path/("compact-startup-"+std::to_string(attempts++)));
    }
    auto Rows()const{return ChainDBTransactionReadTestPeer::HandoffRows(reopened);}
    auto ArchiveBytes()const {
        std::map<std::string,std::string> result;
        for(const auto& entry:std::filesystem::directory_iterator(f.path/"flatfiles"/"blocks")) {
            const auto name=entry.path().filename().string();
            if(entry.is_regular_file()&&(name.starts_with("blk")||name.starts_with("rev"))) {
                std::ifstream in(entry.path(),std::ios::binary);
                std::string bytes((std::istreambuf_iterator<char>(in)),{});
                OrchardAdmissionFixture::Require(!in.bad());result.emplace(name,std::move(bytes));
            }
        }
        return result;
    }
    void Mutate(const std::function<void(shielded_store_fixture::Raw&)>& change) {
        reopened.close();auto families=shielded_store_fixture::legacy;families.push_back(shielded_store_fixture::shielded);
        {shielded_store_fixture::Raw raw(f.path,families);change(raw);}
        OrchardAdmissionFixture::Require(reopened.init(f.path)==Status::Ok);
    }
    void Undo(OrchardCompactChainstate& owner,const std::shared_ptr<const CatalogSolvedTemplate>& source) {
        const auto body=OrchardBlockCandidate::DecodeExact(source->WireBytes());
        const auto c=*consensus::SelectedOrchardBlockContext(body.Header(),source->Height());
        auto undo=PreparedOrchardChainstateWrite::DisconnectCompactIndexed(startup_mutex,reopened,f.token,*files,
            *dinero::FindBlockIndex(c.block_hash),owner,c,body,*reopened.getHeader(c.parent_hash),
            [this](uint32_t h){return Mtp(h);},true);undo->Commit();
    }
    void Connect(OrchardCompactChainstate& owner,const std::shared_ptr<const CatalogSolvedTemplate>& source) {
        const auto body=OrchardBlockCandidate::DecodeExact(source->WireBytes());
        const auto c=*consensus::SelectedOrchardBlockContext(body.Header(),source->Height());
        auto write=PreparedOrchardChainstateWrite::ConnectCompactIndexed(startup_mutex,reopened,f.token,*files,
            *dinero::FindBlockIndex(c.block_hash),owner,c,body,*reopened.getHeader(c.parent_hash),
            [this](uint32_t h){return Mtp(h);},true,std::nullopt,true);write->Commit();
    }
};
}
TEST(OrchardCanonicalCatalog, CompactFreshOwnerReplaysPreservesSourceAndRestoresBoundary) {
    CompactStartupFixture f;const auto rows=f.Rows();const auto archives=f.ArchiveBytes();
    auto owner=f.Restore();ASSERT_TRUE(owner);EXPECT_EQ(f.Rows(),rows);EXPECT_EQ(f.ArchiveBytes(),archives);
    {std::lock_guard<AnnotatedRecursiveMutex> lock(f.startup_mutex);
        ASSERT_NE(owner->SelectedUnderLock(f.startup_mutex),nullptr);
        EXPECT_EQ(owner->SelectedUnderLock(f.startup_mutex)->Encode(),f.expected_catalog.at(f.second->Header().GetHash()));}
    f.Undo(*owner,f.second);f.Undo(*owner,f.first);owner.reset();
    f.reopened.close();ASSERT_EQ(f.reopened.init(f.f.path),Status::Ok);
    const auto parent_rows=f.Rows();const auto parent_archives=f.ArchiveBytes();
    auto boundary=f.Restore();ASSERT_TRUE(boundary);EXPECT_EQ(f.Rows(),parent_rows);EXPECT_EQ(f.ArchiveBytes(),parent_archives);
    f.Connect(*boundary,f.first);f.Connect(*boundary,f.second);
    EXPECT_EQ(*f.reopened.getOrchardCatalogState(f.second->Header().GetHash()),f.expected_catalog.at(f.second->Header().GetHash()));
    size_t coins=0;ASSERT_EQ(f.reopened.forEachUTXO([&](const uint256&,uint32_t,const Coin&){++coins;return true;}),Status::Ok);
    EXPECT_EQ(coins,0u);
}
TEST(OrchardCanonicalCatalog, CompactFreshOwnerRejectsForgedCatalogAndOffPathNodeLoss) {
    namespace c=storage::catalog;CompactStartupFixture f;
    const auto hash=f.second->Header().GetHash();const auto original=f.expected_catalog.at(hash);
    const auto key=std::string("Morchard_catalog_v1/state/")+std::string(reinterpret_cast<const char*>(hash.data),32);
    auto forged=c::State::Decode(original);++forged.transaction_count;
    f.Mutate([&](auto& raw){raw.put("utreexo",key,forged.Encode());});
    const auto bad_rows=f.Rows();EXPECT_THROW((void)f.Restore(),std::exception);EXPECT_EQ(f.Rows(),bad_rows);
    f.Mutate([&](auto& raw){raw.put("utreexo",key,original);});
    // A historical transaction-id leaf that the next spend need not query.
    // Startup must verify complete reachable storage, not just a lookup path.
    auto id=c::State::Decode(f.expected_catalog.at(f.parent->hash)).transactions;
    std::string node;
    while(true) {const auto value=f.reopened.getOrchardCatalogNode(id);ASSERT_TRUE(value.ok());node=*value;
        const auto decoded=c::Node::Decode(node);if(decoded.bit==256)break;id=decoded.zero;}
    const auto node_key=std::string("Morchard_catalog_v1/node/")+std::string(reinterpret_cast<const char*>(id.data),32);
    f.Mutate([&](auto& raw){rocksdb::WriteOptions options;options.sync=true;
        OrchardAdmissionFixture::Require(raw.db->Delete(options,raw.cf("utreexo"),node_key).ok());});
    const auto missing_rows=f.Rows();EXPECT_THROW((void)f.Restore(),std::exception);EXPECT_EQ(f.Rows(),missing_rows);
    EXPECT_EQ(f.reopened.getOrchardCatalogNode(id).status(),Status::NotFound);
    f.Mutate([&](auto& raw){raw.put("utreexo",node_key,node);});
    EXPECT_TRUE(f.Restore());
}
TEST(OrchardCanonicalCatalog, CompactFreshOwnerRejectsStorageDomainAndRetainedJournalDamage) {
    CompactStartupFixture f;const auto rows=f.Rows();const auto archives=f.ArchiveBytes();
    BlockStorage wrong;ASSERT_EQ(wrong.init(f.f.path/"wrong-files"),Status::Ok);
    EXPECT_THROW((void)OrchardReindexOwner::RestoreCompact(f.startup_mutex,f.reopened,f.f.token,wrong,
        f.f.path/"flatfiles",f.f.path/"wrong-domain-scratch"),std::exception);
    EXPECT_EQ(f.Rows(),rows);EXPECT_EQ(f.ArchiveBytes(),archives);
    const auto hash=f.second->Header().GetHash();
    const auto key=std::string("orchard_consensus_journal:v1:00000067:")+hash.GetHex();
    std::string journal;ASSERT_EQ(f.reopened.getRaw(key,journal),Status::Ok);
    rocksdb::WriteBatch bad;bad.Put(key,"bad-journal");ASSERT_EQ(f.reopened.writeBatch(f.f.token,std::move(bad),true),Status::Ok);
    const auto bad_rows=f.Rows();EXPECT_THROW((void)f.Restore(),std::exception);EXPECT_EQ(f.Rows(),bad_rows);EXPECT_EQ(f.ArchiveBytes(),archives);
    rocksdb::WriteBatch restore;restore.Put(key,journal);ASSERT_EQ(f.reopened.writeBatch(f.f.token,std::move(restore),true),Status::Ok);
    EXPECT_TRUE(f.Restore());
}

namespace {
class OrchardReadErrorIterator final : public rocksdb::Iterator {
    std::unique_ptr<rocksdb::Iterator> original_;
    size_t fail_after_, moves_=0;
    bool fail_=false;
    void Observe(){if(moves_==fail_after_){fail_=true;observed=true;}}
public:
    bool observed=false;
    OrchardReadErrorIterator(std::unique_ptr<rocksdb::Iterator> original,size_t fail_after)
        :original_(std::move(original)),fail_after_(fail_after){}
    bool Valid()const override{return !fail_&&original_->Valid();}
    void SeekToFirst()override{original_->SeekToFirst();moves_=0;fail_=false;Observe();}
    void SeekToLast()override{original_->SeekToLast();moves_=0;fail_=false;Observe();}
    void Seek(const rocksdb::Slice& target)override{original_->Seek(target);moves_=0;fail_=false;Observe();}
    void SeekForPrev(const rocksdb::Slice& target)override{original_->SeekForPrev(target);moves_=0;fail_=false;Observe();}
    void Next()override{original_->Next();++moves_;Observe();}
    void Prev()override{original_->Prev();++moves_;Observe();}
    rocksdb::Slice key()const override{return original_->key();}
    rocksdb::Slice value()const override{return original_->value();}
    rocksdb::Status status()const override{
        return fail_?rocksdb::Status::IOError("isolated Orchard iterator read refusal"):original_->status();
    }
};
}
TEST(OrchardCanonicalCatalog, CompactStorageComparisonChecksReadErrorsAndBothEnds) {
    CompactStartupFixture f;using Peer=ChainDBTransactionReadTestPeer;
    const auto before=f.Rows();
    EXPECT_EQ(f.reopened.compareOrchardStorage(f.reopened),Status::Ok);
    size_t count=0;
    {auto it=Peer::OrchardComparisonIterator(f.reopened);
        for(it->Seek("O1");it->Valid()&&it->key().starts_with("O1");it->Next())++count;
        ASSERT_TRUE(it->status().ok());}
    ASSERT_GT(count,1u);
    for(bool expected_side:{false,true})for(size_t at:{size_t{0},size_t{1},count}) {
        SCOPED_TRACE(std::to_string(expected_side)+":"+std::to_string(at));
        OrchardReadErrorIterator failed(Peer::OrchardComparisonIterator(f.reopened),at);
        auto healthy=Peer::OrchardComparisonIterator(f.reopened);
        EXPECT_EQ(expected_side?Peer::CompareOrchardIterators(f.reopened,*healthy,failed):
            Peer::CompareOrchardIterators(f.reopened,failed,*healthy),Status::Io);
        EXPECT_TRUE(failed.observed);
        EXPECT_EQ(f.reopened.compareOrchardStorage(f.reopened),Status::Ok);
    }
    ChainDB closed;
    EXPECT_EQ(closed.compareOrchardStorage(f.reopened),Status::Internal);
    EXPECT_EQ(f.reopened.compareOrchardStorage(closed),Status::Internal);
    EXPECT_EQ(f.Rows(),before);
}
TEST(OrchardCanonicalCatalog, CompactFreshOwnerRejectsUnclaimedOrchardRows) {
    CompactStartupFixture f;
    const auto check=[&](const std::string& key) {
        SCOPED_TRACE(key.substr(0,3));
        const auto before=f.Rows();const auto archives=f.ArchiveBytes();
        f.Mutate([&](auto& raw){std::string existing;
            OrchardAdmissionFixture::Require(raw.db->Get({},raw.cf(shielded_store_fixture::shielded),key,&existing).IsNotFound());
            raw.put(shielded_store_fixture::shielded,key,"unclaimed startup row");});
        const auto corrupted=f.Rows();
        EXPECT_THROW((void)f.Restore(),std::exception);
        EXPECT_EQ(f.Rows(),corrupted);EXPECT_EQ(f.ArchiveBytes(),archives);
        f.Mutate([&](auto& raw){rocksdb::WriteOptions options;options.sync=true;
            OrchardAdmissionFixture::Require(raw.db->Delete(options,raw.cf(shielded_store_fixture::shielded),key).ok());});
        EXPECT_EQ(f.Rows(),before);EXPECT_TRUE(f.Restore());
        EXPECT_EQ(f.Rows(),before);EXPECT_EQ(f.ArchiveBytes(),archives);
    };
    check(std::string("O1U")+std::string(32,char{0x7f}));
    check("O1Z-unclaimed");
    auto owner=f.Restore();ASSERT_TRUE(owner);
    f.Undo(*owner,f.second);f.Undo(*owner,f.first);owner.reset();
    f.reopened.close();ASSERT_EQ(f.reopened.init(f.f.path),Status::Ok);
    ASSERT_EQ(f.reopened.getOrchardState().status(),Status::NotFound);
    for(char kind:{'N','A','U'})check(std::string("O1")+kind+std::string(32,char{0x7f}));
    check("O1Z-unclaimed");
    auto boundary=f.Restore();ASSERT_TRUE(boundary);
    f.Connect(*boundary,f.first);f.Connect(*boundary,f.second);
    EXPECT_EQ(*f.reopened.getOrchardCatalogState(f.second->Header().GetHash()),
        f.expected_catalog.at(f.second->Header().GetHash()));
}

TEST(OrchardCanonicalCatalog, ServiceSpendsStartupAndReindexPreserveCatalog) {
    namespace c=storage::catalog;CanonicalPoolFixture f(true);
    ASSERT_EQ(Params().TargetSpacing(101),120u);
    ASSERT_EQ(Params().TargetSpacing(102),60u);
    ASSERT_TRUE(Params().regtest_enforce_pow);
    auto notices=std::make_shared<TypedForkNotices>();
    f.f.service->setRuntimeBlockNotifications(notices);
    // Give the genuine historical frames their actual flatfile locators. No
    // synthetic undo or catalog record is installed by the fixture.
    {
        auto selected=f.f.service->AcquireBlockIngressActivationLock();
        std::lock_guard<std::recursive_mutex> graph(g_block_index_mutex);
        for(uint32_t height=0;height<f.f.blocks.size();++height) {
            const auto& body=f.f.blocks[height];auto* index=dinero::FindBlockIndex(body.GetHash());ASSERT_NE(index,nullptr);
            const auto position=f.files->writeBlock(body.GetHash(),body);ASSERT_TRUE(position.ok());
            CBlockIndex recorded=*index;recorded.file_number=position->file_number;
            recorded.data_pos=position->offset;recorded.data_size=position->size;
            rocksdb::WriteBatch batch;ASSERT_EQ(f.f.db.updateBlockIndex(f.f.token,&recorded,&batch),Status::Ok);
            ASSERT_EQ(f.f.db.writeBatch(f.f.token,std::move(batch),true),Status::Ok);
            index->file_number=recorded.file_number;index->data_pos=recorded.data_pos;index->data_size=recorded.data_size;
        }
    }
    const auto first=SolveCatalogTemplate(f.Build());ASSERT_TRUE(first);ASSERT_EQ(first->Transactions().size(),2u);
    ASSERT_TRUE(f.Submit(first->WireBytes()).connected);
    const auto initial=f.f.db.getOrchardCatalogState(f.parent->hash);
    const auto a=f.f.db.getOrchardCatalogState(first->Header().GetHash());ASSERT_TRUE(initial.ok());ASSERT_TRUE(a.ok());
    const auto before=c::State::Decode(*initial),after_first=c::State::Decode(*a);
    const auto read=[&](const uint256& id){auto value=f.f.db.getOrchardCatalogNode(id);if(!value.ok())throw std::runtime_error("catalog test node unavailable");return *value;};
    c::Tree ids(c::Kind::Transactions,read),legacy(c::Kind::LegacyCoins,read);
    const OutPoint funding(f.f.blocks[1].vtx.front().GetTxid(),0);
    const auto funding_key=c::LegacyKey(c::OutpointBytes(funding));
    EXPECT_TRUE(legacy.Find(before.legacy,funding_key));EXPECT_FALSE(legacy.Find(after_first.legacy,funding_key));
    EXPECT_EQ(after_first.legacy_count+1,before.legacy_count);
    EXPECT_EQ(after_first.transaction_count,before.transaction_count+first->Transactions().size());
    for(const auto& tx:first->Transactions())EXPECT_TRUE(ids.Find(after_first.transactions,c::TransactionKey(tx.GetTxid())));
    const auto& shield=first->Transactions()[1];
    const OutPoint modern(shield.GetTxid(),0);EXPECT_FALSE(legacy.Find(after_first.legacy,c::LegacyKey(c::OutpointBytes(modern))));
    const auto spend=SelectionSpend(f.f,modern,shield.OutputCoin(0,102),100000);
    const MempoolTransaction spend_body(spend);
    const auto child=SelectionSpend(f.f,OutPoint(spend.GetTxid(),0),spend_body.OutputCoin(0,103),200000);
    ASSERT_TRUE(f.f.ingress->SubmitBody(spend_body,TxOrigin::INTERNAL).accepted());
    ASSERT_TRUE(f.f.ingress->SubmitBody(MempoolTransaction(child),TxOrigin::INTERNAL).accepted());
    BlockAssembler assembler(&f.f.db);WireOrchardAssembler(assembler,f.f);
    const auto second=SolveCatalogTemplate(assembler.CreateOrchardBlock(OrchardMiningPayout));ASSERT_TRUE(second);
    ASSERT_EQ(second->Height(),103u);ASSERT_EQ(second->Transactions().size(),3u);
    ASSERT_TRUE(f.Submit(second->WireBytes()).connected);
    const auto b=f.f.db.getOrchardCatalogState(second->Header().GetHash());ASSERT_TRUE(b.ok());
    const auto after=c::State::Decode(*b);EXPECT_EQ(after.legacy,after_first.legacy);EXPECT_EQ(after.legacy_count,after_first.legacy_count);
    EXPECT_EQ(after.transaction_count,after_first.transaction_count+second->Transactions().size());
    for(const auto& tx:second->Transactions())EXPECT_TRUE(ids.Find(after.transactions,c::TransactionKey(tx.GetTxid())));
    EXPECT_EQ(f.f.db.getCoin(spend.GetTxid().AsUint256(),0).status(),Status::NotFound);
    EXPECT_TRUE(ids.Find(after.transactions,c::TransactionKey(spend.GetTxid()))); // fully spent in the same block
    EXPECT_FALSE(legacy.Find(after.legacy,c::LegacyKey(c::OutpointBytes(OutPoint(child.GetTxid(),0)))));
    EXPECT_TRUE(ShieldedStateStartupTestAccess::AuditBoundary(*f.f.service));
    // Actual startup-only reindex owner, isolated candidate, original source
    // remains selected. It must reconstruct the same catalog from real bodies.
    const auto candidate_path=f.f.path/"catalog-reindex-candidate";ChainDB candidate;
    ASSERT_EQ(candidate.init(candidate_path),Status::Ok);
    consensus::BlockReindexer::Config config;config.mode=consensus::BlockReindexer::Mode::FULL;config.use_assumevalid=false;
    config.shielded_frontier_output_path=f.f.path/"catalog-reindex-frontier.bin";
    config.shielded_nullifier_db_path=f.f.path/"catalog-reindex-nullifiers.db";
    const auto rebuilt=OrchardReindexOwner::Run(f.f.db,candidate,f.f.token,*f.files,f.f.path/"flatfiles",candidate_path,config);
    ASSERT_TRUE(rebuilt.ok());ASSERT_TRUE(rebuilt->success)<<rebuilt->error;
    const auto rebuilt_parent=candidate.getOrchardCatalogState(f.parent->hash);
    const auto rebuilt_first=candidate.getOrchardCatalogState(first->Header().GetHash());
    const auto rebuilt_tip=candidate.getOrchardCatalogState(second->Header().GetHash());
    ASSERT_TRUE(rebuilt_parent.ok());ASSERT_TRUE(rebuilt_first.ok());ASSERT_TRUE(rebuilt_tip.ok());
    EXPECT_EQ(*rebuilt_parent,*initial);EXPECT_EQ(*rebuilt_first,*a);EXPECT_EQ(*rebuilt_tip,*b);
    EXPECT_EQ(f.f.db.getTip()->hash,second->Header().GetHash());
    const auto candidate_read=[&](const uint256& id){auto v=candidate.getOrchardCatalogNode(id);if(!v.ok())throw std::runtime_error("reindex catalog node unavailable");return *v;};
    c::Tree rebuilt_ids(c::Kind::Transactions,candidate_read),rebuilt_coins(c::Kind::LegacyCoins,candidate_read);
    for(const auto& tx:second->Transactions())EXPECT_TRUE(rebuilt_ids.Find(after.transactions,c::TransactionKey(tx.GetTxid())));
    EXPECT_FALSE(rebuilt_coins.Find(after.legacy,funding_key));
    candidate.close();ASSERT_EQ(candidate.init(candidate_path),Status::Ok);EXPECT_EQ(*candidate.getOrchardCatalogState(second->Header().GetHash()),*b);
    candidate.close();
    std::string error;ASSERT_TRUE(f.f.service->InvalidateBlock(second->Header().GetHash(),error))<<error;
    ASSERT_EQ(notices->plan_count,1u);ASSERT_TRUE(notices->plans[0]);
    EXPECT_TRUE(notices->progress[0].complete);
    EXPECT_EQ(notices->progress[0].disconnected,1u);EXPECT_EQ(notices->progress[0].connected,0u);
    ASSERT_EQ(notices->plans[0]->disconnect.size(),1u);EXPECT_TRUE(notices->plans[0]->connect.empty());
    EXPECT_EQ(notices->plans[0]->disconnect[0].hash,second->Header().GetHash());
    EXPECT_EQ(notices->plans[0]->disconnect[0].body.Orchard().WireBytes(),second->WireBytes());
    EXPECT_EQ(f.f.service->GetActiveTip()->hash,first->Header().GetHash());EXPECT_EQ(*f.f.db.getOrchardCatalogState(first->Header().GetHash()),*a);
    EXPECT_TRUE(ShieldedStateStartupTestAccess::AuditBoundary(*f.f.service));
    ASSERT_TRUE(f.f.service->ReconsiderBlock(second->Header().GetHash(),error))<<error;
    f.f.service->PumpReplayMetadataRecovery();
    EXPECT_EQ(f.f.service->GetActiveTip()->hash,second->Header().GetHash());EXPECT_EQ(*f.f.db.getOrchardCatalogState(second->Header().GetHash()),*b);
    ASSERT_EQ(notices->event_count,4u);
    EXPECT_EQ(notices->events[2].hash,second->Header().GetHash());
    EXPECT_EQ(notices->events[2].direction,RuntimeBlockDirection::Disconnect);
    EXPECT_EQ(notices->events[3].hash,second->Header().GetHash());
    EXPECT_EQ(notices->events[3].direction,RuntimeBlockDirection::Connect);
    // Refusal probes run after all canonical transitions. Startup refusal
    // deliberately latches safe mode; the fixture never resets that guard.
    const auto audit_undo=[&] {
        auto selected=f.f.service->AcquireBlockIngressActivationLock();
        const auto state=f.f.db.getOrchardState();if(!state.ok())throw std::runtime_error("fixture Orchard state missing");
        const auto context=consensus::SelectedOrchardBlockContext(second->Header(),103);
        if(!context)throw std::runtime_error("fixture Orchard context missing");
        const auto body=OrchardBlockCandidate::DecodeExact(second->WireBytes());
        return consensus::AuditOrchardUndoStepUnderLock(f.f.db,*f.files,*context,body,first->Header(),
            *state,f.f.service->GetConsensusUTXOSet()->GetForest(),true,true);
    };
    EXPECT_NO_THROW((void)audit_undo());
    // Serialized fixture corruption of exact existing records. The checked
    // production writer has no deletion/reset API. Restore exact original
    // bytes after each refusal, without changing coins, headers or tips.
    for(unsigned fault=0;fault<4;++fault) {
        SCOPED_TRACE(fault);
        const bool parent_fault=fault==1;
        const auto hash=parent_fault?first->Header().GetHash():second->Header().GetHash();
        const auto key=std::string("Morchard_catalog_v1/state/")+std::string(reinterpret_cast<const char*>(hash.data),32);
        const std::string original=parent_fault?*a:*b;
        auto families=shielded_store_fixture::legacy;families.push_back(shielded_store_fixture::shielded);
        f.f.db.close();
        {
            shielded_store_fixture::Raw raw(f.f.path,families);
            if(fault<2) {
                rocksdb::WriteOptions options;options.sync=true;
                ASSERT_TRUE(raw.db->Delete(options,raw.cf("utreexo"),key).ok());
            } else if(fault==2) {auto bad=original;bad.back()^=1;raw.put("utreexo",key,bad);}
            else {auto bad=c::State::Decode(original);bad.previous_record.data[0]^=1;raw.put("utreexo",key,bad.Encode());}
        }
        ASSERT_EQ(f.f.db.init(f.f.path),Status::Ok);
        EXPECT_FALSE(ShieldedStateStartupTestAccess::AuditBoundary(*f.f.service));
        EXPECT_TRUE(f.f.service->IsInSafeMode());
        EXPECT_THROW((void)audit_undo(),std::exception);
        EXPECT_EQ(f.f.db.getTip()->hash,second->Header().GetHash());
        EXPECT_EQ(f.f.service->GetActiveTip()->hash,second->Header().GetHash());
        f.f.db.close();{shielded_store_fixture::Raw raw(f.f.path,families);raw.put("utreexo",key,original);}
        ASSERT_EQ(f.f.db.init(f.f.path),Status::Ok);
        EXPECT_TRUE(ShieldedStateStartupTestAccess::AuditBoundary(*f.f.service));EXPECT_NO_THROW((void)audit_undo());
        EXPECT_TRUE(f.f.service->IsInSafeMode()); // Valid data does not clear the startup safety latch.
    }
}

} // namespace dinero
#else
TEST(OrchardCanonicalCatalog, UnavailableWithoutBackend) {dinero::ChainstateService service;EXPECT_FALSE(service.PrepareSelectedOrchardParent());}
#endif
