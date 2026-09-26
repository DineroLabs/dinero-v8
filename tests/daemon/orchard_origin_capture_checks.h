#pragma once
#include "daemon/services/assumeutxo_replay.h"
#include "wallet/runtime_origin_projection.h"
#include "wallet/runtime_index_delivery.h"
#include "consensus/subsidy.h"
#include "consensus/orchard_profile.h"
#include "consensus/orchard_legacy_accounting.h"
#include <sqlite3.h>
#include <future>

// A separate, genesis-bound regtest history. Unlike AtomicForest's deliberately
// synthetic prehistory, every origin ancestor passes the owned replay engine.
static void ServiceOriginCaptureChecks() {
    using Access=dinero::ShieldedStateStartupTestAccess;
    struct Restore { ChainParams p; NodeConfig c; ~Restore(){MutableParams()=p;GetConfig()=c;} } restore{Params(),GetConfig()};
    auto& params=MutableParams();
    params.orchard_activation_height=4;params.orchard_branch_id=0xa1b2c3d4;
    params.shielded_activation_height=1;
    params.state_commitment_activation_height=UINT32_MAX;
    params.enforce_witness_commitment=true;params.witness_commitment_enforcement_height=4;
    GetConfig().utreexo_stateless=false;GetConfig().consensus_atomic_persist=false;
    TempDir temp;
    // Select the supported separated test layout without Seed()'s unrelated
    // synthetic nullifiers or opaque legacy-state records.
    { auto families=legacy;families.push_back(shielded_store_fixture::shielded);Raw raw(temp.path,families);
      const uint32_t version=4;
      raw.put("meta","schema_version",std::string(reinterpret_cast<const char*>(&version),4));
      raw.put("meta","storage_layout_v1",ready);
      consensus::shielded::CommitmentTree empty_tree;consensus::shielded::AnchorHistory empty_anchors;
      const auto f=empty_tree.SerializeFrontier(),a=empty_anchors.SerializePersistenceBytes();
      raw.put(shielded_store_fixture::shielded,"Mshielded_frontier",std::string(f.begin(),f.end()));
      raw.put(shielded_store_fixture::shielded,"Mshielded_anchor_history",std::string(a.begin(),a.end()));
      raw.put("meta","shielded_tip",std::string(84,'\0')); }
    ChainDB db;CHECK(db.init(temp.path)==Status::Ok);
    auto files=std::make_shared<BlockStorage>();CHECK(files->init(temp.path)==Status::Ok);
    Block genesis;genesis.header=BuildCanonicalGenesis(params).header;
    Transaction cb;CHECK(TransactionSerializer::Deserialize(cb,params.genesis.genesisCoinbaseHex));genesis.vtx.push_back(cb);
    ConsensusUTXOSet builder;
    for(uint32_t n=0;n<cb.vout.size();++n)CHECK(builder.AddCoin(OutPoint(cb.GetTxid(),n),
        UTXOEntry(cb.vout[n].value,cb.vout[n].scriptPubKey,0,true,cb.vout[n].is_confidential,cb.vout[n].commitment)));
    BlockValidator validator(&builder);
    assumeutxo::AssumeUtxoReplayEngine replay;std::string error;
    CHECK(replay.SeedGenesis(genesis,error));
    HeaderChainSelector headers;CHECK(headers.AddHeader(genesis.header));
    arith_uint256 work=GetBlockProof(genesis.header.difficulty);
    auto store=[&](const Block& body,uint32_t height) {
        CHECK(db.putBlock(token,body.GetHash(),body)==Status::Ok);
        CHECK(db.putHeader(token,body.GetHash(),body.header,height,work)==Status::Ok);
        CHECK(db.putHeightIndex(token,height,body.GetHash())==Status::Ok);
    };
    store(genesis,0);std::vector<Block> history{genesis};
    for(uint32_t height=1;height<4;++height) {
        Block body;body.header.version=1;body.header.prev_block_hash=history.back().GetHash();
        body.header.timestamp=genesis.header.timestamp+height*120;body.header.difficulty=0x207fffff;
        body.header.ZeroReserved();
        Transaction coinbase;coinbase.version=2;TxInput in;in.prevout.vout=UINT32_MAX;
        in.scriptSig=CoinbaseHeightScript(height);coinbase.vin.push_back(in);
        Bytes script(22,0);script[1]=20;script[2]=height;
        coinbase.vout.emplace_back(ConsensusSubsidy::GetBlockSubsidy(height),script);
        body.vtx.push_back(coinbase);body.header.merkle_root=coinbase.GetTxid().AsUint256();
        CHECK(validator.ComputeUtreexoRootPure(body,height,body.header.utreexo_root,error));
        BlockUndo undo;CHECK(validator.ConnectBlock(body,height,body.GetHash(),undo,error));
        CHECK(replay.ConnectAndAdvance(body,height,body.GetHash(),error));
        CHECK(headers.AddHeader(body.header));work+=GetBlockProof(body.header.difficulty);
        store(body,height);history.push_back(body);
    }
    const auto parent=history.back().header;const auto forest=*replay.Forest();
    CHECK(replay.UtreexoRootHex()==parent.utreexo_root.GetHex());
    ConsensusUTXOSet live;View view;view.height=3;rocksdb::WriteBatch seed;
    for(const auto& [point,coin]:replay.ProvenUtxos()) {
        CHECK(live.AddCoin(point,coin));view.coins.emplace(point,coin);
        Coin stored;stored.amount=coin.value.GetUna();stored.script_pubkey=StorageScriptHex(coin.scriptPubKey);
        stored.height=coin.height;stored.coinbase=coin.isCoinbase;
        CHECK(db.putCoin(token,point.txid.AsUint256(),point.vout,stored,&seed)==Status::Ok);
    }
    live.ReplaceForestGuarded(forest);live.SetBestBlock(parent.GetHash(),3);
    CHECK(db.putForestTipMarker(token,{3,parent.GetHash(),parent.utreexo_root},&seed)==Status::Ok);
    CHECK(db.putUtreexoCheckpointWithChecksum(token,3,forest.serialize(),&seed)==Status::Ok);
    const auto frontier=replay.ShieldedTree()->SerializeFrontier();
    const auto anchors=replay.ShieldedAnchors()->SerializePersistenceBytes();
    CHECK(db.putShieldedState(token,ChainDB::ShieldedStateRecord::Frontier,{frontier.begin(),frontier.end()},&seed)==Status::Ok);
    CHECK(db.putShieldedState(token,ChainDB::ShieldedStateRecord::AnchorHistory,{anchors.begin(),anchors.end()},&seed)==Status::Ok);
    uint256 shielded_root;const auto root=replay.ShieldedTree()->Root();std::copy(root.begin(),root.end(),shielded_root.begin());
    CHECK(db.putShieldedTipMarker(token,{3,parent.GetHash(),shielded_root,0,0},&seed)==Status::Ok);
    CHECK(db.setTip(token,parent.GetHash(),3,work,&seed)==Status::Ok);
    CHECK(db.setValidatedTip(token,parent.GetHash(),3,&seed)==Status::Ok);Commit(db,seed);
    // Accounting is derived from the very history just independently replayed.
    AnnotatedRecursiveMutex activation;
    CHECK(OrchardProfileConfigurationValid(Params()));
    CHECK(Params().orchard_activation_height==4 && Params().shielded_activation_height==1);
    for(uint32_t height=1;height<4;++height) {
        CHECK(db.getBlock(history[height].GetHash()).ok());
        CHECK(db.getBlockHeight(history[height].GetHash()).ok());
    }
    std::optional<storage::LegacyRetirementRecord> retirement;
    {std::lock_guard<AnnotatedRecursiveMutex> lock(activation);
     try { const auto accounting=DeriveSelectedLegacyPoolAccountingUnderLock(db,files.get(),4);
         CHECK(accounting.blocks_read==3 && accounting.value_una==0);
         std::cout<<"Origin capture historical accounting PASS\n";
         retirement=DeriveSelectedLegacyRetirementUnderLock(db,files.get(),4); }
     catch(const OrchardStateLookupError& e) { throw std::runtime_error("Origin fixture retirement status "+std::to_string(int(e.SourceStatus()))); }}
    CHECK(retirement->retired_value==0 && retirement->boundary_parent==parent.GetHash());
    auto context=*SelectedOrchardBlockContext(BlockHeader{},4);context.parent_hash=parent.GetHash();
    auto bare=WithParentTiming(CandidateWires(context,{}),parent);context.block_hash=bare.Header().GetHash();
    const auto initial=PrepareOrchardBlockCoinsUnderChainstateLock(bare,context,view,{},true);
    const auto filtered=WithFilterHash(bare,BuildOrchardBlockFilter(initial).GetHash());context.block_hash=filtered.Header().GetHash();
    const auto filtered_coins=PrepareOrchardBlockCoinsUnderChainstateLock(filtered,context,view,{},true);
    const auto next=PrepareOrchardStateTransition(context,std::nullopt,filtered_coins.Authorizations(),{
        [&](const uint256& a)->StatusOr<bool>{return db.getOrchardAnchorReferences(a).ok();},
        [&](const uint256& n)->StatusOr<bool>{return db.getOrchardNullifierOwner(n).ok();}});
    const auto sets=RequiredValue(db.previewOrchardCommitmentSets(std::nullopt,next.Next(),next.Nullifiers()));
    const auto commitment=ComputeOrchardStateRoot({context.domain,4,4,context.parent_hash},*retirement,next.Next(),sets);
    const auto draft=AppendStateScript(filtered,BuildStateCommitmentScript(commitment,StateCommitmentEncoding::Orchard));
    context.block_hash=draft.Header().GetHash();
    const auto coins=PrepareOrchardBlockCoinsUnderChainstateLock(draft,context,view,{},true);
    auto header=draft.Header();header.utreexo_root=PrepareOrchardForestTransition(coins,parent,forest).Root();
    auto bytes=draft.WireBytes();const auto prefix=header.SerializeForHash();std::copy(prefix.begin(),prefix.end(),bytes.begin());
    const auto boundary=WithProof(OrchardBlockCandidate::DecodeExact(bytes),MixedProof(coins,forest));
    context.block_hash=header.GetHash();CHECK(headers.AddHeader(header));work+=GetBlockProof(header.difficulty);
    CHECK(db.putHeader(token,context.block_hash,header,4,work)==Status::Ok);
    ChainDB::PersistedHeaderMetadata metadata;metadata.height=4;metadata.parent_hash=parent.GetHash();
    metadata.chainwork=work;metadata.status_flags=BLOCK_VALID_HEADER;
    CHECK(db.putHeaderMetadata(token,context.block_hash,metadata)==Status::Ok);
    auto index=DiskIndex(db,header,4);
    auto write=PreparedOrchardChainstateWrite::ConnectIndexed(activation,db,token,*files,index,live,context,
        boundary,parent,forest,{},true,true,retirement,true);
    write->Commit();write.reset();
    ChainstateService service;service.setChainDB(&db);service.setBlockStorage(files);
    Access::Set(service,index,live.GetForest());
    WalletManager wallet(temp.path/"wallet");wallet.create("origin");
    const auto owned=history[1].vtx.front().vout.front().scriptPubKey;
    wallet.addWatchScript(owned,"m/84'/1'/0'/0/0",false);
    const auto session=wallet.AcquireDatabaseLease()->Session();
    struct Observe { ChainstateService* service; unsigned reads=0; bool unlocked=true; } observe{&service};
    sqlite3_set_authorizer(wallet.getCurrentDatabase(),[](void* opaque,int action,const char* table,const char* column,const char*,const char*) {
        auto& o=*static_cast<Observe*>(opaque);
        if(action==SQLITE_READ && table && column && std::string_view(table)=="watch_scripts" && std::string_view(column)=="path") {
            o.unlocked &= std::async(std::launch::async,[&]{return Access::TryChain(*o.service);}).get();
            if(++o.reads==2)return SQLITE_DENY; // The final ownership recheck must actually run.
        }
        return SQLITE_OK;
    },&observe);
    CHECK(!service.getRuntimeWalletOrigin(wallet,session).ok());
    sqlite3_set_authorizer(wallet.getCurrentDatabase(),nullptr,nullptr);
    CHECK(observe.reads==2 && observe.unlocked);
    const auto capture=service.getRuntimeWalletOrigin(wallet,session);
    CHECK(capture.ok());const auto& projected=**capture;
    CHECK(projected.OriginHeight()==3 && projected.OriginHash()==parent.GetHash());
    CHECK(projected.FirstCursor()==(*service.getRuntimeDeliveryPage({},1))->events.front().cursor);
    CHECK(projected.Coins().size()==1 && projected.Transactions().size()==1);
    const auto& coin=projected.Coins().begin()->second;
    CHECK(coin.height==1 && coin.coinbase && !coin.spent && coin.output.value==history[1].vtx[0].vout[0].value);
    CHECK(projected.Transactions()[0].transaction.Serialize(TxSerializationMode::WithWitness)==history[1].vtx[0].Serialize(TxSerializationMode::WithWitness));
    // Replacing the mutable height index cannot redirect hash-bound ancestry.
    CHECK(db.putHeightIndex(token,1,genesis.GetHash())==Status::Ok);
    CHECK(service.getRuntimeWalletOrigin(wallet,session).ok());
    CHECK(db.putHeightIndex(token,1,history[1].GetHash())==Status::Ok);
    auto corrupt=history[1];corrupt.vtx[0].vout[0].value=AmountUna::Una(1);
    CHECK(db.putBlock(token,history[1].GetHash(),corrupt)==Status::Ok);
    CHECK(!service.getRuntimeWalletOrigin(wallet,session).ok());
    CHECK(projected.Coins().begin()->second.output.value==history[1].vtx[0].vout[0].value);
    CHECK(db.putBlock(token,history[1].GetHash(),history[1])==Status::Ok);
    wallet.open("origin");CHECK(!service.getRuntimeWalletOrigin(wallet,session).ok());
    const auto reopened=wallet.AcquireDatabaseLease()->Session();
    CHECK(service.getRuntimeWalletOrigin(wallet,reopened).ok());
    dinero::UTXOIndex wallet_index((temp.path/"origin-index.sqlite").string());CHECK(wallet_index.Initialize());
    const auto index_owned=history[2].vtx[0].vout[0].scriptPubKey;
    wallet_index.RegisterAddress(index_owned,"m/84'/1'/0'/0/1");
    // Existing event receipts are insufficient if their pre-origin rows were
    // never reconstructed. Exercise the real old narrow delivery API.
    dinero::UTXOIndex incomplete((temp.path/"incomplete-index.sqlite").string());CHECK(incomplete.Initialize());
    incomplete.RegisterAddress(index_owned,"m/84'/1'/0'/0/1");
    const auto first=(*service.getRuntimeDeliveryPage({},1))->events.front();
    RuntimeIndexDelivery::ApplyForWallet(wallet,incomplete,reopened,first);
    auto incomplete_source=service.getRuntimeWalletOrigin(wallet,reopened,&incomplete);CHECK(incomplete_source.ok());
    CHECK(service.adoptRuntimeWalletOrigin(wallet,incomplete,**incomplete_source)!=Status::Ok);
    CHECK(!RuntimeOrdinaryDelivery::ReadForWallet(wallet,reopened));
    dinero::WalletUTXO retained(history[2].vtx[0].GetTxid(),0,history[2].vtx[0].vout[0].value,index_owned,"m/84'/1'/0'/0/1",2,true);
    retained.utreexo_position=42;CHECK(wallet_index.AddUTXO(retained));
    wallet_index.RegisterAddress(history[3].vtx[0].vout[0].scriptPubKey,"m/84'/1'/0'/0/3");
    auto dual=service.getRuntimeWalletOrigin(wallet,reopened,&wallet_index);CHECK(dual.ok());
    CHECK((*dual)->Coins().size()==3);
    // A changed index path binding refuses before either store adopts.
    wallet_index.RegisterAddress(index_owned,"m/84'/1'/0'/0/2");
    CHECK(service.adoptRuntimeWalletOrigin(wallet,wallet_index,**dual)!=Status::Ok);
    CHECK(!RuntimeIndexDelivery::ReadForWallet(wallet,wallet_index,reopened));
    CHECK(!RuntimeOrdinaryDelivery::ReadForWallet(wallet,reopened));
    wallet_index.RegisterAddress(index_owned,"m/84'/1'/0'/0/1");
    CHECK(sqlite3_exec(wallet.getCurrentDatabase(),"CREATE TRIGGER reject_origin_history BEFORE INSERT ON transactions BEGIN SELECT RAISE(ABORT,'origin history failure'); END",nullptr,nullptr,nullptr)==SQLITE_OK);
    CHECK(service.adoptRuntimeWalletOrigin(wallet,wallet_index,**dual)!=Status::Ok);
    CHECK(RuntimeIndexDelivery::ReadForWallet(wallet,wallet_index,reopened)->cursor==(*dual)->FirstCursor());
    CHECK(!RuntimeOrdinaryDelivery::ReadForWallet(wallet,reopened));
    // Index commits first; failed ordinary effects/history/receipt roll back.
    sqlite3_stmt* count=nullptr;CHECK(sqlite3_prepare_v2(wallet.getCurrentDatabase(),"SELECT COUNT(*) FROM utxos",-1,&count,nullptr)==SQLITE_OK);
    CHECK(sqlite3_step(count)==SQLITE_ROW && sqlite3_column_int(count,0)==0);sqlite3_finalize(count);
    CHECK(sqlite3_exec(wallet.getCurrentDatabase(),"DROP TRIGGER reject_origin_history",nullptr,nullptr,nullptr)==SQLITE_OK);
    wallet.open("origin");const auto retry_session=wallet.AcquireDatabaseLease()->Session();
    CHECK(service.adoptRuntimeWalletOrigin(wallet,wallet_index,**dual)!=Status::Ok);
    dual=service.getRuntimeWalletOrigin(wallet,retry_session,&wallet_index);CHECK(dual.ok());
    CHECK(service.adoptRuntimeWalletOrigin(wallet,wallet_index,**dual)==Status::Ok);
    CHECK(RuntimeOrdinaryDelivery::ReadForWallet(wallet,retry_session)->cursor==(*dual)->FirstCursor());
    CHECK(wallet_index.GetUTXO(history[2].vtx[0].GetTxid(),0)->value==history[2].vtx[0].vout[0].value);
    CHECK(!wallet_index.GetUTXO(history[1].vtx[0].GetTxid(),0));
    CHECK(wallet_index.GetUTXO(history[3].vtx[0].GetTxid(),0)->value==history[3].vtx[0].vout[0].value);
    // GetUTXO does not populate the optional position; inspect the durable
    // column rather than mistake that API limitation for a lost field.
    sqlite3* index_read=nullptr;
    CHECK(sqlite3_open_v2((temp.path/"origin-index.sqlite").c_str(),&index_read,SQLITE_OPEN_READONLY,nullptr)==SQLITE_OK);
    CHECK(sqlite3_prepare_v2(index_read,"SELECT utreexo_position FROM wallet_utxos WHERE utreexo_position IS NOT NULL",-1,&count,nullptr)==SQLITE_OK);
    CHECK(sqlite3_step(count)==SQLITE_ROW && sqlite3_column_int64(count,0)==42);
    CHECK(sqlite3_step(count)==SQLITE_DONE);sqlite3_finalize(count);sqlite3_close(index_read);
    CHECK(sqlite3_prepare_v2(wallet.getCurrentDatabase(),"SELECT amount,height,is_coinbase FROM utxos",-1,&count,nullptr)==SQLITE_OK);
    CHECK(sqlite3_step(count)==SQLITE_ROW && sqlite3_column_int64(count,0)==history[1].vtx[0].vout[0].value.GetInt64() && sqlite3_column_int(count,1)==1 && sqlite3_column_int(count,2)==1);
    CHECK(sqlite3_step(count)==SQLITE_DONE);sqlite3_finalize(count);
    CHECK(service.adoptRuntimeWalletOrigin(wallet,wallet_index,**dual)==Status::Ok);
    std::cout<<"OrchardOriginAdoption dual domains/ordered receipts/rollback/reopen PASS\n";
    std::cout<<"OrchardOriginCapture validated ancestors/canonical boundary/immutable projection/reopen PASS\n";
}
