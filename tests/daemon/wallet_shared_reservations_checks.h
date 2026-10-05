#pragma once
#include "wallet/wallet_transaction_signer.h"
#include "wallet/orchard_ownership_inventory.h"
#ifdef DINERO_TEST_ORCHARD_ORIGIN
din::Json rpc_context_wallet_sendmany(const ExecutionContext&,const din::Json&);
din::Json rpc_context_wallet_sendtoaddress(const ExecutionContext&,const din::Json&);
namespace dinero {
namespace {
// The funding transaction is genuinely signed, mined and observed by the
// existing isolated fixture. No vault withdrawal or fee policy is changed.
struct SharedPaymentFixture : CanonicalVaultWithdrawalFixture {
    SharedPaymentFixture():CanonicalVaultWithdrawalFixture(1){}
    auto Input() {
        UnsignedTransaction input;input.tx.version=2;input.tx.vin.emplace_back();
        input.tx.vin[0].prevout.txid=TxId(funding.txid);input.tx.vin[0].prevout.vout=funding.vout;
        input.tx.vin[0].sequence=UINT32_MAX;
        input.tx.vout.emplace_back(AmountUna::Una(20000),script);
        input.tx.vout.emplace_back(AmountUna::Una(79000),script);
        input.selected_utxos={funding};input.fee=1000;input.change_amount=79000;input.change_address=address;
        return input;
    }
    auto Intent(){return PendingPaymentIntent{address,20000,"shared reservation fixture"};}
    void PrepareRpc(){
        context.tx_ingress=f.ingress.get();
        auto use=ChainstateService::AcquireWalletIndexUse(f.service);
        const auto found=use->Index().GetUTXO(TxId(funding.txid),funding.vout);
        if(!found)Need(use->Index().AddUTXO(WalletUTXO(TxId(funding.txid),funding.vout,
            funding.value,funding.spk,funding.path,funding.height)));
    }
    ~SharedPaymentFixture(){context.tx_ingress=nullptr;}
    auto BatchRequest(){din::Json request(Json::arrayValue),recipients;
        recipients[address]="0.00020000";request.append(recipients);request.append(2.0);return request;}

