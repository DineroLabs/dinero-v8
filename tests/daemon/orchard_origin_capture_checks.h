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
static void ServiceOriginCaptureChecks(const std::string& fixture_base, bool signed_history=false) {
    using Access=dinero::ShieldedStateStartupTestAccess;
    struct Restore { ChainParams p; NodeConfig c; ~Restore(){MutableParams()=p;GetConfig()=c;} } restore{Params(),GetConfig()};
    auto& params=MutableParams();
    const uint32_t activation_height=signed_history?103:4;
    const uint32_t origin_height=activation_height-1;
    const Fixture keys(fixture_base);
    const auto signed_script=keys.view.coins.at(Point(keys.inputs[0])).scriptPubKey;
    Transaction historical_spend,boundary_spend;
    params.orchard_activation_height=activation_height;params.orchard_branch_id=0xa1b2c3d4;
    params.shielded_activation_height=1;
    params.state_commitment_activation_height=UINT32_MAX;
    params.enforce_witness_commitment=true;params.witness_commitment_enforcement_height=activation_height;
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
    for(uint32_t height=1;height<activation_height;++height) {
        Block body;body.header.version=1;body.header.prev_block_hash=history.back().GetHash();
        body.header.timestamp=genesis.header.timestamp+height*120;body.header.difficulty=0x207fffff;
        body.header.ZeroReserved();
        Transaction coinbase;coinbase.version=2;TxInput in;in.prevout.vout=UINT32_MAX;
        in.scriptSig=CoinbaseHeightScript(height);coinbase.vin.push_back(in);
        Bytes script(22,0);script[1]=20;script[2]=height;
        if(signed_history && height==1)script=signed_script;
        coinbase.vout.emplace_back(ConsensusSubsidy::GetBlockSubsidy(height),script);
        body.vtx.push_back(coinbase);
        if(signed_history && height==101) {
            const OutPoint funding(history[1].vtx[0].GetTxid(),0);
            const auto* coin=builder.GetCoin(funding);CHECK(coin && coin->isCoinbase);
            historical_spend=Child(funding,*coin,keys);
            body.vtx.push_back(historical_spend);
        }
        std::vector<TxId> ids;for(const auto& tx:body.vtx)ids.push_back(tx.GetTxid());
        body.header.merkle_root=ComputeTransactionMerkleRoot(ids);
        CHECK(validator.ComputeUtreexoRootPure(body,height,body.header.utreexo_root,error));
        BlockUndo undo;CHECK(validator.ConnectBlock(body,height,body.GetHash(),undo,error));
        CHECK(replay.ConnectAndAdvance(body,height,body.GetHash(),error));
        CHECK(headers.AddHeader(body.header));work+=GetBlockProof(body.header.difficulty);
        store(body,height);history.push_back(body);
    }
    const auto parent=history.back().header;const auto forest=*replay.Forest();
    CHECK(replay.UtreexoRootHex()==parent.utreexo_root.GetHex());
    ConsensusUTXOSet live;View view;view.height=origin_height;rocksdb::WriteBatch seed;
    for(const auto& [point,coin]:replay.ProvenUtxos()) {
        CHECK(live.AddCoin(point,coin));view.coins.emplace(point,coin);
        Coin stored;stored.amount=coin.value.GetUna();stored.script_pubkey=StorageScriptHex(coin.scriptPubKey);
        stored.height=coin.height;stored.coinbase=coin.isCoinbase;
        CHECK(db.putCoin(token,point.txid.AsUint256(),point.vout,stored,&seed)==Status::Ok);
    }
    live.ReplaceForestGuarded(forest);live.SetBestBlock(parent.GetHash(),origin_height);
    CHECK(db.putForestTipMarker(token,{int32_t(origin_height),parent.GetHash(),parent.utreexo_root},&seed)==Status::Ok);
    CHECK(db.putUtreexoCheckpointWithChecksum(token,origin_height,forest.serialize(),&seed)==Status::Ok);
    const auto frontier=replay.ShieldedTree()->SerializeFrontier();
    const auto anchors=replay.ShieldedAnchors()->SerializePersistenceBytes();
    CHECK(db.putShieldedState(token,ChainDB::ShieldedStateRecord::Frontier,{frontier.begin(),frontier.end()},&seed)==Status::Ok);
    CHECK(db.putShieldedState(token,ChainDB::ShieldedStateRecord::AnchorHistory,{anchors.begin(),anchors.end()},&seed)==Status::Ok);
    uint256 shielded_root;const auto root=replay.ShieldedTree()->Root();std::copy(root.begin(),root.end(),shielded_root.begin());
    CHECK(db.putShieldedTipMarker(token,{int32_t(origin_height),parent.GetHash(),shielded_root,0,0},&seed)==Status::Ok);
    CHECK(db.setTip(token,parent.GetHash(),origin_height,work,&seed)==Status::Ok);
    CHECK(db.setValidatedTip(token,parent.GetHash(),origin_height,&seed)==Status::Ok);Commit(db,seed);
    // Accounting is derived from the very history just independently replayed.
    AnnotatedRecursiveMutex activation;
    CHECK(OrchardProfileConfigurationValid(Params()));
    CHECK(Params().orchard_activation_height==activation_height && Params().shielded_activation_height==1);
    for(uint32_t height=1;height<activation_height;++height) {
        CHECK(db.getBlock(history[height].GetHash()).ok());
        CHECK(db.getBlockHeight(history[height].GetHash()).ok());
    }
    std::optional<storage::LegacyRetirementRecord> retirement;
    {std::lock_guard<AnnotatedRecursiveMutex> lock(activation);
     try { const auto accounting=DeriveSelectedLegacyPoolAccountingUnderLock(db,files.get(),activation_height);
         CHECK(accounting.blocks_read==origin_height && accounting.value_una==0);
         std::cout<<"Origin capture historical accounting PASS\n";
         retirement=DeriveSelectedLegacyRetirementUnderLock(db,files.get(),activation_height); }
     catch(const OrchardStateLookupError& e) { throw std::runtime_error("Origin fixture retirement status "+std::to_string(int(e.SourceStatus()))); }}
    CHECK(retirement->retired_value==0 && retirement->boundary_parent==parent.GetHash());
    auto context=*SelectedOrchardBlockContext(BlockHeader{},activation_height);context.parent_hash=parent.GetHash();
    std::vector<Bytes> boundary_transactions;
    if(signed_history) {
        const OutPoint change(historical_spend.GetTxid(),0);
        const auto* coin=live.GetCoin(change);CHECK(coin && !coin->isCoinbase);
        boundary_spend=Child(change,*coin,keys);
        boundary_transactions.push_back(Wire(boundary_spend));
    }
    auto bare=WithParentTiming(CandidateWires(context,boundary_transactions),parent);context.block_hash=bare.Header().GetHash();
    const auto initial=PrepareOrchardBlockCoinsUnderChainstateLock(bare,context,view,{},true);
    const auto filtered=WithFilterHash(bare,BuildOrchardBlockFilter(initial).GetHash());context.block_hash=filtered.Header().GetHash();
    const auto filtered_coins=PrepareOrchardBlockCoinsUnderChainstateLock(filtered,context,view,{},true);
    const auto next=PrepareOrchardStateTransition(context,std::nullopt,filtered_coins.Authorizations(),{
        [&](const uint256& a)->StatusOr<bool>{return db.getOrchardAnchorReferences(a).ok();},
        [&](const uint256& n)->StatusOr<bool>{return db.getOrchardNullifierOwner(n).ok();}});
    const auto sets=RequiredValue(db.previewOrchardCommitmentSets(std::nullopt,next.Next(),next.Nullifiers()));
    const auto commitment=ComputeOrchardStateRoot({context.domain,activation_height,activation_height,context.parent_hash},*retirement,next.Next(),sets);
    const auto draft=AppendStateScript(filtered,BuildStateCommitmentScript(commitment,StateCommitmentEncoding::Orchard));
    context.block_hash=draft.Header().GetHash();
    const auto coins=PrepareOrchardBlockCoinsUnderChainstateLock(draft,context,view,{},true);
    auto header=draft.Header();header.utreexo_root=PrepareOrchardForestTransition(coins,parent,forest).Root();
    auto bytes=draft.WireBytes();const auto prefix=header.SerializeForHash();std::copy(prefix.begin(),prefix.end(),bytes.begin());
    const auto boundary=WithProof(OrchardBlockCandidate::DecodeExact(bytes),MixedProof(coins,forest));
    context.block_hash=header.GetHash();CHECK(headers.AddHeader(header));work+=GetBlockProof(header.difficulty);
    CHECK(db.putHeader(token,context.block_hash,header,activation_height,work)==Status::Ok);
    ChainDB::PersistedHeaderMetadata metadata;metadata.height=activation_height;metadata.parent_hash=parent.GetHash();
    metadata.chainwork=work;metadata.status_flags=BLOCK_VALID_HEADER;
    CHECK(db.putHeaderMetadata(token,context.block_hash,metadata)==Status::Ok);
    auto index=DiskIndex(db,header,activation_height);
    auto write=PreparedOrchardChainstateWrite::ConnectIndexed(activation,db,token,*files,index,live,context,
        boundary,parent,forest,{},true,true,retirement,true);
    write->Commit();write.reset();
    ChainstateService service;service.setChainDB(&db);service.setBlockStorage(files);
    Access::Set(service,index,live.GetForest());
    WalletManager wallet(temp.path/"wallet");wallet.create("origin");
    const auto owned=history[1].vtx.front().vout.front().scriptPubKey;
    wallet.addWatchScript(owned,"m/84'/1'/0'/0/0",false);
    const auto session=wallet.AcquireDatabaseLease()->Session();
    if(signed_history) {
        dinero::UTXOIndex wallet_index((temp.path/"signed-index.sqlite").string());CHECK(wallet_index.Initialize());
        wallet_index.RegisterAddress(owned,"m/86'/1'/0'/0/0");
        const auto source=service.getRuntimeWalletOrigin(wallet,session,&wallet_index);CHECK(source.ok());
        CHECK((*source)->OriginHeight()==origin_height && (*source)->Coins().size()==2 && (*source)->Transactions().size()==2);
        auto invalid_signed=history[101];invalid_signed.vtx[1].vin[0].witness[0][0]^=1;
        CHECK(invalid_signed.GetHash()==history[101].GetHash());
        CHECK(invalid_signed.vtx[1].GetTxid()==historical_spend.GetTxid());
        CHECK(db.putBlock(token,history[101].GetHash(),invalid_signed)==Status::Ok);
        const auto invalid_capture=service.getRuntimeWalletOrigin(wallet,session,&wallet_index);
        CHECK(!invalid_capture.ok() && invalid_capture.status()==Status::Invalid);
        CHECK((*source)->Transactions().back().transaction.Serialize(TxSerializationMode::WithWitness)==Wire(historical_spend));
        CHECK(db.putBlock(token,history[101].GetHash(),history[101])==Status::Ok);
        const TxOutPoint funding{history[1].vtx[0].GetTxid(),0},change{historical_spend.GetTxid(),0};
        CHECK((*source)->Coins().at(funding).spent->txid==historical_spend.GetTxid());
        CHECK((*source)->Coins().at(funding).spent->height==101 && !(*source)->Coins().at(change).spent);
        // A genuine signed owned spend is insufficient to invent local history.
        CHECK(service.adoptRuntimeWalletOrigin(wallet,wallet_index,**source)!=Status::Ok);
        CHECK(!RuntimeIndexDelivery::ReadForWallet(wallet,wallet_index,session));
        CHECK(!RuntimeOrdinaryDelivery::ReadForWallet(wallet,session));
        const auto old_id=historical_spend.GetTxid().AsUint256().GetHex();
        const auto first_id=boundary_spend.GetTxid().AsUint256().GetHex();
        CHECK(wallet.addTransaction(old_id,"local self transfer",-0.00000123,"send",false,"origin label",123456,0));
        // Pre-origin history alone cannot account for a new owned spend in
        // event 1. Refuse before the independent index can commit a prefix.
        CHECK(service.adoptRuntimeWalletOrigin(wallet,wallet_index,**source)!=Status::Ok);
        CHECK(!RuntimeIndexDelivery::ReadForWallet(wallet,wallet_index,session));
        CHECK(!RuntimeOrdinaryDelivery::ReadForWallet(wallet,session));
        std::cout<<"OrchardOriginFirstHistory missing first-event metadata refuses before stores PASS\n";
        CHECK(wallet.addTransaction(first_id,"pending self transfer",-0.00000123,"send",false,"boundary label",123457,0));
        const auto pending_id=H(240).GetHex();
        CHECK(wallet.addTransaction(pending_id,"unconfirmed local record",-0.00000123,"send",false,"pending label",123458,0));
        // Local metadata is fixture setup through the real API; neither history
        // rows nor this test establish a durable pending/broadcast owner.
        CHECK(sqlite3_exec(wallet.getCurrentDatabase(),"CREATE TRIGGER reject_signed_spend BEFORE UPDATE OF is_spent ON utxos WHEN NEW.spent_height=103 BEGIN SELECT RAISE(ABORT,'signed event failure'); END",nullptr,nullptr,nullptr)==SQLITE_OK);
        CHECK(service.adoptRuntimeWalletOrigin(wallet,wallet_index,**source)!=Status::Ok);
        CHECK(RuntimeIndexDelivery::ReadForWallet(wallet,wallet_index,session)->cursor==(*source)->FirstCursor());
        CHECK(!RuntimeOrdinaryDelivery::ReadForWallet(wallet,session));
        auto scalar=[&](const std::string& sql) {
            sqlite3_stmt* q=nullptr;CHECK(sqlite3_prepare_v2(wallet.getCurrentDatabase(),sql.c_str(),-1,&q,nullptr)==SQLITE_OK);
            CHECK(sqlite3_step(q)==SQLITE_ROW);const auto value=sqlite3_column_int64(q,0);CHECK(sqlite3_step(q)==SQLITE_DONE);sqlite3_finalize(q);return value;
        };
        CHECK(scalar("SELECT COUNT(*) FROM utxos")==0);
        CHECK(scalar("SELECT SUM(height) FROM transactions")==0);
        CHECK(sqlite3_exec(wallet.getCurrentDatabase(),"DROP TRIGGER reject_signed_spend",nullptr,nullptr,nullptr)==SQLITE_OK);
        wallet.open("origin");const auto current_session=wallet.AcquireDatabaseLease()->Session();
        const auto retry=service.getRuntimeWalletOrigin(wallet,current_session,&wallet_index);CHECK(retry.ok());
        CHECK(service.adoptRuntimeWalletOrigin(wallet,wallet_index,**retry)==Status::Ok);
        CHECK(service.adoptRuntimeWalletOrigin(wallet,wallet_index,**retry)==Status::Ok);
        auto metadata=[&](const std::string& id,const char* address,const char* label,int64_t time,uint32_t height) {
            sqlite3_stmt* q=nullptr;CHECK(sqlite3_prepare_v2(wallet.getCurrentDatabase(),"SELECT address,amount,category,label,time,height FROM transactions WHERE txid=?",-1,&q,nullptr)==SQLITE_OK);
            CHECK(sqlite3_bind_text(q,1,id.c_str(),-1,SQLITE_TRANSIENT)==SQLITE_OK);
            if(sqlite3_step(q)!=SQLITE_ROW) {sqlite3_finalize(q);throw std::runtime_error("Origin signed spend metadata missing at height "+std::to_string(height));}
            const auto text=[&](int n){const auto* p=sqlite3_column_text(q,n);return p?std::string(reinterpret_cast<const char*>(p)):std::string();};
            CHECK(text(0)==address && sqlite3_column_double(q,1)==-0.00000123 && text(2)=="send" && text(3)==label);
            CHECK(sqlite3_column_int64(q,4)==time && sqlite3_column_int64(q,5)==height);CHECK(sqlite3_step(q)==SQLITE_DONE);sqlite3_finalize(q);
        };
        metadata(old_id,"local self transfer","origin label",123456,101);
        metadata(first_id,"pending self transfer","boundary label",123457,activation_height);
        CHECK(scalar("SELECT COUNT(*) FROM utxos WHERE is_spent=1")==2);
        CHECK(scalar("SELECT COUNT(*) FROM utxos WHERE spent_height=101 AND spent_txid='"+old_id+"'")==1);
        CHECK(scalar("SELECT COUNT(*) FROM utxos WHERE spent_height=103 AND spent_txid='"+first_id+"'")==1);
        CHECK(scalar("SELECT COUNT(*) FROM transactions WHERE category='receive'")==0);
        // Undo the actual canonical boundary, then consume its checked outbox
        // event. Origin history stays confirmed; boundary local metadata must
        // survive unconfirmation without claiming mempool acceptance.
        auto undo=PreparedOrchardChainstateWrite::DisconnectIndexed(activation,db,token,*files,index,live,context,boundary,parent,live.GetForest(),true);
        undo->Commit();undo.reset();
        const auto read_page=[&](RuntimeOutboxCursor cursor) {std::lock_guard<AnnotatedRecursiveMutex> guard(activation);return ReadRuntimeOutboxUnderLock(db,context,cursor,1);};
        const auto page=read_page((*retry)->FirstCursor());CHECK(page.events.size()==1);
        const auto event=page.events[0];CHECK(event.direction==RuntimeBlockDirection::Disconnect);
        RuntimeIndexDelivery::ApplyForWallet(wallet,wallet_index,current_session,event);
        // Unconfirmation and coin undo must roll back together if SQL fails,
        // while the separately committed index prefix remains retryable.
        CHECK(sqlite3_exec(wallet.getCurrentDatabase(),"CREATE TRIGGER reject_unconfirm BEFORE UPDATE OF height ON transactions WHEN OLD.height=103 AND NEW.height=0 BEGIN SELECT RAISE(ABORT,'unconfirmation failure'); END",nullptr,nullptr,nullptr)==SQLITE_OK);
        bool refused=false;try{RuntimeOrdinaryDelivery::ApplyForWallet(wallet,current_session,event);}catch(const std::runtime_error&){refused=true;}
        CHECK(refused);
        CHECK(RuntimeOrdinaryDelivery::ReadForWallet(wallet,current_session)->cursor==(*retry)->FirstCursor());
        CHECK(scalar("SELECT COUNT(*) FROM utxos WHERE is_spent=1")==2);
        metadata(first_id,"pending self transfer","boundary label",123457,activation_height);
        CHECK(sqlite3_exec(wallet.getCurrentDatabase(),"DROP TRIGGER reject_unconfirm",nullptr,nullptr,nullptr)==SQLITE_OK);
        wallet.open("origin");const auto undo_session=wallet.AcquireDatabaseLease()->Session();
        RuntimeIndexDelivery::ApplyForWallet(wallet,wallet_index,undo_session,event);
        RuntimeOrdinaryDelivery::ApplyForWallet(wallet,undo_session,event);
        metadata(old_id,"local self transfer","origin label",123456,101);
        metadata(first_id,"pending self transfer","boundary label",123457,0);
        CHECK(scalar("SELECT COUNT(*) FROM utxos WHERE is_spent=1")==1);
        CHECK(scalar("SELECT COUNT(*) FROM utxos WHERE txid='"+old_id+"' AND is_spent=0 AND spent_txid IS NULL AND spent_height IS NULL")==1);
        CHECK(scalar("SELECT COUNT(*) FROM utxos WHERE txid='"+first_id+"'")==0);
        CHECK(RuntimeOrdinaryDelivery::ReadForWallet(wallet,undo_session)->cursor==event.cursor);
        metadata(pending_id,"unconfirmed local record","pending label",123458,0);
        CHECK(!wallet_index.GetUTXO(historical_spend.GetTxid(),0)->spend_height);
        CHECK(!wallet_index.GetUTXO(boundary_spend.GetTxid(),0));
        auto reconnect=PreparedOrchardChainstateWrite::ConnectIndexed(activation,db,token,*files,index,live,context,
            boundary,parent,live.GetForest(),{},true,true,retirement,true);
        reconnect->Commit();reconnect.reset();
        const auto again_page=read_page(event.cursor);CHECK(again_page.events.size()==1);
        const auto again=again_page.events[0];CHECK(again.direction==RuntimeBlockDirection::Connect);
        RuntimeIndexDelivery::ApplyForWallet(wallet,wallet_index,undo_session,again);
        RuntimeOrdinaryDelivery::ApplyForWallet(wallet,undo_session,again);
        metadata(old_id,"local self transfer","origin label",123456,101);
        metadata(first_id,"pending self transfer","boundary label",123457,activation_height);
        metadata(pending_id,"unconfirmed local record","pending label",123458,0);
        CHECK(RuntimeOrdinaryDelivery::ReadForWallet(wallet,undo_session)->cursor==again.cursor);
        CHECK(scalar("SELECT COUNT(*) FROM utxos WHERE is_spent=1")==2);
        std::cout<<"OrchardOriginSignedSpends consensus history/first event/metadata/rollback/undo PASS\n";
        return;
    }
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
