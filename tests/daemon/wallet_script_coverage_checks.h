#pragma once
#include "wallet/runtime_origin_projection.h"
#ifdef DINERO_TEST_ORCHARD_ORIGIN
namespace dinero {
namespace {
template<class Fixture> auto CaptureScriptCoverage(Fixture& f) {
    auto use=WalletService::AcquireWalletUse(f.wallet);
    auto index=ChainstateService::AcquireWalletIndexUse(f.f.service);
    return f.f.service->getRuntimeWalletCoverage(use->Wallet(),f.Selected().session,index->Index());
}
template<class Fixture> std::string CoverageIndexSnapshot(Fixture& f) {
    struct Database {sqlite3* value=nullptr;~Database(){if(value)sqlite3_close(value);}} db;
    const auto path=f.f.path/"wallet-runtime"/"blockchain"/"utxo";
    SharedPaymentFixture::Need(sqlite3_open_v2(path.string().c_str(),&db.value,SQLITE_OPEN_READONLY,nullptr)==SQLITE_OK);
    struct Statement {sqlite3_stmt* value=nullptr;~Statement(){sqlite3_finalize(value);}} tables;
    SharedPaymentFixture::Need(sqlite3_prepare_v2(db.value,"SELECT name,sql FROM sqlite_master WHERE type='table' AND name NOT LIKE 'sqlite_%' ORDER BY name",-1,&tables.value,nullptr)==SQLITE_OK);
    std::string snapshot;int rc;
    while((rc=sqlite3_step(tables.value))==SQLITE_ROW) {
        const auto name=std::string(reinterpret_cast<const char*>(sqlite3_column_text(tables.value,0)));
        const auto schema=std::string(reinterpret_cast<const char*>(sqlite3_column_text(tables.value,1)));
        std::string quoted="\"";for(char c:name){quoted+=c;if(c=='\"')quoted+='\"';}quoted+='\"';
        Statement rows;const auto sql="SELECT * FROM "+quoted;
        SharedPaymentFixture::Need(sqlite3_prepare_v2(db.value,sql.c_str(),-1,&rows.value,nullptr)==SQLITE_OK);
        std::vector<std::string> values;int row_rc;
        while((row_rc=sqlite3_step(rows.value))==SQLITE_ROW) {
            std::string row;
            for(int col=0;col<sqlite3_column_count(rows.value);++col) {
                row+=std::to_string(sqlite3_column_type(rows.value,col))+":";
                const auto* bytes=static_cast<const char*>(sqlite3_column_blob(rows.value,col));const int size=sqlite3_column_bytes(rows.value,col);
                SharedPaymentFixture::Need(size>=0&&(bytes||size==0));row+=std::to_string(size)+":";if(size)row.append(bytes,size);
            }
            values.push_back(std::move(row));
        }
        SharedPaymentFixture::Need(row_rc==SQLITE_DONE);std::sort(values.begin(),values.end());snapshot+=schema+"\n";
        for(const auto& row:values)snapshot+=std::to_string(row.size())+":"+row;
    }
    SharedPaymentFixture::Need(rc==SQLITE_DONE);return snapshot;
}
}
TEST(WalletScriptReconciliation, ExpandedScriptDomainPreservesPendingAndAccountOwners) {
    SharedPaymentFixture f;f.Create(3);f.Create(17);const auto payment=f.Pay();ASSERT_TRUE(payment.success)<<payment.error;
    const auto pending=f.wallet->get().getPendingPayments();ASSERT_EQ(pending.size(),1u);
    const auto account=OrchardCreationBytes(f.Account(17).account);const auto stale=CaptureScriptCoverage(f);ASSERT_TRUE(stale.ok());
    (void)f.wallet->get().getNewChangeAddress("","taproot");const auto before=f.Snapshot();
    auto use=WalletService::AcquireWalletUse(f.wallet);auto index=ChainstateService::AcquireWalletIndexUse(f.f.service);
    EXPECT_TRUE(RuntimeOrdinaryDelivery::CoverageRequiredForWallet(use->Wallet(),index->Index(),f.Selected().session));
    EXPECT_NE(f.f.service->reconcileRuntimeWalletCoverage(use->Wallet(),index->Index(),**stale),Status::Ok);
    EXPECT_EQ(f.Snapshot(),before);
    const auto source=f.f.service->getRuntimeWalletCoverage(use->Wallet(),f.Selected().session,index->Index());ASSERT_TRUE(source.ok());
    ASSERT_EQ(f.f.service->reconcileRuntimeWalletCoverage(use->Wallet(),index->Index(),**source),Status::Ok);
    EXPECT_FALSE(RuntimeOrdinaryDelivery::CoverageRequiredForWallet(use->Wallet(),index->Index(),f.Selected().session));
    const auto ordinary=RuntimeOrdinaryDelivery::ReadForWallet(use->Wallet(),f.Selected().session);
    const auto indexed=RuntimeIndexDelivery::ReadForWallet(use->Wallet(),index->Index(),f.Selected().session);
    ASSERT_TRUE(ordinary);ASSERT_TRUE(indexed);EXPECT_EQ(ordinary->cursor,(*source)->Head());EXPECT_EQ(indexed->cursor,ordinary->cursor);
    const auto after=use->Wallet().getPendingPayments();ASSERT_EQ(after.size(),1u);
    EXPECT_EQ(after[0].signed_body,pending[0].signed_body);EXPECT_EQ(after[0].intent,pending[0].intent);
    EXPECT_EQ(after[0].created_at,pending[0].created_at);EXPECT_EQ(after[0].fee_una,pending[0].fee_una);
    EXPECT_EQ(OrchardCreationBytes(f.Account(17).account),account);
    const auto reconciled=f.Snapshot();ASSERT_EQ(f.f.service->reconcileRuntimeWalletCoverage(use->Wallet(),index->Index(),**source),Status::Ok);
    EXPECT_EQ(f.Snapshot(),reconciled);
}
TEST(WalletScriptReconciliation, PreparationCommitRefusalLeavesBothStoresUnchanged) {
    SharedPaymentFixture f;(void)f.wallet->get().getNewChangeAddress("","taproot");
    const auto source=CaptureScriptCoverage(f);ASSERT_TRUE(source.ok());
    const auto before=f.Snapshot(),index_before=CoverageIndexSnapshot(f);
    auto use=WalletService::AcquireWalletUse(f.wallet);auto index=ChainstateService::AcquireWalletIndexUse(f.f.service);
    struct Observe {sqlite3* db;int commits=0;~Observe(){sqlite3_commit_hook(db,nullptr,nullptr);}} observed{use->Wallet().getCurrentDatabase()};
    sqlite3_commit_hook(observed.db,[](void* p){++static_cast<Observe*>(p)->commits;return 1;},&observed);
    EXPECT_NE(f.f.service->reconcileRuntimeWalletCoverage(use->Wallet(),index->Index(),**source),Status::Ok);
    sqlite3_commit_hook(observed.db,nullptr,nullptr);
    EXPECT_EQ(observed.commits,1);EXPECT_EQ(f.Snapshot(),before);EXPECT_EQ(CoverageIndexSnapshot(f),index_before);
    EXPECT_TRUE(RuntimeOrdinaryDelivery::CoverageRequiredForWallet(use->Wallet(),index->Index(),f.Selected().session));
    const auto retry=f.f.service->getRuntimeWalletCoverage(use->Wallet(),f.Selected().session,index->Index());ASSERT_TRUE(retry.ok());
    ASSERT_EQ(f.f.service->reconcileRuntimeWalletCoverage(use->Wallet(),index->Index(),**retry),Status::Ok);
    EXPECT_FALSE(RuntimeOrdinaryDelivery::CoverageRequiredForWallet(use->Wallet(),index->Index(),f.Selected().session));
}
TEST(WalletScriptReconciliation, OrdinaryCommitRefusalRetainsIndexPrefixForRetry) {
    SharedPaymentFixture f;(void)f.wallet->get().getNewChangeAddress("","taproot");
    const auto source=CaptureScriptCoverage(f);ASSERT_TRUE(source.ok());const auto before=f.Snapshot();
    {
        auto use=WalletService::AcquireWalletUse(f.wallet);auto index=ChainstateService::AcquireWalletIndexUse(f.f.service);
        auto* db=use->Wallet().getCurrentDatabase();
        struct Observe {
            sqlite3* db;int commits=0,refusals=0;bool ordinary_receipt=false;
            ~Observe(){sqlite3_commit_hook(db,nullptr,nullptr);sqlite3_set_authorizer(db,nullptr,nullptr);}
        } observed{db};
        // Preparation now has its own checked commit. Arm the refusal only
        // when the actual ordinary receipt UPDATE is prepared after index effects.
        ASSERT_EQ(sqlite3_set_authorizer(db,[](void* p,int action,const char* table,const char* column,const char*,const char*){
            auto& state=*static_cast<Observe*>(p);
            if(action==SQLITE_UPDATE&&table&&column&&std::string(table)=="wallet_meta"&&
               std::string(column)=="runtime_ordinary_receipt")state.ordinary_receipt=true;
            return SQLITE_OK;
        },&observed),SQLITE_OK);
        sqlite3_commit_hook(db,[](void* p){auto& state=*static_cast<Observe*>(p);++state.commits;
            if(state.ordinary_receipt){++state.refusals;return 1;}return 0;},&observed);
        EXPECT_NE(f.f.service->reconcileRuntimeWalletCoverage(use->Wallet(),index->Index(),**source),Status::Ok);
        sqlite3_commit_hook(db,nullptr,nullptr);ASSERT_EQ(sqlite3_set_authorizer(db,nullptr,nullptr),SQLITE_OK);
        EXPECT_TRUE(observed.ordinary_receipt);EXPECT_EQ(observed.commits,2);EXPECT_EQ(observed.refusals,1);
        EXPECT_EQ(f.Snapshot(),before);
        const auto partial=RuntimeIndexDelivery::ReadForWallet(use->Wallet(),index->Index(),f.Selected().session);
        ASSERT_TRUE(partial);EXPECT_EQ(partial->cursor,(*source)->Head());
        EXPECT_TRUE(RuntimeOrdinaryDelivery::CoverageRequiredForWallet(use->Wallet(),index->Index(),f.Selected().session));
    }
    f.Reopen();ASSERT_TRUE(f.wallet->EnsureRuntimeWalletBindings());
    const auto retry=CaptureScriptCoverage(f);ASSERT_TRUE(retry.ok());
    auto use=WalletService::AcquireWalletUse(f.wallet);auto index=ChainstateService::AcquireWalletIndexUse(f.f.service);
    ASSERT_EQ(f.f.service->reconcileRuntimeWalletCoverage(use->Wallet(),index->Index(),**retry),Status::Ok);
    EXPECT_FALSE(RuntimeOrdinaryDelivery::CoverageRequiredForWallet(use->Wallet(),index->Index(),f.Selected().session));
}
TEST(WalletScriptReconciliation, UnknownCoinAndUnavailableKeyInventoryRefuseBeforeIndex) {
    for(bool wrong_coin:{true,false}) {
        SharedPaymentFixture f;(void)f.wallet->get().getNewChangeAddress("","taproot");
        const auto source=CaptureScriptCoverage(f);ASSERT_TRUE(source.ok());
        if(wrong_coin)f.Sql("UPDATE utxos SET amount=amount+1");
        const auto before=f.Snapshot();auto use=WalletService::AcquireWalletUse(f.wallet);auto index=ChainstateService::AcquireWalletIndexUse(f.f.service);
        const auto indexed=CoverageIndexSnapshot(f);
        auto* db=use->Wallet().getCurrentDatabase();
        if(!wrong_coin)ASSERT_EQ(sqlite3_set_authorizer(db,[](void*,int action,const char* table,const char*,const char*,const char*){
            return action==SQLITE_READ&&table&&std::string_view(table)=="address_derivation_paths"?SQLITE_DENY:SQLITE_OK;
        },nullptr),SQLITE_OK);
        EXPECT_NE(f.f.service->reconcileRuntimeWalletCoverage(use->Wallet(),index->Index(),**source),Status::Ok);
        if(!wrong_coin)ASSERT_EQ(sqlite3_set_authorizer(db,nullptr,nullptr),SQLITE_OK);
        EXPECT_EQ(f.Snapshot(),before);EXPECT_EQ(CoverageIndexSnapshot(f),indexed);
    }
}
TEST(WalletScriptReconciliation, NormalRecoveryReconcilesNewScriptAndPreservesReadyHistory) {
    ShieldRpcFixture f;const auto request=f.Request();const auto queued=f.QueueRpc(request);ASSERT_FALSE(queued.isMember("error"))<<queued.toStyledString();
    f.Complete();const auto finished=f.FinishRpc(request);ASSERT_FALSE(finished.isMember("error"))<<finished.toStyledString();
    const auto body=f.broadcast_body;ASSERT_FALSE(body.empty());
    EXPECT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(),Recovery::AppliedPrefix);
    const auto after=f.QueueRpc(request);ASSERT_FALSE(after.isMember("error"))<<after.toStyledString();EXPECT_TRUE(after["existing_request"].asBool());
    EXPECT_EQ(f.broadcast_body,body);
    const auto envelope=orchard::TransactionEnvelope::DecodeExact(body);ASSERT_TRUE(f.Mine(MempoolTransaction::FromOrchard(envelope)));
    EXPECT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(),Recovery::AppliedPrefix);
    EXPECT_EQ(f.Account(17).account.Scan().BalanceUna(),20000u);
    f.Reopen();ASSERT_TRUE(f.wallet->EnsureRuntimeWalletBindings());
    EXPECT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(),Recovery::AppliedPrefix);
    EXPECT_EQ(f.Account(17).account.Scan().BalanceUna(),20000u);
}
TEST(WalletScriptReconciliation, ProvedDisconnectAndReinclusionPreserveLocalHistory) {
    ShieldHistoryFixture f;f.Complete();const auto body=f.FinishOwned();const auto id=f.Txid(body);const auto local=f.History(id);
    const auto envelope=orchard::TransactionEnvelope::DecodeExact(body);
    const auto funding=TxOutPoint{TxId(f.funding.txid),f.funding.vout};const auto change=TxOutPoint{TxId(uint256::FromHexUnsafe(id)),0};
    const auto included=f.Mine(MempoolTransaction::FromOrchard(envelope));ASSERT_TRUE(included);
    (void)f.wallet->get().getNewChangeAddress("","taproot");
    ASSERT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(),Recovery::AppliedPrefix);
    const auto confirmed=f.History(id);for(size_t i=0;i<5;++i)EXPECT_EQ(confirmed[i],local[i]);EXPECT_GT(std::stoull(confirmed[5]),0u);
    f.Disconnect();(void)f.wallet->get().getNewChangeAddress("","taproot");
    const auto branch=CaptureScriptCoverage(f);ASSERT_TRUE(branch.ok());
    EXPECT_FALSE((*branch)->Coins().contains(change));EXPECT_FALSE((*branch)->Coins().at(funding).spent);
    ASSERT_TRUE((*branch)->ObservedCoins().contains(change));ASSERT_TRUE((*branch)->ObservedTransactions().contains(change.txid));
    ASSERT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(),Recovery::AppliedPrefix);
    const auto unconfirmed=f.History(id);for(size_t i=0;i<5;++i)EXPECT_EQ(unconfirmed[i],local[i]);
    EXPECT_EQ(unconfirmed[5],"0");EXPECT_EQ(unconfirmed[6],"0");EXPECT_EQ(f.Account(17).account.Scan().BalanceUna(),0u);
    {
        auto index=ChainstateService::AcquireWalletIndexUse(f.f.service);
        EXPECT_FALSE(index->Index().GetUTXO(change.txid,change.vout));
        const auto coin=index->Index().GetUTXO(funding.txid,funding.vout);ASSERT_TRUE(coin);EXPECT_FALSE(coin->spend_height);
    }
    f.Reopen();ASSERT_TRUE(f.wallet->EnsureRuntimeWalletBindings());
    ASSERT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(),Recovery::AppliedPrefix);EXPECT_EQ(f.History(id),unconfirmed);
    // Direct fixture disconnect keeps the original block eligible. Reinclude
    // its retained body explicitly instead of mining an equal-work competitor.
    const auto restored=f.Submit(included->Orchard().WireBytes());ASSERT_TRUE(restored.accepted()&&restored.connected)<<restored.reason;
    ASSERT_EQ(f.f.service->GetActiveTip()->hash,included->Orchard().Header().GetHash());
    (void)f.wallet->get().getNewChangeAddress("","taproot");
    ASSERT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(),Recovery::AppliedPrefix);
    const auto reconfirmed=f.History(id);for(size_t i=0;i<5;++i)EXPECT_EQ(reconfirmed[i],local[i]);
    EXPECT_GT(std::stoull(reconfirmed[5]),0u);EXPECT_EQ(f.Account(17).account.Scan().BalanceUna(),20000u);
}
TEST(WalletScriptReconciliation, MissingOriginatedHistoryRefusesWithoutChangingEitherStore) {
    ShieldHistoryFixture f;f.Complete();const auto body=f.FinishOwned();const auto id=f.Txid(body);
    const auto envelope=orchard::TransactionEnvelope::DecodeExact(body);ASSERT_TRUE(f.Mine(MempoolTransaction::FromOrchard(envelope)));
    (void)f.wallet->get().getNewChangeAddress("","taproot");
    f.Sql(("DELETE FROM transactions WHERE txid='"+id+"'").c_str());const auto before=f.Snapshot();const auto indexed=CoverageIndexSnapshot(f);
    EXPECT_NE(f.wallet->RecoverActiveWalletFromCanonicalSource(),Recovery::AppliedPrefix);
    EXPECT_EQ(f.Snapshot(),before);EXPECT_EQ(CoverageIndexSnapshot(f),indexed);
}
TEST(WalletScriptReconciliation, CurrentOrchardReservationRequiresProvedSourceCoin) {
    for(bool unknown:{false,true}) {
        SharedPaymentFixture f;f.Create(3);f.Create(17);
        if(unknown)f.funding.txid=uint256::FromHexUnsafe(std::string(64,'e'));
        // An authenticated component-level reservation is not itself proof
        // that this selected source ever contained its claimed input.
        f.ReserveFunding(17);const auto account=OrchardCreationBytes(f.Account(17).account);
        (void)f.wallet->get().getNewChangeAddress("","taproot");
        const auto source=CaptureScriptCoverage(f);ASSERT_TRUE(source.ok());
        const auto point=TxOutPoint{TxId(f.funding.txid),f.funding.vout};
        EXPECT_EQ((*source)->ObservedCoins().contains(point),!unknown);
        const auto before=f.Snapshot(),indexed=CoverageIndexSnapshot(f);
        auto use=WalletService::AcquireWalletUse(f.wallet);auto index=ChainstateService::AcquireWalletIndexUse(f.f.service);
        const auto status=f.f.service->reconcileRuntimeWalletCoverage(use->Wallet(),index->Index(),**source);
        if(unknown){EXPECT_NE(status,Status::Ok);EXPECT_EQ(f.Snapshot(),before);EXPECT_EQ(CoverageIndexSnapshot(f),indexed);}
        else EXPECT_EQ(status,Status::Ok);
        EXPECT_EQ(OrchardCreationBytes(f.Account(17).account),account);
    }
}
TEST(WalletScriptCoverage, CatalogInventoryUsesCallerSnapshotWithoutCommit) {
    SharedPaymentFixture f;const auto view=f.View();const auto session=f.Selected().session;
    const auto before=f.Snapshot();
    auto& manager=f.wallet->get();
    EXPECT_THROW(wallet::OrchardAccountDelivery::ReadCatalogForReplayInTransaction(manager,session,*view),std::exception);
    {
        auto lease=manager.AcquireDatabaseLease();auto* db=lease->Database();
        ASSERT_EQ(sqlite3_exec(db,"BEGIN IMMEDIATE",nullptr,nullptr,nullptr),SQLITE_OK);
        const auto changes=sqlite3_total_changes64(db);
        const auto inventory=wallet::OrchardAccountDelivery::ReadCatalogForReplayInTransaction(manager,session,*view);
        EXPECT_TRUE(inventory.catalog.generated);
        EXPECT_EQ(inventory.accounts.size(),inventory.catalog.accounts.size());
        EXPECT_FALSE(sqlite3_get_autocommit(db));EXPECT_EQ(sqlite3_total_changes64(db),changes);
        // The original owning entry point still refuses an outer transaction.
        EXPECT_THROW(wallet::OrchardAccountDelivery::ReadCatalogForReplay(manager,session,*view),std::exception);
        EXPECT_FALSE(sqlite3_get_autocommit(db));
        EXPECT_THROW(wallet::OrchardAccountDelivery::ReadCatalogForReplayInTransaction(manager,session+1,*view),std::exception);
        EXPECT_FALSE(sqlite3_get_autocommit(db));EXPECT_EQ(sqlite3_total_changes64(db),changes);
        ASSERT_EQ(sqlite3_exec(db,"COMMIT",nullptr,nullptr,nullptr),SQLITE_OK);
    }
    EXPECT_EQ(f.Snapshot(),before);
}
TEST(WalletScriptCoverage, CatalogReadRefusalLeavesCallerTransactionForRetry) {
    SharedPaymentFixture f;const auto view=f.View();const auto session=f.Selected().session;
    const auto before=f.Snapshot();
    {
        auto lease=f.wallet->get().AcquireDatabaseLease();auto* db=lease->Database();
        ASSERT_EQ(sqlite3_exec(db,"BEGIN IMMEDIATE",nullptr,nullptr,nullptr),SQLITE_OK);
        const auto changes=sqlite3_total_changes64(db);
        // A normal SQLite read denial, with no synchronization removed.
        ASSERT_EQ(sqlite3_set_authorizer(db,[](void*,int action,const char* table,const char*,const char*,const char*){
            return action==SQLITE_READ&&table&&std::string_view(table)=="wallet_meta"?SQLITE_DENY:SQLITE_OK;
        },nullptr),SQLITE_OK);
        EXPECT_THROW(wallet::OrchardAccountDelivery::ReadCatalogForReplayInTransaction(f.wallet->get(),session,*view),std::exception);
        EXPECT_FALSE(sqlite3_get_autocommit(db));EXPECT_EQ(sqlite3_total_changes64(db),changes);
        ASSERT_EQ(sqlite3_set_authorizer(db,nullptr,nullptr),SQLITE_OK);
        EXPECT_NO_THROW(wallet::OrchardAccountDelivery::ReadCatalogForReplayInTransaction(f.wallet->get(),session,*view));
        EXPECT_FALSE(sqlite3_get_autocommit(db));EXPECT_EQ(sqlite3_total_changes64(db),changes);
        ASSERT_EQ(sqlite3_exec(db,"ROLLBACK",nullptr,nullptr,nullptr),SQLITE_OK);
    }
    EXPECT_EQ(f.Snapshot(),before);
}
TEST(WalletScriptCoverage, NewChangeScriptGetsReadOnlyCoverageThroughCurrentHead) {
    SharedPaymentFixture f;const auto before=f.Snapshot();
    const auto old=CaptureScriptCoverage(f);ASSERT_TRUE(old.ok());EXPECT_EQ(f.Snapshot(),before);
    const auto point=TxOutPoint{TxId(f.funding.txid),f.funding.vout};
    ASSERT_TRUE((*old)->Coins().count(point));EXPECT_FALSE((*old)->Coins().at(point).spent);
    const auto address=f.wallet->get().getNewChangeAddress("","taproot");
    const auto decoded=DecodeWitnessAddress(address,"rdin");ASSERT_TRUE(decoded.is_valid);
    const auto changed=f.Snapshot();ASSERT_NE(changed,before);
    const auto current=CaptureScriptCoverage(f);ASSERT_TRUE(current.ok());
    EXPECT_EQ((*current)->Head(),f.View()->Head());EXPECT_EQ((*current)->TipHeight(),102u);
    EXPECT_EQ((*current)->TipHash(),f.f.service->GetActiveTip()->hash);EXPECT_EQ(f.Snapshot(),changed);
    EXPECT_EQ((*current)->Coins().at(point).output.scriptPubKey,f.script);
    for(const auto& [outpoint,coin]:(*current)->Coins())EXPECT_NE(coin.output.scriptPubKey,decoded.script_pubkey);
    // A detached source projection neither installs a receipt nor touches any
    // keys, pending history or account rows; reopen still requires recovery.
    f.Reopen();ASSERT_TRUE(f.wallet->EnsureRuntimeWalletBindings());const auto reopened=f.Snapshot();
    const auto after=CaptureScriptCoverage(f);ASSERT_TRUE(after.ok());EXPECT_EQ(f.Snapshot(),reopened);
    EXPECT_EQ((*after)->Head(),(*current)->Head());EXPECT_EQ((*after)->Coins().size(),(*current)->Coins().size());
}
TEST(WalletScriptCoverage, OrchardSpendDisconnectAndReconnectPreserveExactFacts) {
    ShieldHistoryFixture f;f.Complete();const auto body=f.FinishOwned();const auto id=TxId(uint256::FromHexUnsafe(f.Txid(body)));
    const auto funding=TxOutPoint{TxId(f.funding.txid),f.funding.vout};const auto change=TxOutPoint{id,0};
    const auto before=CaptureScriptCoverage(f);ASSERT_TRUE(before.ok());EXPECT_FALSE((*before)->Coins().at(funding).spent);
    const auto envelope=orchard::TransactionEnvelope::DecodeExact(body);
    const auto included=f.Mine(MempoolTransaction::FromOrchard(envelope));ASSERT_TRUE(included);
    const auto included_hash=included->Orchard().Header().GetHash();
    const auto selected=CaptureScriptCoverage(f);ASSERT_TRUE(selected.ok());ASSERT_TRUE((*selected)->Coins().at(funding).spent);
    EXPECT_EQ((*selected)->Coins().at(funding).spent->txid,id);EXPECT_EQ((*selected)->Coins().at(change).output.value.GetUna(),70000u);
    const auto& history=(*selected)->Transactions().at(id);EXPECT_EQ(history.inputs,std::vector<TxOutPoint>{funding});
    ASSERT_EQ(history.outputs.size(),1u);EXPECT_EQ(history.outputs[0].scriptPubKey,f.script);EXPECT_FALSE(history.coinbase);
    f.Disconnect();const auto disconnected=CaptureScriptCoverage(f);ASSERT_TRUE(disconnected.ok());
    EXPECT_FALSE((*disconnected)->Coins().at(funding).spent);EXPECT_FALSE((*disconnected)->Coins().count(change));
    EXPECT_FALSE((*disconnected)->Transactions().count(id));EXPECT_TRUE((*selected)->Coins().at(funding).spent);
    // Reconnect the exact disconnected block through ordinary block ingress.
    // The retained original remains eligible for selection after disconnect.
    const auto reattached=f.Submit(included->Orchard().WireBytes());
    ASSERT_TRUE(reattached.accepted()&&reattached.connected)<<reattached.reason;
    EXPECT_EQ(reattached.block_hash,included_hash);
    ASSERT_NE(f.f.service->GetActiveTip(),nullptr);
    ASSERT_EQ(f.f.service->GetActiveTip()->hash,included_hash);
    const auto stored=f.f.service->getRuntimeBlockByHash(included_hash);
    ASSERT_TRUE(stored.ok()&&(*stored)->IsOrchardProfile());
    const auto& block=(*stored)->Orchard();ASSERT_EQ(block.Transactions().size(),2u);
    EXPECT_EQ(block.Transactions()[1].Serialize(TxSerializationMode::WithWitness),body);
    EXPECT_EQ(block.WireBytes(),included->Orchard().WireBytes());
    EXPECT_EQ(ReadBlockRpc(f.context,included_hash.GetHex(),0).asString(),util::hex(block.WireBytes()));
    EXPECT_EQ(f.f.ingress->mempool().size(),0u);
    EXPECT_TRUE(ShieldedStateStartupTestAccess::AuditBoundary(*f.f.service));
    const auto reconnected=CaptureScriptCoverage(f);ASSERT_TRUE(reconnected.ok());
    EXPECT_GT((*reconnected)->Head().sequence,(*selected)->Head().sequence);
    EXPECT_EQ((*reconnected)->Coins().at(funding).spent->txid,id);EXPECT_EQ((*reconnected)->Coins().at(change).output.value.GetUna(),70000u);
}
TEST(WalletScriptCoverage, BorrowedOwnersWrongSessionAndDeniedInventoryRefuse) {
    SharedPaymentFixture f;const auto before=f.Snapshot();auto use=WalletService::AcquireWalletUse(f.wallet);
    auto index=ChainstateService::AcquireWalletIndexUse(f.f.service);const auto session=f.Selected().session;
    EXPECT_FALSE(f.f.service->getRuntimeWalletCoverage(use->Wallet(),session+1,index->Index()).ok());
    {auto lease=use->Wallet().AcquireDatabaseLease();
     ASSERT_EQ(sqlite3_exec(lease->Database(),"BEGIN",nullptr,nullptr,nullptr),SQLITE_OK);
     EXPECT_FALSE(f.f.service->getRuntimeWalletCoverage(use->Wallet(),session,index->Index()).ok());
     EXPECT_FALSE(sqlite3_get_autocommit(lease->Database()));
     ASSERT_EQ(sqlite3_exec(lease->Database(),"ROLLBACK",nullptr,nullptr,nullptr),SQLITE_OK);}
    {auto selected=f.f.service->AcquireBlockIngressActivationLock();
     EXPECT_FALSE(f.f.service->getRuntimeWalletCoverage(use->Wallet(),session,index->Index()).ok());}
    auto* db=use->Wallet().getCurrentDatabase();
    sqlite3_set_authorizer(db,[](void*,int action,const char* table,const char*,const char*,const char*){
        return action==SQLITE_READ&&table&&std::string_view(table)=="watch_scripts"?SQLITE_DENY:SQLITE_OK;
    },nullptr);
    const auto denied=f.f.service->getRuntimeWalletCoverage(use->Wallet(),session,index->Index());
    sqlite3_set_authorizer(db,nullptr,nullptr);EXPECT_FALSE(denied.ok());EXPECT_EQ(f.Snapshot(),before);
    const auto retry=f.f.service->getRuntimeWalletCoverage(use->Wallet(),session,index->Index());ASSERT_TRUE(retry.ok());
    EXPECT_EQ(f.Snapshot(),before);
    f.Sql("CREATE TEMP TABLE coverage_saved_identity AS SELECT runtime_delivery_id FROM wallet_meta WHERE id=1");
    f.Sql("UPDATE wallet_meta SET runtime_delivery_id=NULL WHERE id=1");const auto missing=f.Snapshot();
    EXPECT_FALSE(f.f.service->getRuntimeWalletCoverage(use->Wallet(),session,index->Index()).ok());
    EXPECT_EQ(f.Snapshot(),missing); // No lazy identity creation by a coverage read.
    f.Sql("UPDATE wallet_meta SET runtime_delivery_id=(SELECT runtime_delivery_id FROM coverage_saved_identity) WHERE id=1");
    EXPECT_EQ(f.Snapshot(),before);
}
} // namespace dinero
#endif
