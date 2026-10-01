#pragma once
#ifdef DINERO_TEST_ORCHARD_ORIGIN
namespace dinero {
namespace {
class CurrentSpendView final:public consensus::ChainStateView {
    const consensus::IConsensusUTXOSet& coins_;
public:
    explicit CurrentSpendView(const consensus::IConsensusUTXOSet& coins):coins_(coins){}
    StatusOr<consensus::UTXOEntry> getCoin(const OutPoint& point)const override {
        const auto* coin=coins_.GetCoin(point);if(!coin)return Status::NotFound;return *coin;
    }
    bool hasCoin(const OutPoint& point)const override{return coins_.HaveCoin(point);}
    uint32_t getHeight()const override{return coins_.GetHeight();}
};
struct OrchardSpendOwnerFixture : OrchardCatalogRecoveryFixture {
    OrchardSpendOwnerFixture(){
        OrchardAdmissionFixture::Require(!Call(3).isMember("error")&&!Call(17).isMember("error"));Adopt();
        const auto [body,bundle]=Shield(AccountKeys(3));(void)Mine(body);
        OrchardAdmissionFixture::Require(wallet->RecoverActiveWalletFromCanonicalSource()==Recovery::AppliedPrefix);
    }
    static orchard::Hash Operation(uint8_t n){orchard::Hash id{};id[0]=n;return id;}
    auto Reserve(uint8_t id=1,uint64_t amount=200000,bool withdraw=false){
        const auto view=f.service->getRuntimeAccountReplay();OrchardAdmissionFixture::Require(view.ok());
        const auto current=Account(3);auto use=WalletService::AcquireWalletUse(wallet);
        std::vector<orchard::WalletPayment> payments;std::vector<orchard::TransparentOutput> outputs;
        if(withdraw)outputs.push_back({amount,f.script});
        else payments.push_back({amount,AccountKeys(17).Receiver(orchard::WalletScope::External,{})});
        return wallet::OrchardAccountDelivery::ReserveCatalogSpendForReplay(use->Wallet(),Session(),
            {Domain(),102,3},current.revision,**view,Operation(id),payments,outputs,100000);
    }
    auto AuthorizeAtSelectedTip(const orchard::TransactionEnvelope& envelope){
        auto selected=f.service->AcquireBlockIngressActivationLock();
        const auto* coins=f.service->GetConsensusUTXOSet();const auto* tip=f.service->GetActiveTip();
        OrchardAdmissionFixture::Require(coins&&tip&&tip->height>=0&&coins->GetHeight()==uint32_t(tip->height));
        CurrentSpendView view(*coins);
        const auto snapshot=consensus::OrchardCoinSnapshot::ResolveUnderChainstateLock(envelope,view);
        return consensus::VerifyOrchardAuthorizations(snapshot,Domain(),uint32_t(tip->height)+1,{});
    }
    auto Prove(wallet::OrchardAccountDelivery::PreparedSpend& request){
        const auto bundle=std::move(request.plan).Prove(request.signing);
        auto envelope=orchard::TransactionEnvelope::Create(0,{},request.transparent_outputs,request.fee_una,bundle.Bytes());
        // No transparent inputs: use the real fixture coin view; it is never
        // consulted for an invented coin or amount. Locks are zero by contract.
        return AuthorizeAtSelectedTip(envelope);
    }
    auto Ready(uint8_t id,uint64_t revision,const consensus::VerifiedOrchardAuthorizations& authorization){
        const auto view=f.service->getRuntimeAccountReplay();OrchardAdmissionFixture::Require(view.ok());auto use=WalletService::AcquireWalletUse(wallet);
        return wallet::OrchardAccountDelivery::ReadyCatalogSpendForReplay(use->Wallet(),Session(),{Domain(),102,3},revision,**view,Operation(id),authorization);
    }
    sqlite3* Database(){auto use=WalletService::AcquireWalletUse(wallet);auto lease=use->Wallet().AcquireDatabaseLease();return lease->Database();}
};
}
TEST(OrchardSpendOwner, DurableReservationReadyAndActualTransferPreserveChange){
    OrchardSpendOwnerFixture f;const auto catalog=f.Catalog();const auto before=f.Account(3);auto request=f.Reserve();
    auto current=f.Account(3);ASSERT_EQ(current.revision,request.revision);ASSERT_EQ(current.account.Operations().Entries().size(),1u);
    EXPECT_EQ(current.account.Operations().Entries().at(f.Operation(1)).phase,wallet::OrchardOperationQueue::Phase::Reserved);
    EXPECT_EQ(current.account.Delivery(),before.account.Delivery());EXPECT_EQ(current.account.Scan().BalanceUna(),500000u);EXPECT_EQ(f.Catalog(),catalog);
    const auto reserved=f.Snapshot();
    EXPECT_THROW(f.Reserve(2,1),std::runtime_error);
    EXPECT_THROW(f.Reserve(1),std::runtime_error);EXPECT_EQ(f.Snapshot(),reserved);
    const auto authorization=f.Prove(request);auto ready=f.Ready(1,request.revision,authorization);
    EXPECT_EQ(ready.account.Operations().Entries().at(f.Operation(1)).transaction,authorization.Orchard().CanonicalBytes());
    const auto bytes=f.Snapshot();const auto retry=f.Ready(1,ready.revision,authorization);EXPECT_EQ(retry.revision,ready.revision);EXPECT_EQ(f.Snapshot(),bytes);
    {auto use=WalletService::AcquireWalletUse(f.wallet);use->Wallet().open("canonical-recovery");use->Wallet().unlockWallet("canonical-fixture-pass",0);}
    ASSERT_TRUE(f.wallet->EnsureRuntimeWalletBindings());EXPECT_EQ(f.Account(3).account.Operations().Entries().at(f.Operation(1)).transaction,authorization.Orchard().CanonicalBytes());
    // Fresh real admission/mining only AFTER the bound Ready owner committed.
    (void)f.Mine(MempoolTransaction::FromOrchard(authorization.Transaction()));
    EXPECT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(),Recovery::AppliedPrefix);
    EXPECT_EQ(f.Account(3).account.Scan().BalanceUna(),200000u);EXPECT_EQ(f.Account(17).account.Scan().BalanceUna(),200000u);EXPECT_EQ(f.Catalog(),catalog);
}
TEST(OrchardSpendOwner, ReadyRequiredWritesAndCommitPreserveReservedThenUnshield){
    OrchardSpendOwnerFixture f;auto request=f.Reserve(1,400000,true);const auto authorization=f.Prove(request);const auto before=f.Snapshot();
    EXPECT_THROW(f.Ready(2,request.revision,authorization),std::runtime_error);
    EXPECT_THROW(f.Ready(1,request.revision+1,authorization),std::runtime_error);EXPECT_EQ(f.Snapshot(),before);
    f.Sql("CREATE TRIGGER refuse_spend_ready BEFORE UPDATE ON orchard_wallet_snapshots BEGIN SELECT RAISE(ABORT,'Ready refusal'); END");
    EXPECT_THROW(f.Ready(1,request.revision,authorization),std::runtime_error);f.Sql("DROP TRIGGER refuse_spend_ready");EXPECT_EQ(f.Snapshot(),before);
    auto* db=f.Database();bool seen=false;sqlite3_commit_hook(db,[](void* p){*static_cast<bool*>(p)=true;return 1;},&seen);
    EXPECT_THROW(f.Ready(1,request.revision,authorization),std::runtime_error);sqlite3_commit_hook(db,nullptr,nullptr);EXPECT_TRUE(seen);EXPECT_EQ(f.Snapshot(),before);
    const auto ready=f.Ready(1,request.revision,authorization);EXPECT_EQ(ready.account.Operations().Entries().at(f.Operation(1)).phase,wallet::OrchardOperationQueue::Phase::Ready);
    EXPECT_THROW(ready.account.CancelReserved(f.Operation(1)),std::runtime_error);
    const auto body=MempoolTransaction::FromOrchard(authorization.Transaction());const auto txid=body.GetTxid().AsUint256();(void)f.Mine(body);
    EXPECT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(),Recovery::AppliedPrefix);EXPECT_EQ(f.Account(3).account.Scan().BalanceUna(),0u);
    ASSERT_TRUE(f.f.db.getCoin(txid,0).ok());EXPECT_EQ(f.f.db.getCoin(txid,0)->amount,400000u);EXPECT_EQ(f.f.db.getCoin(txid,0)->script_pubkey,util::hex(f.f.script));
    ASSERT_TRUE(ShieldedStateStartupTestAccess::DisconnectBoundary(*f.f.service,f.f.service->GetActiveTip()));
    EXPECT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(),Recovery::AppliedPrefix);const auto reopened=f.Account(3);
    EXPECT_EQ(reopened.account.Scan().BalanceUna(),500000u);EXPECT_EQ(reopened.account.Operations().Entries().at(f.Operation(1)).transaction,authorization.Orchard().CanonicalBytes());
    EXPECT_THROW(f.Reserve(2,1),std::runtime_error);
}
TEST(OrchardSpendOwner, ReservationRequiredReadWriteCommitAndAmountsRefuseWithoutEffects){
    OrchardSpendOwnerFixture f;const auto before=f.Snapshot();
    EXPECT_THROW(f.Reserve(0),std::runtime_error);
    EXPECT_THROW(f.Reserve(1,0),std::runtime_error);
    EXPECT_THROW(f.Reserve(1,orchard::kMaxMoneyUna),std::runtime_error);
    EXPECT_THROW(f.Reserve(1,500000),std::runtime_error);EXPECT_EQ(f.Snapshot(),before);
    f.Sql("CREATE TRIGGER refuse_spend_reservation BEFORE UPDATE ON orchard_wallet_snapshots BEGIN SELECT RAISE(ABORT,'reservation refusal'); END");
    EXPECT_THROW(f.Reserve(),std::runtime_error);f.Sql("DROP TRIGGER refuse_spend_reservation");EXPECT_EQ(f.Snapshot(),before);
    auto* db=f.Database();bool seen=false;
    // Call the owner directly so the hook targets the reservation COMMIT,
    // rather than the fixture's separate current-state read transaction.
    const auto view=f.f.service->getRuntimeAccountReplay();ASSERT_TRUE(view.ok());
    const auto revision=f.Account(3).revision;auto use=WalletService::AcquireWalletUse(f.wallet);const auto session=f.Session();
    const std::vector<orchard::WalletPayment> payments{{200000,f.AccountKeys(17).Receiver(orchard::WalletScope::External,{})}};
    sqlite3_commit_hook(db,[](void* p){*static_cast<bool*>(p)=true;return 1;},&seen);
    EXPECT_THROW(wallet::OrchardAccountDelivery::ReserveCatalogSpendForReplay(use->Wallet(),session,{f.Domain(),102,3},revision,**view,f.Operation(1),payments,{},100000),std::runtime_error);
    sqlite3_commit_hook(db,nullptr,nullptr);EXPECT_TRUE(seen);EXPECT_EQ(f.Snapshot(),before);
    sqlite3_set_authorizer(db,[](void*,int action,const char* table,const char*,const char*,const char*){return action==SQLITE_READ&&table&&std::string_view(table)=="orchard_wallet_retained"?SQLITE_DENY:SQLITE_OK;},nullptr);
    EXPECT_THROW(wallet::OrchardAccountDelivery::ReserveCatalogSpendForReplay(use->Wallet(),session,{f.Domain(),102,3},revision,**view,f.Operation(1),payments,{},100000),std::runtime_error);
    sqlite3_set_authorizer(db,nullptr,nullptr);EXPECT_EQ(f.Snapshot(),before);auto valid=f.Reserve();EXPECT_EQ(valid.committed_operations.Entries().size(),1u);
}
TEST(OrchardSpendOwner, EveryCatalogOwnerMustBePresentCaughtUpAndBoundToSession){
    OrchardSpendOwnerFixture f;f.MineEmpty();const auto behind=f.Snapshot();
    EXPECT_THROW(f.Reserve(),std::runtime_error);EXPECT_EQ(f.Snapshot(),behind);
    f.ReplayAccount(3);const auto other_behind=f.Snapshot();
    EXPECT_THROW(f.Reserve(),std::runtime_error);EXPECT_EQ(f.Snapshot(),other_behind);f.ReplayAccount(17);
    f.Sql("CREATE TEMP TABLE saved_spend_owner AS SELECT * FROM orchard_wallet_snapshots WHERE account=17; DELETE FROM orchard_wallet_snapshots WHERE account=17");
    const auto missing=f.Snapshot();
    EXPECT_THROW(f.Reserve(),std::runtime_error);EXPECT_EQ(f.Snapshot(),missing);
    f.Sql("INSERT INTO orchard_wallet_snapshots SELECT * FROM saved_spend_owner; DROP TABLE saved_spend_owner");
    const auto good=f.Snapshot();const auto view=f.f.service->getRuntimeAccountReplay();ASSERT_TRUE(view.ok());const auto revision=f.Account(3).revision;
    auto use=WalletService::AcquireWalletUse(f.wallet);const auto session=f.Session();auto* db=f.Database();const std::vector<orchard::TransparentOutput> outputs{{400000,f.f.script}};
    EXPECT_THROW(wallet::OrchardAccountDelivery::ReserveCatalogSpendForReplay(use->Wallet(),session+1,{f.Domain(),102,3},revision,**view,f.Operation(1),{},outputs,100000),std::runtime_error);
    auto borrowed_use=WalletService::AcquireWalletUse(f.wallet);auto borrowed_lease=borrowed_use->Wallet().AcquireDatabaseLease();f.Sql("BEGIN IMMEDIATE");
    EXPECT_THROW(wallet::OrchardAccountDelivery::ReserveCatalogSpendForReplay(use->Wallet(),session,{f.Domain(),102,3},revision,**view,f.Operation(1),{},outputs,100000),std::runtime_error);
    EXPECT_FALSE(sqlite3_get_autocommit(db));f.Sql("ROLLBACK");borrowed_lease.reset();borrowed_use.reset();EXPECT_EQ(f.Snapshot(),good);auto valid=f.Reserve(1,400000,true);EXPECT_TRUE(valid.committed_operations.Entries().at(f.Operation(1)).inputs.empty());
}
} // namespace dinero
#endif
