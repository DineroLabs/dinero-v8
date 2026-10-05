#pragma once
#ifdef DINERO_TEST_ORCHARD_ORIGIN
namespace dinero {
namespace {
struct ShieldHistoryFixture:ShieldReservationFixture {
    void Complete(){auto use=WalletService::AcquireWalletUse(wallet);auto& jobs=use->OrchardProofs();
        Need(QueueSelected(jobs,Account(3).revision,Inputs())->enqueued);
        Need(ServiceProofTerminal(jobs,orchard::Hash{81})==wallet::OrchardProofJobs::State::Succeeded);}
    auto FinishOwned(){auto use=WalletService::AcquireWalletUse(wallet);return Finish(use->OrchardProofs());}
    std::string Txid(const std::vector<uint8_t>& body){const auto wire=orchard::TransactionEnvelope::DecodeExact(body).Txid();uint256 id;
        std::copy(wire.begin(),wire.end(),id.begin());return id.GetHex();}
    std::vector<std::string> History(const std::string& id){auto use=WalletService::AcquireWalletUse(wallet);auto lease=use->Wallet().AcquireDatabaseLease();
        struct Statement{sqlite3_stmt* p=nullptr;~Statement(){sqlite3_finalize(p);}} q;
        Need(sqlite3_prepare_v2(lease->Database(),"SELECT address,amount,category,label,time,height,confirmations FROM transactions WHERE txid=?",-1,&q.p,nullptr)==SQLITE_OK);
        Need(sqlite3_bind_text(q.p,1,id.data(),int(id.size()),SQLITE_TRANSIENT)==SQLITE_OK);Need(sqlite3_step(q.p)==SQLITE_ROW);
        std::vector<std::string> row;for(int i=0;i<7;++i){const auto* value=static_cast<const char*>(sqlite3_column_blob(q.p,i));const int n=sqlite3_column_bytes(q.p,i);
            Need(n>=0&&(value||!n));row.emplace_back(n?std::string(value,size_t(n)):std::string());}
        Need(sqlite3_step(q.p)==SQLITE_DONE);return row;}
};
}
TEST(WalletShieldHistory, ReadyHistoryAndTimestampSurviveReopenMiningAndRewind){
    ShieldHistoryFixture f;f.Complete();const auto body=f.FinishOwned();const auto id=f.Txid(body);
    const auto entry=f.Account(3).account.Operations().Entries().at(orchard::Hash{81});ASSERT_TRUE(entry.shield_ready_time);
    EXPECT_GT(*entry.shield_ready_time,0u);const auto row=f.History(id);
    EXPECT_EQ(row[0],f.Payments()[0].recipient.EncodeAddress(orchard::WalletNetwork::Regtest));
    EXPECT_DOUBLE_EQ(std::stod(row[1]),-0.00030);EXPECT_EQ(row[2],"shield");EXPECT_TRUE(row[3].empty());
    EXPECT_EQ(std::stoull(row[4]),*entry.shield_ready_time);EXPECT_EQ(row[5],"0");EXPECT_EQ(row[6],"0");
    const auto bytes=ShieldQueueBytes(f.Account(3).account.Operations());ASSERT_EQ(bytes[7],'4');
    EXPECT_EQ(ShieldQueueBytes(wallet::OrchardOperationQueue::Restore(orchard::WalletStateBytes(bytes),f.Domain())),bytes);
    for(size_t size=bytes.size()-9;size<bytes.size();++size){const std::vector<uint8_t> bad(bytes.begin(),bytes.begin()+size);
        EXPECT_THROW(wallet::OrchardOperationQueue::Restore(orchard::WalletStateBytes(bad),f.Domain()),std::runtime_error);}
    for(int mode:{0,1,2}){auto bad=bytes;
        if(mode==0)bad[bad.size()-9]=2;
        if(mode==1)std::fill(bad.end()-8,bad.end(),0);
        if(mode==2)bad.back()|=0x80;
        EXPECT_THROW(wallet::OrchardOperationQueue::Restore(orchard::WalletStateBytes(bad),f.Domain()),std::runtime_error);}
    const auto ready=f.Snapshot();f.Reopen();ASSERT_TRUE(f.wallet->EnsureRuntimeWalletBindings());
    EXPECT_EQ(f.FinishOwned(),body);EXPECT_EQ(f.Snapshot(),ready);EXPECT_EQ(f.History(id),row);
    const auto envelope=orchard::TransactionEnvelope::DecodeExact(body);ASSERT_TRUE(f.Mine(MempoolTransaction::FromOrchard(envelope)));
    ASSERT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(),Recovery::AppliedPrefix);
    const auto confirmed=f.History(id);for(size_t i=0;i<5;++i)EXPECT_EQ(confirmed[i],row[i]);EXPECT_GT(std::stoull(confirmed[5]),0u);
    f.ArchiveConfirmedShield();
    const auto archived=f.Stored(f.Payments());ASSERT_TRUE(archived&&archived->archived&&archived->durable);
    EXPECT_EQ(archived->durable->shield_ready_time,entry.shield_ready_time);
    // The legacy standalone history rewind must also retain this local origin.
    ASSERT_TRUE(f.wallet->get().removeTransactionsAtHeight(uint32_t(std::stoul(confirmed[5]))));
    const auto rewound=f.History(id);for(size_t i=0;i<5;++i)EXPECT_EQ(rewound[i],row[i]);EXPECT_EQ(rewound[5],"0");EXPECT_EQ(rewound[6],"0");
}
TEST(WalletShieldHistory, HistoryWriteAndReadyCommitRefusalsKeepProofAndNoPartialRows){
    ShieldHistoryFixture f;f.Complete();const auto before=f.Snapshot();
    f.Sql("CREATE TRIGGER refuse_shield_history BEFORE INSERT ON transactions WHEN NEW.category='shield' BEGIN SELECT RAISE(ABORT,'shield history refusal'); END");
    EXPECT_THROW(f.FinishOwned(),std::runtime_error);f.Sql("DROP TRIGGER refuse_shield_history");EXPECT_EQ(f.Snapshot(),before);
    auto* db=f.wallet->get().getCurrentDatabase();struct Fault{bool history=false;int commits=0;} fault;
    sqlite3_set_authorizer(db,[](void* p,int action,const char* table,const char*,const char*,const char*){
        if(action==SQLITE_INSERT&&table&&std::string_view(table)=="transactions")static_cast<Fault*>(p)->history=true;
        return SQLITE_OK;},&fault);
    sqlite3_commit_hook(db,[](void* p){auto& f=*static_cast<Fault*>(p);if(!f.history)return 0;++f.commits;return 1;},&fault);
    EXPECT_THROW(f.FinishOwned(),std::runtime_error);sqlite3_commit_hook(db,nullptr,nullptr);sqlite3_set_authorizer(db,nullptr,nullptr);
    EXPECT_EQ(fault.commits,1);EXPECT_TRUE(sqlite3_get_autocommit(db));EXPECT_EQ(f.Snapshot(),before);
    {auto use=WalletService::AcquireWalletUse(f.wallet);EXPECT_EQ(use->OrchardProofs().Query(orchard::Hash{81}),wallet::OrchardProofJobs::State::Succeeded);}
    const auto body=f.FinishOwned();EXPECT_EQ(f.History(f.Txid(body))[2],"shield");
}
TEST(WalletShieldHistory, AlteredMissingAndLegacyReadyHistoryRefuseWithoutReconstruction){
    ShieldHistoryFixture f;f.Complete();const auto body=f.FinishOwned();const auto id=f.Txid(body);const auto history=f.History(id);
    f.Sql("CREATE TEMP TABLE saved_shield_history AS SELECT * FROM transactions WHERE category='shield'");
    for(const char* field:{"amount=0","address='wrong'","category='send'","label='changed'","time=time+1"}){
        const auto sql=std::string("UPDATE transactions SET ")+field+" WHERE txid='"+id+"'";f.Sql(sql.c_str());const auto bad=f.Snapshot();
        EXPECT_THROW(f.Account(3),std::runtime_error);
        EXPECT_THROW(f.FinishOwned(),std::runtime_error);EXPECT_EQ(f.Snapshot(),bad);
        const auto restore="DELETE FROM transactions WHERE txid='"+id+"';INSERT INTO transactions SELECT * FROM saved_shield_history";f.Sql(restore.c_str());
        EXPECT_EQ(f.History(id),history);
    }
    f.Sql(("DELETE FROM transactions WHERE txid='"+id+"'").c_str());const auto missing=f.Snapshot();
    EXPECT_THROW(f.Stored(f.Payments()),std::runtime_error);
    EXPECT_THROW(f.FinishOwned(),std::runtime_error);EXPECT_EQ(f.Snapshot(),missing);
    f.Sql("INSERT INTO transactions SELECT * FROM saved_shield_history");
    // A separate pristine fixture isolates old Ready-format refusal from the
    // ordinary receipt invalidation caused by the earlier tampering cases.
    ShieldHistoryFixture old_fixture;old_fixture.Complete();const auto old_body=old_fixture.FinishOwned();
    const auto old_id=old_fixture.Txid(old_body);const auto old_history=old_fixture.History(old_id);
    auto legacy=ShieldQueueBytes(old_fixture.Account(3).account.Operations());legacy.resize(legacy.size()-9);legacy[7]='3';
    EXPECT_NO_THROW(wallet::OrchardOperationQueue::Restore(orchard::WalletStateBytes(legacy),old_fixture.Domain()));
    old_fixture.StoreQueueFixture(legacy);const auto old=old_fixture.Snapshot();
    EXPECT_THROW(old_fixture.FinishOwned(),std::runtime_error);EXPECT_EQ(old_fixture.Snapshot(),old);EXPECT_EQ(old_fixture.History(old_id),old_history);
}
} // namespace dinero
#endif