    auto Pay(){auto use=WalletService::AcquireWalletUse(wallet);auto index=ChainstateService::AcquireWalletIndexUse(f.service);
        return f.service->signAndStageWalletPayment(use->Wallet(),Selected(),Input(),Intent());}
    auto View(){auto view=f.service->getRuntimeAccountReplay();Need(view.ok());return *view;}
    auto Account(uint32_t number){const auto view=View();auto use=WalletService::AcquireWalletUse(wallet);
        return wallet::OrchardAccountDelivery::ReadForReplay(use->Wallet(),Selected().session,{Domain(),102,number},*view);}
    void Create(uint32_t number,bool catch_up=true){
        const auto created=rpc_context_wallet_orchard_createaccount(execution,OrchardCreationRequest(number));
        if(created.isMember("error"))throw std::runtime_error(created["error"].asString());
        if(!catch_up)return;
        const auto view=View();auto current=Account(number);auto use=WalletService::AcquireWalletUse(wallet);
        for(uint64_t sequence=current.account.Delivery().sequence+1;sequence<=view->Head().sequence;++sequence)
            current=wallet::OrchardAccountDelivery::ApplyForReplay(use->Wallet(),Selected().session,
                {Domain(),102,number},current.revision,*view,sequence);
    }
    void Sql(const char* sql){auto use=WalletService::AcquireWalletUse(wallet);auto lease=use->Wallet().AcquireDatabaseLease();
        Need(sqlite3_exec(lease->Database(),sql,nullptr,nullptr,nullptr)==SQLITE_OK);}
    // Exact main schema and all typed row bytes, sorted without relying on rowid.
    // Test-only snapshot; no production completeness or backup claim.
    std::string Snapshot(){auto use=WalletService::AcquireWalletUse(wallet);auto lease=use->Wallet().AcquireDatabaseLease();
        auto* db=lease->Database();struct Statement{sqlite3_stmt* p=nullptr;~Statement(){sqlite3_finalize(p);}} names;
        Need(sqlite3_prepare_v2(db,"SELECT name,sql FROM sqlite_master WHERE type='table' AND name NOT LIKE 'sqlite_%' ORDER BY name",-1,&names.p,nullptr)==SQLITE_OK);
        std::vector<std::pair<std::string,std::string>> tables;int rc;
        while((rc=sqlite3_step(names.p))==SQLITE_ROW){
            const auto read=[&](int c){const auto* data=static_cast<const char*>(sqlite3_column_blob(names.p,c));const auto n=sqlite3_column_bytes(names.p,c);Need(data&&n>0);return std::string(data,n);};
            tables.emplace_back(read(0),read(1));
        }Need(rc==SQLITE_DONE);std::string result;
        for(const auto& [name,schema]:tables){std::string quoted="\"";for(char c:name){quoted+=c;if(c=='\"')quoted+=c;}quoted+='\"';
            Statement rows;const auto sql="SELECT * FROM "+quoted;Need(sqlite3_prepare_v2(db,sql.c_str(),-1,&rows.p,nullptr)==SQLITE_OK);std::vector<std::string> values;
            while((rc=sqlite3_step(rows.p))==SQLITE_ROW){std::string row;
                for(int c=0;c<sqlite3_column_count(rows.p);++c){row+=std::to_string(sqlite3_column_type(rows.p,c))+":";
                    const auto* data=static_cast<const char*>(sqlite3_column_blob(rows.p,c));const auto n=sqlite3_column_bytes(rows.p,c);Need(n>=0&&(data||n==0));
                    row+=std::to_string(n)+":";if(n)row.append(data,n);}
                values.push_back(std::move(row));
            }Need(rc==SQLITE_DONE);std::sort(values.begin(),values.end());result+=schema+"\n";
            for(const auto& row:values)result+=std::to_string(row.size())+":"+row;
        }return result;
    }
    void ReserveFunding(uint32_t number){
        auto current=Account(number);auto use=WalletService::AcquireWalletUse(wallet);auto lease=use->Wallet().AcquireDatabaseLease();
        auto pin=lease->CopyRecoverySeed(Selected().session);
        const auto keys=orchard::WalletKeys::FromSeed(pin->Bytes(),number);
        orchard::ResolvedInput input{};std::copy(funding.txid.begin(),funding.txid.end(),input.txid_wire.begin());
        input.output_index=funding.vout;input.sequence=UINT32_MAX;input.amount_una=funding.value.GetUna();input.script_pub_key=funding.spk;
        const std::vector<orchard::ResolvedInput> inputs{input};
        const std::vector<orchard::TransparentOutput> outputs{{79000,script}};
        const std::vector<orchard::WalletPayment> payments{{20000,keys.Receiver(orchard::WalletScope::External,{})}};
        const auto signing=orchard::SigningContext::Create(Domain(),0,inputs,outputs,1000);
        auto plan=orchard::WalletBundlePlan::PrepareShield(keys,payments);
        const auto reserved=current.account.Reserve(orchard::Hash{73},plan.Intent(signing));
        InventoryTestTransaction tx(lease->Database());
        const auto inventory=wallet::OrchardOwnershipInventory::Read(lease->Database(),pin->Bytes());
        const auto found=std::find_if(inventory.accounts.begin(),inventory.accounts.end(),[&](const auto& a){return a.entry.account==number;});
        Need(found!=inventory.accounts.end());orchard::WalletSnapshotStore store(lease->Database(),found->identity,pin->Bytes());
        (void)store.StageReplaceRetaining(current.revision,reserved.Encode());tx.Commit();
    }
};
}
TEST(WalletSharedReservations, GeneratedEmptyAndNonconsecutiveOwnersCommitAndReopen){
    for(bool populated:{false,true}){
        SharedPaymentFixture f;if(populated){f.Create(3);f.Create(17);}
        const auto before=f.Snapshot();const auto result=f.Pay();ASSERT_TRUE(result.success)<<result.error;
        const auto pending=f.wallet->get().getPendingPayments();ASSERT_EQ(pending.size(),1u);
        EXPECT_EQ(pending[0].signed_body,result.signed_tx.tx.Serialize(TxSerializationMode::WithWitness));
        EXPECT_EQ(pending[0].intent,f.Intent());EXPECT_NE(f.Snapshot(),before);
        const auto committed=f.Snapshot();EXPECT_FALSE(f.Pay().success);EXPECT_EQ(f.Snapshot(),committed);
        f.Reopen();EXPECT_EQ(f.Snapshot(),committed);EXPECT_EQ(f.wallet->get().getPendingPayments()[0].signed_body,pending[0].signed_body);
        EXPECT_FALSE(f.Pay().success);EXPECT_EQ(f.Snapshot(),committed);
    }
}
TEST(WalletSharedReservations, OrchardFundingReservationRefusesOrdinaryDoubleSpend){
    SharedPaymentFixture f;f.Create(3);f.Create(17);f.ReserveFunding(17);
    const auto before=f.Snapshot();const auto account=OrchardCreationBytes(f.Account(17).account);
    const auto refused=f.Pay();EXPECT_FALSE(refused.success);EXPECT_FALSE(refused.error.empty());
    EXPECT_TRUE(f.wallet->get().getPendingPayments().empty());EXPECT_EQ(f.Snapshot(),before);
    EXPECT_EQ(OrchardCreationBytes(f.Account(17).account),account);
    f.Reopen();EXPECT_FALSE(f.Pay().success);EXPECT_EQ(f.Snapshot(),before);
}
TEST(WalletSharedReservations, RequiredHistoryAndCommitRefusalRollBackEntirePayment){
    SharedPaymentFixture f;f.Create(3);f.Create(17);const auto before=f.Snapshot();
    f.Sql("CREATE TRIGGER refuse_shared_payment BEFORE INSERT ON transactions BEGIN SELECT RAISE(ABORT,'shared payment refusal'); END");
    EXPECT_FALSE(f.Pay().success);EXPECT_EQ(f.Snapshot(),before);EXPECT_TRUE(f.wallet->get().getPendingPayments().empty());
    f.Sql("DROP TRIGGER refuse_shared_payment");
    struct Commit {
        ChainstateService& service;int count=0,preparations=0;
        bool selected=false,staged=false,staged_selected=false,preparation_selected=false;
    } observed{*f.f.service};
    auto* db=f.wallet->get().getCurrentDatabase();
    // The detached read-only preparation commits before selected ownership.
    // Reject the payment commit only after its durable owner write actually
    // ran, so the original whole-payment rollback and selected-lock checks
    // still exercise the writer rather than an earlier preparation refusal.
    ASSERT_EQ(sqlite3_create_function_v2(db,"observe_shared_payment_owner",0,SQLITE_UTF8,&observed,
        [](sqlite3_context* context,int,sqlite3_value**){
            auto& state=*static_cast<Commit*>(sqlite3_user_data(context));state.staged=true;
            state.staged_selected=ShieldedStateStartupTestAccess::VaultSelectedHeld(state.service);
            sqlite3_result_null(context);
        },nullptr,nullptr,nullptr),SQLITE_OK);
    struct ObserverCleanup {
        sqlite3* db;
        ~ObserverCleanup(){
            if(!db)return;
            sqlite3_commit_hook(db,nullptr,nullptr);
            sqlite3_exec(db,"DROP TRIGGER IF EXISTS temp.observe_shared_payment_owner",nullptr,nullptr,nullptr);
            sqlite3_create_function_v2(db,"observe_shared_payment_owner",0,SQLITE_UTF8,nullptr,nullptr,nullptr,nullptr,nullptr);
        }
    } cleanup{db};
    f.Sql("CREATE TEMP TRIGGER observe_shared_payment_owner AFTER UPDATE OF pending_payment_owner ON main.wallet_meta WHEN NEW.id=1 BEGIN SELECT observe_shared_payment_owner(); END");
    sqlite3_commit_hook(db,[](void* context){auto& state=*static_cast<Commit*>(context);
        if(!state.staged){++state.preparations;
            state.preparation_selected|=ShieldedStateStartupTestAccess::VaultSelectedHeld(state.service);return 0;}
        ++state.count;state.selected=ShieldedStateStartupTestAccess::VaultSelectedHeld(state.service);return 1;
    },&observed);
    const auto refused=f.Pay();sqlite3_commit_hook(db,nullptr,nullptr);
    f.Sql("DROP TRIGGER temp.observe_shared_payment_owner");
    EXPECT_EQ(sqlite3_create_function_v2(db,"observe_shared_payment_owner",0,SQLITE_UTF8,nullptr,nullptr,nullptr,nullptr,nullptr),SQLITE_OK);
    cleanup.db=nullptr; // Reopen below invalidates the old SQLite handle.
    EXPECT_EQ(observed.preparations,1);EXPECT_FALSE(observed.preparation_selected);
    EXPECT_TRUE(observed.staged);EXPECT_TRUE(observed.staged_selected);
    EXPECT_FALSE(refused.success);EXPECT_EQ(observed.count,1);EXPECT_TRUE(observed.selected);EXPECT_TRUE(sqlite3_get_autocommit(db));
    EXPECT_EQ(f.Snapshot(),before);EXPECT_TRUE(f.wallet->get().getPendingPayments().empty());
    f.Reopen();EXPECT_EQ(f.Snapshot(),before);EXPECT_TRUE(f.Pay().success);
}
TEST(WalletSharedReservations, DeniedInventoryAndBorrowedTransactionPreserveOwners){
    SharedPaymentFixture f;f.Create(3);f.Create(17);const auto before=f.Snapshot();auto* db=f.wallet->get().getCurrentDatabase();
    sqlite3_set_authorizer(db,[](void*,int action,const char* table,const char*,const char*,const char*){
        return action==SQLITE_READ&&table&&std::string_view(table)=="orchard_wallet_retained"?SQLITE_DENY:SQLITE_OK;},nullptr);
    const auto denied=f.Pay();sqlite3_set_authorizer(db,nullptr,nullptr);EXPECT_FALSE(denied.success);EXPECT_EQ(f.Snapshot(),before);
    {
        auto use=WalletService::AcquireWalletUse(f.wallet);auto lease=use->Wallet().AcquireDatabaseLease();
        ASSERT_EQ(sqlite3_exec(db,"BEGIN IMMEDIATE",nullptr,nullptr,nullptr),SQLITE_OK);
        EXPECT_FALSE(f.Pay().success);EXPECT_FALSE(sqlite3_get_autocommit(db));
        ASSERT_EQ(sqlite3_exec(db,"ROLLBACK",nullptr,nullptr,nullptr),SQLITE_OK);
    }
    EXPECT_EQ(f.Snapshot(),before);EXPECT_TRUE(f.Pay().success);
}
TEST(WalletSharedReservations, LaggingMissingAndUnknownCatalogRefuseWithoutSigning){
    SharedPaymentFixture f;f.Create(3);f.Create(17,false);auto before=f.Snapshot();
    EXPECT_FALSE(f.Pay().success);EXPECT_EQ(f.Snapshot(),before);
    f.Sql("DELETE FROM orchard_wallet_snapshots WHERE account=17");before=f.Snapshot();
    EXPECT_FALSE(f.Pay().success);EXPECT_EQ(f.Snapshot(),before);EXPECT_TRUE(f.wallet->get().getPendingPayments().empty());
    f.Sql("DELETE FROM settings WHERE key='orchard_account_catalog_v1'");before=f.Snapshot();
    EXPECT_FALSE(f.Pay().success);EXPECT_EQ(f.Snapshot(),before);
}
TEST(WalletSharedReservations, TipCoinHasOneConfirmationBeforeOrdinarySelection){
    SharedPaymentFixture f;f.Create(3);f.Create(17);f.PrepareRpc();
    ASSERT_EQ(f.f.service->GetActiveTip()->height,f.funding.height);
    const auto rows=f.wallet->get().listUnspentUTXOs(1,9999999);
    const auto coin=std::find_if(rows.begin(),rows.end(),[&](const auto& u){return u.txid==f.funding.GetTxIdHex()&&u.vout==f.funding.vout;});
    ASSERT_NE(coin,rows.end());EXPECT_EQ(coin->confirmations,1);EXPECT_TRUE(coin->spendable);EXPECT_TRUE(coin->is_mature);
    const auto two=f.wallet->get().listUnspentUTXOs(2,9999999);
    const auto waiting=std::find_if(two.begin(),two.end(),[&](const auto& u){return u.txid==f.funding.GetTxIdHex()&&u.vout==f.funding.vout;});
    ASSERT_NE(waiting,two.end());EXPECT_EQ(waiting->confirmations,1);EXPECT_FALSE(waiting->spendable);
    const auto before=f.Snapshot();EXPECT_EQ(f.wallet->get().listUnspentUTXOs(1,9999999).size(),rows.size());EXPECT_EQ(f.Snapshot(),before);
}
TEST(WalletSharedReservations, ActualSendManyCommitsBeforeCanonicalAdmission){
    SharedPaymentFixture f;f.Create(3);f.Create(17);f.PrepareRpc();
    const auto result=::rpc_context_wallet_sendmany(f.execution,f.BatchRequest());
    ASSERT_FALSE(result.isMember("error"))<<result.toStyledString();ASSERT_TRUE(result["payment_retained"].asBool());
    const auto pending=f.wallet->get().getPendingPayments();ASSERT_EQ(pending.size(),1u);
    EXPECT_EQ(pending[0].txid,result["txid"].asString());EXPECT_EQ(pending[0].intent.amount_una,20000u);
    const auto txid=uint256::FromHexUnsafe(pending[0].txid);ASSERT_TRUE(f.f.ingress->HasTransaction(txid));
    const auto admitted=f.f.ingress->GetTransaction(txid);ASSERT_TRUE(admitted);
    EXPECT_EQ(admitted->Serialize(TxSerializationMode::WithWitness),pending[0].signed_body);
    f.Reopen();EXPECT_EQ(f.wallet->get().getPendingPayments()[0].signed_body,pending[0].signed_body);
}
TEST(WalletSharedReservations, ActualSendManyRefusesOtherAccountReservationBeforeSubmission){
    SharedPaymentFixture f;f.Create(3);f.Create(17);f.ReserveFunding(17);f.PrepareRpc();
    const auto before=OrchardCreationBytes(f.Account(17).account);const auto pool=f.f.ingress->mempool().size();
    const auto result=::rpc_context_wallet_sendmany(f.execution,f.BatchRequest());
    EXPECT_TRUE(result.isMember("error"));EXPECT_FALSE(result["payment_retained"].asBool());
    EXPECT_TRUE(f.wallet->get().getPendingPayments().empty());EXPECT_EQ(f.f.ingress->mempool().size(),pool);
    EXPECT_EQ(OrchardCreationBytes(f.Account(17).account),before);
}
TEST(WalletSharedReservations, ActualSendToAddressRetainsSameCanonicalAcceptedBody){
    SharedPaymentFixture f;f.Create(3);f.Create(17);f.PrepareRpc();
    din::Json request;request["address"]=f.address;request["amount"]="0.00020000";request["fee_rate"]=2.0;request["broadcast"]=true;
    const auto result=::rpc_context_wallet_sendtoaddress(f.execution,request);
    ASSERT_FALSE(result.isMember("error"))<<result.toStyledString();ASSERT_TRUE(result["payment_retained"].asBool());
    const auto pending=f.wallet->get().getPendingPayments();ASSERT_EQ(pending.size(),1u);
    EXPECT_EQ(pending[0].txid,result["txid"].asString());const auto txid=uint256::FromHexUnsafe(pending[0].txid);
    ASSERT_TRUE(f.f.ingress->HasTransaction(txid));const auto admitted=f.f.ingress->GetTransaction(txid);ASSERT_TRUE(admitted);
    EXPECT_EQ(admitted->Serialize(TxSerializationMode::WithWitness),pending[0].signed_body);
}
} // namespace dinero
#endif
