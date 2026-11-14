#pragma once
#include "daemon/services/chainstate_recovery_source.h"
#include "wallet/runtime_origin_projection.h"
#include "wallet/runtime_ordinary_tip.h"
#include "wallet/wallet_index_v3_schema.h"
#include "crypto/sha256.h"
#include "rpc/orchard_account_rpc.h"
din::Json rpc_context_wallet_getbalance(const ExecutionContext&, const din::Json&);
din::Json rpc_context_wallet_getwalletinfo(const ExecutionContext&, const din::Json&);
din::Json rpc_context_wallet_snapshot(const ExecutionContext&, const din::Json&);
din::Json rpc_context_wallet_listtransactions(const ExecutionContext&, const din::Json&);
din::Json rpc_context_wallet_sendtoaddress(const ExecutionContext&, const din::Json&);
din::Json rpc_context_wallet_getstatus(const ExecutionContext&, const din::Json&);
din::Json rpc_context_wallet_getsyncstate(const ExecutionContext&, const din::Json&);
din::Json rpc_context_wallet_unlock(const ExecutionContext&, const din::Json&);
namespace dinero {
namespace {
using Recovery=WalletService::CanonicalRecovery;
#ifdef DINERO_TEST_ORCHARD_ORIGIN
std::string TestV3LegacyGuardDigest(sqlite3* db) {
    sqlite3_stmt* raw=nullptr;
    const char* sql="SELECT tbl_name,type,name,sql FROM sqlite_schema WHERE "
        "tbl_name IN ('wallet_utxos_legacy_v2','utxo_metadata') AND type IN ('index','trigger') "
        "AND sql IS NOT NULL ORDER BY tbl_name,type,name";
    OrchardAdmissionFixture::Require(sqlite3_prepare_v2(db,sql,-1,&raw,nullptr)==SQLITE_OK);
    std::unique_ptr<sqlite3_stmt,decltype(&sqlite3_finalize)> query(raw,sqlite3_finalize);
    crypto::CSHA256 hash;
    const auto number=[&](uint64_t value) {
        std::array<uint8_t,8> bytes{};
        for(size_t i=0;i<bytes.size();++i)bytes[i]=uint8_t(value>>(8*i));
        hash.Write(bytes.data(),bytes.size());
    };
    number(4);
    int step=SQLITE_OK;
    while((step=sqlite3_step(query.get()))==SQLITE_ROW) {
        const uint8_t row=0x72;hash.Write(&row,1);
        for(int n=0;n<4;++n) {
            OrchardAdmissionFixture::Require(sqlite3_column_type(query.get(),n)==SQLITE_TEXT);
            const uint8_t tag=SQLITE_TEXT;hash.Write(&tag,1);
            const int size=sqlite3_column_bytes(query.get(),n);
            OrchardAdmissionFixture::Require(size>=0);
            const auto* value=static_cast<const uint8_t*>(sqlite3_column_blob(query.get(),n));
            OrchardAdmissionFixture::Require(size==0 || value);
            number(uint64_t(size));if(size)hash.Write(value,size);
        }
    }
    OrchardAdmissionFixture::Require(step==SQLITE_DONE);
    std::array<uint8_t,32> digest{};hash.Finalize(digest.data());
    static constexpr char hex[]="0123456789abcdef";
    std::string result;result.reserve(64);
    for(const auto byte:digest){result.push_back(hex[byte>>4]);result.push_back(hex[byte&15]);}
    return result;
}
std::string TestV3MetadataText(sqlite3* db,const char* key) {
    sqlite3_stmt* raw=nullptr;
    OrchardAdmissionFixture::Require(sqlite3_prepare_v2(db,
        "SELECT value FROM utxo_metadata WHERE key=?",-1,&raw,nullptr)==SQLITE_OK);
    std::unique_ptr<sqlite3_stmt,decltype(&sqlite3_finalize)> query(raw,sqlite3_finalize);
    OrchardAdmissionFixture::Require(sqlite3_bind_text(query.get(),1,key,-1,SQLITE_TRANSIENT)==SQLITE_OK);
    OrchardAdmissionFixture::Require(sqlite3_step(query.get())==SQLITE_ROW &&
        sqlite3_column_type(query.get(),0)==SQLITE_TEXT);
    const auto* value=static_cast<const char*>(sqlite3_column_blob(query.get(),0));
    const std::string result(value,sqlite3_column_bytes(query.get(),0));
    OrchardAdmissionFixture::Require(sqlite3_step(query.get())==SQLITE_DONE);
    return result;
}
void TestV3SetMetadata(sqlite3* db,const char* key,const std::string& value) {
    sqlite3_stmt* raw=nullptr;
    OrchardAdmissionFixture::Require(sqlite3_prepare_v2(db,
        "UPDATE utxo_metadata SET value=? WHERE key=?",-1,&raw,nullptr)==SQLITE_OK);
    std::unique_ptr<sqlite3_stmt,decltype(&sqlite3_finalize)> query(raw,sqlite3_finalize);
    OrchardAdmissionFixture::Require(sqlite3_bind_text(query.get(),1,value.data(),int(value.size()),SQLITE_TRANSIENT)==SQLITE_OK);
    OrchardAdmissionFixture::Require(sqlite3_bind_text(query.get(),2,key,-1,SQLITE_TRANSIENT)==SQLITE_OK);
    OrchardAdmissionFixture::Require(sqlite3_step(query.get())==SQLITE_DONE && sqlite3_changes(db)==1);
}
#endif
struct CanonicalRecoveryHistoricalFixture : WalletIndexOwnerFixture {
    std::shared_ptr<WalletService> wallet=std::make_shared<WalletService>();
    CanonicalRecoveryHistoricalFixture() {
        context.wallet=wallet;
        if(!wallet->Init(context))throw std::runtime_error("actual wallet Init");
        auto use=WalletService::AcquireWalletUse(wallet);auto index=ChainstateService::AcquireWalletIndexUse(source);
        use->Wallet().setUTXOIndex(&index->Index());use->Wallet().create("historical-recovery");
    }
    ~CanonicalRecoveryHistoricalFixture(){wallet->Stop();context.wallet.reset();wallet.reset();}
};
}
TEST(WalletCanonicalRecovery, MissingOwnersAndConfiguredUnavailableSourceDefer) {
    auto missing=std::make_shared<WalletService>();std::string error;
    EXPECT_EQ(missing->RecoverActiveWalletFromCanonicalSource(&error),Recovery::Deferred);EXPECT_FALSE(error.empty());
    CanonicalRecoveryHistoricalFixture f;
    MutableParams().orchard_activation_height=102;MutableParams().orchard_branch_id=1;
    EXPECT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(&error),Recovery::Deferred);EXPECT_FALSE(error.empty());
    auto use=WalletService::AcquireWalletUse(f.wallet);auto lease=use->Wallet().AcquireDatabaseLease();
    EXPECT_TRUE(sqlite3_get_autocommit(lease->Database()));
    EXPECT_EQ(use->Wallet().getCurrentWalletName(),"historical-recovery");
}
TEST(WalletCanonicalRecovery, UnsetProfileAndBorrowedLeasePreserveHistoricalWallet) {
    CanonicalRecoveryHistoricalFixture f;
    EXPECT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(),Recovery::NotRequired);
    auto use=WalletService::AcquireWalletUse(f.wallet);
    {auto lease=use->Wallet().AcquireDatabaseLease();const auto session=lease->Session();
     EXPECT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(),Recovery::Deferred);
     EXPECT_EQ(lease->Session(),session);EXPECT_TRUE(sqlite3_get_autocommit(lease->Database()));}
    EXPECT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(),Recovery::NotRequired);
    EXPECT_TRUE(f.wallet->RecoverActiveWalletFromSnapshotIfNeeded());
    {auto selected=f.source->AcquireBlockIngressActivationLock();
     EXPECT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(),Recovery::Deferred);}
    EXPECT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(),Recovery::NotRequired);
    const auto previous_session=use->Wallet().AcquireDatabaseLease()->Session();
    use->Wallet().open("historical-recovery");
    EXPECT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(nullptr,previous_session),Recovery::Deferred);
    EXPECT_FALSE(f.wallet->RecoverActiveWalletFromSnapshotIfNeeded(nullptr,previous_session));
    EXPECT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(),Recovery::NotRequired);
}
#ifdef DINERO_TEST_ORCHARD_ORIGIN
namespace {
struct CanonicalRecoveryFixture : OrchardCycleFixture {
    std::shared_ptr<WalletService> wallet=std::make_shared<WalletService>();
    std::string receive_address;
    explicit CanonicalRecoveryFixture(bool encrypted=false,bool funded_historical=false)
        :OrchardCycleFixture(false,funded_historical
            ?OrchardAdmissionFixture::HistoricalSpend::NamedWallet
            :OrchardAdmissionFixture::HistoricalSpend::None) {
        // Stop the old adapter before Init replaces its coin owner. Retain the
        // stopped service for base-fixture cleanup if initialization throws.
        f.ingress->Stop();context.mempool.reset();
        auto config=std::make_shared<ConfigService>();config->Set("datadir",(f.path/"wallet-runtime").string());
        context.config=config;context.logger=std::make_shared<LoggerService>("");context.logger_interface=f.logger.get();
        context.block_storage=files;
        OrchardAdmissionFixture::Require(f.service->Init(context));
        ShieldedStateStartupTestAccess::PopulateInitializedParent(*f.service,*parent,*f.replay);
        f.ingress=std::make_shared<MempoolService>();OrchardAdmissionFixture::Require(f.ingress->Init(context));context.mempool=f.ingress;
        context.wallet=wallet;OrchardAdmissionFixture::Require(wallet->Init(context));
        {auto use=WalletService::AcquireWalletUse(wallet);auto index=ChainstateService::AcquireWalletIndexUse(f.service);
         use->Wallet().setUTXOIndex(&index->Index());
         if(funded_historical)
             use->Wallet().createFromBip39("canonical-recovery",
                 "abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon about","");
         else use->Wallet().create("canonical-recovery");
         receive_address=use->Wallet().getNewAddress();
         if(funded_historical)OrchardAdmissionFixture::Require(receive_address==f.funding_address);
         if(encrypted){use->Wallet().encryptWallet("canonical-fixture-pass");use->Wallet().unlockWallet("canonical-fixture-pass",0);}}
    }
    ~CanonicalRecoveryFixture(){wallet->Stop();context.wallet.reset();wallet.reset();}
    uint64_t Session(){auto use=WalletService::AcquireWalletUse(wallet);return use->Wallet().AcquireDatabaseLease()->Session();}
    auto Keys(){auto use=WalletService::AcquireWalletUse(wallet);auto lease=use->Wallet().AcquireDatabaseLease();
        auto seed=lease->CopyRecoverySeed(lease->Session());return orchard::WalletKeys::FromSeed(seed->Bytes(),0);}
    void EnrollAccount() {
        auto use=WalletService::AcquireWalletUse(wallet);auto lease=use->Wallet().AcquireDatabaseLease();
        auto seed=lease->CopyRecoverySeed(lease->Session());const auto keys=orchard::WalletKeys::FromSeed(seed->Bytes(),0);
        const auto encoded=lease->EnsureDeliveryIdentity();orchard::Hash identity{};
        for(size_t i=0;i<32;++i)identity[i]=uint8_t(std::stoul(encoded.substr(7+2*i,2),nullptr,16));
        auto account=dinero::wallet::OrchardAccountState::Begin(Domain(),keys.ExportFullViewingKey(),102,parent->hash)
            .IssueReceiver(orchard::WalletScope::External).first;
        OrchardAdmissionFixture::Require(sqlite3_exec(lease->Database(),"BEGIN IMMEDIATE",nullptr,nullptr,nullptr)==SQLITE_OK);
        orchard::WalletSnapshotStore::InitializeSchemaUnderTransaction(lease->Database());
        orchard::WalletSnapshotStore store(lease->Database(),{orchard::WalletNetwork::Regtest,Domain().genesis_wire,identity,0},seed->Bytes());
        OrchardAdmissionFixture::Require(store.StageReplaceRetaining(0,account.Encode())==1);
        OrchardAdmissionFixture::Require(sqlite3_exec(lease->Database(),"COMMIT",nullptr,nullptr,nullptr)==SQLITE_OK);
    }
    void MineAndAdopt() {
        const auto keys=Keys();const auto [body,bundle]=Shield(keys);(void)Mine(body);
        ExecutionContext request;request.daemon=&context;request.walletName="canonical-recovery";
        din::Json params;params["account"]=Json::UInt64(0);
        const auto created=rpc_context_wallet_orchard_createaccount(request,params);
        OrchardAdmissionFixture::Require(!created.isMember("error"));
        auto use=WalletService::AcquireWalletUse(wallet);auto index=ChainstateService::AcquireWalletIndexUse(f.service);
        const auto session=use->Wallet().AcquireDatabaseLease()->Session();
        const auto origin=f.service->getRuntimeWalletOrigin(use->Wallet(),session,&index->Index());
        OrchardAdmissionFixture::Require(origin.ok());
        OrchardAdmissionFixture::Require(f.service->adoptRuntimeWalletOrigin(use->Wallet(),index->Index(),**origin)==Status::Ok);
    }
    auto ReadAccount() {
        // Source captured outside wallet database/key ownership.
        const auto view=f.service->getRuntimeAccountReplay();OrchardAdmissionFixture::Require(view.ok());
        auto use=WalletService::AcquireWalletUse(wallet);const auto session=use->Wallet().AcquireDatabaseLease()->Session();
        return dinero::wallet::OrchardAccountDelivery::ReadForReplay(use->Wallet(),session,{Domain(),102,0},**view);
    }
    void MineEmpty() {
        BlockAssembler assembler(&f.db);WireOrchardAssembler(assembler,f);
        const auto block=assembler.CreateOrchardBlock(OrchardMiningPayout);OrchardAdmissionFixture::Require(bool(block));
        const auto result=Submit(block->WireBytes());OrchardAdmissionFixture::Require(result.accepted()&&result.connected);
    }
    void DowngradeToPreIdentityTransparentWallet() {
        auto use=WalletService::AcquireWalletUse(wallet);
        auto lease=use->Wallet().AcquireDatabaseLease();
        auto* db=lease->Database();
        OrchardAdmissionFixture::Require(sqlite3_exec(db,
            "DELETE FROM settings WHERE key IN ('wallet_initial_owner_v1','orchard_account_catalog_v1');"
            "ALTER TABLE wallet_meta DROP COLUMN runtime_delivery_id;",
            nullptr,nullptr,nullptr)==SQLITE_OK);
        sqlite3_stmt* rows=nullptr;
        OrchardAdmissionFixture::Require(sqlite3_prepare_v2(db,
            "SELECT 1 FROM sqlite_master WHERE type='table' AND name LIKE 'orchard_wallet_%'",
            -1,&rows,nullptr)==SQLITE_OK);
        OrchardAdmissionFixture::Require(sqlite3_step(rows)==SQLITE_DONE);
        sqlite3_finalize(rows);
        lease.reset();
        use->Wallet().setBlockchainHeight(101);
    }
    uint64_t MineToWallet() {
        BlockAssembler assembler(&f.db);WireOrchardAssembler(assembler,f);
        const auto block=assembler.CreateOrchardBlock(receive_address);
        OrchardAdmissionFixture::Require(bool(block));
        const auto result=Submit(block->WireBytes());OrchardAdmissionFixture::Require(result.accepted()&&result.connected);
        const auto coin=f.db.getCoin(block->Transactions().front().GetTxid().AsUint256(),0);
        OrchardAdmissionFixture::Require(coin.ok()&&coin->coinbase&&coin->height==int(block->Height()));
        return coin->amount;
    }
    MempoolTransaction ShieldHistoricalWalletCoin(const orchard::WalletKeys& receiver) {
        const OutPoint point(f.blocks.at(101).vtx.at(1).GetTxid(),0);
        const auto& coin=f.replay->ProvenUtxos().at(point);
        const auto decoded=DecodeWitnessAddress(receive_address,"rdin");
        OrchardAdmissionFixture::Require(decoded.is_valid&&decoded.script_pubkey==coin.scriptPubKey);
        auto use=WalletService::AcquireWalletUse(wallet);auto lease=use->Wallet().AcquireDatabaseLease();
        auto seed=lease->CopyRecoverySeed(lease->Session());
        const auto key=lease->ResolveSigningKey(util::hex(coin.scriptPubKey),*seed);
        OrchardAdmissionFixture::Require(key&&key->policy==SigningKeyPolicy::TaprootCanonical&&
            key->secret.size()==32&&key->script==coin.scriptPubKey);
        orchard::EnvelopeInput input{};
        std::copy(point.txid.AsUint256().begin(),point.txid.AsUint256().end(),input.txid_wire.begin());
        input.output_index=0;input.sequence=UINT32_MAX;
        constexpr uint64_t fee=100000,deposit=500000;
        const std::vector<orchard::TransparentOutput> outputs{{coin.value.GetUna()-fee-deposit,coin.scriptPubKey}};
        const std::vector<orchard::WalletPayment> payments{{deposit,receiver.Receiver(orchard::WalletScope::External,{}),{}}};
        const std::vector<orchard::ResolvedInput> inputs{{input.txid_wire,0,input.sequence,coin.value.GetUna(),coin.scriptPubKey}};
        const auto bundle=orchard::WalletBundlePlan::PrepareShield(receiver,payments)
            .Prove(orchard::SigningContext::Create(Domain(),0,inputs,outputs,fee));
        const auto draft=orchard::TransactionEnvelope::Create(0,{input},outputs,fee,bundle.Bytes());
        FirstBoundaryView view(*f.replay);
        const auto snapshot=consensus::OrchardCoinSnapshot::ResolveUnderChainstateLock(draft,view);
        const auto digest=consensus::OrchardTransparentSigningDigest(snapshot,Domain(),0);
        std::array<uint8_t,32> internal{},xonly{};
        std::copy_n(key->secret.begin(),32,internal.begin());
        std::unique_ptr<secp256k1_context,decltype(&secp256k1_context_destroy)> signing(
            secp256k1_context_create(SECP256K1_CONTEXT_NONE),secp256k1_context_destroy);
        secp256k1_keypair pair;secp256k1_xonly_pubkey public_key;
        OrchardAdmissionFixture::Require(secp256k1_keypair_create(signing.get(),&pair,internal.data())&&
            secp256k1_keypair_xonly_pub(signing.get(),&public_key,nullptr,&pair)&&
            secp256k1_xonly_pubkey_serialize(signing.get(),xonly.data(),&public_key));
        std::array<uint8_t,64> signature{};orchard::Hash aux{};
        OrchardAdmissionFixture::Require(TaprootKeys::SignSchnorrWithInternalKey(
            signature,digest,internal,xonly,aux.data()));
        input.witness={{signature.begin(),signature.end()}};
        return MempoolTransaction::FromOrchard(
            orchard::TransactionEnvelope::Create(0,{input},outputs,fee,bundle.Bytes()));
    }
    std::pair<uint64_t,double> PrepareFundedHistoricalWallet() {
        auto use=WalletService::AcquireWalletUse(wallet);
        auto index=ChainstateService::AcquireWalletIndexUse(f.service);
        std::string error;
        const auto scanned=f.service->RescanWalletFromSelectedHistory(use->Wallet(),0,0,&error);
        OrchardAdmissionFixture::Require(scanned && *scanned==101);
        const auto coin=f.db.getCoin(f.blocks[101].vtx.at(1).GetTxid().AsUint256(),0);
        OrchardAdmissionFixture::Require(coin.ok()&&!coin->coinbase&&coin->height==101);
        const auto balance=use->Wallet().getBalance();
        OrchardAdmissionFixture::Require(balance.confirmed>0);
        return {coin->amount,balance.total};
    }
};
}
TEST(WalletCanonicalRecovery, LegacyTransparentWalletEarnsActualCoinbaseWithoutCatalog) {
    CanonicalRecoveryFixture f(false,true);
    const auto [historical_amount,historical_total]=f.PrepareFundedHistoricalWallet();
    f.DowngradeToPreIdentityTransparentWallet();
    const auto receiver=orchard::WalletKeys::FromSeed(std::array<uint8_t,64>{53},0);
    const auto shield=f.ShieldHistoricalWalletCoin(receiver);
    const auto shield_txid=shield.GetTxid().AsUint256().GetHex();
    const auto spent=f.Mine(shield);
    ASSERT_EQ(spent->Context()->height,102u);
    const auto amount=f.MineToWallet();
    ASSERT_EQ(f.f.service->GetActiveTip()->height,103u);
    ASSERT_EQ(f.f.db.getBlockHashByHeight(103).value(),f.f.service->GetActiveTip()->hash);
    auto use=WalletService::AcquireWalletUse(f.wallet);
    {
        auto lease=use->Wallet().AcquireDatabaseLease();
        sqlite3_stmt* q=nullptr;
        ASSERT_EQ(sqlite3_prepare_v2(lease->Database(),
            "SELECT 1 FROM pragma_table_info('wallet_meta') WHERE name='runtime_delivery_id'",
            -1,&q,nullptr),SQLITE_OK);
        EXPECT_EQ(sqlite3_step(q),SQLITE_DONE);
        sqlite3_finalize(q);
    }
    use.reset();
    EXPECT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(),Recovery::AppliedPrefix);
    ASSERT_TRUE(f.wallet->Start());
    use=WalletService::AcquireWalletUse(f.wallet);
    EXPECT_EQ(use->Wallet().getCurrentWalletName(),"canonical-recovery");
    ASSERT_TRUE(use->Wallet().WaitForHeight(103,std::chrono::seconds(20)));
    const auto old=f.f.blocks[101].vtx.at(1).GetTxid().AsUint256().GetHex();
    sqlite3_stmt* old_row=nullptr;
    auto old_lease=use->Wallet().AcquireDatabaseLease();
    ASSERT_EQ(sqlite3_prepare_v2(old_lease->Database(),
        "SELECT amount,is_spent,spent_height FROM utxos WHERE txid=? AND vout=0",
        -1,&old_row,nullptr),SQLITE_OK);
    ASSERT_EQ(sqlite3_bind_text(old_row,1,old.c_str(),-1,SQLITE_TRANSIENT),SQLITE_OK);
    ASSERT_EQ(sqlite3_step(old_row),SQLITE_ROW);
    EXPECT_EQ(sqlite3_column_int64(old_row,0),int64_t(historical_amount));
    EXPECT_EQ(sqlite3_column_int(old_row,1),1);
    EXPECT_EQ(sqlite3_column_int(old_row,2),102);
    sqlite3_finalize(old_row);
    sqlite3_stmt* watermark=nullptr;
    ASSERT_EQ(sqlite3_prepare_v2(old_lease->Database(),
        "SELECT last_scanned_height,scan_complete FROM sync_meta WHERE id=1",
        -1,&watermark,nullptr),SQLITE_OK);
    ASSERT_EQ(sqlite3_step(watermark),SQLITE_ROW);
    EXPECT_EQ(sqlite3_column_int(watermark,0),103);
    EXPECT_EQ(sqlite3_column_int(watermark,1),1);
    sqlite3_finalize(watermark);
    EXPECT_EQ(old_lease->LegacyTransparentState(),WalletManager::LegacyTransparentState::Complete);
    sqlite3_stmt* catalog=nullptr;
    ASSERT_EQ(sqlite3_prepare_v2(old_lease->Database(),
        "SELECT count(*) FROM settings WHERE key IN ('wallet_initial_owner_v1','orchard_account_catalog_v1')",
        -1,&catalog,nullptr),SQLITE_OK);
    ASSERT_EQ(sqlite3_step(catalog),SQLITE_ROW);
    EXPECT_EQ(sqlite3_column_int(catalog,0),0);
    sqlite3_finalize(catalog);
    sqlite3_stmt* created=nullptr;
    ASSERT_EQ(sqlite3_prepare_v2(old_lease->Database(),
        "SELECT amount,is_coinbase FROM utxos WHERE height=? AND is_spent=0",
        -1,&created,nullptr),SQLITE_OK);
    ASSERT_EQ(sqlite3_bind_int(created,1,102),SQLITE_OK);
    ASSERT_EQ(sqlite3_step(created),SQLITE_ROW);
    EXPECT_EQ(sqlite3_column_int64(created,0),int64_t(historical_amount-600000));
    EXPECT_EQ(sqlite3_column_int(created,1),0);
    EXPECT_EQ(sqlite3_step(created),SQLITE_DONE);
    sqlite3_finalize(created);
    sqlite3_stmt* history=nullptr;
    ASSERT_EQ(sqlite3_prepare_v2(old_lease->Database(),
        "SELECT amount,category,height FROM transactions WHERE txid=?",
        -1,&history,nullptr),SQLITE_OK);
    ASSERT_EQ(sqlite3_bind_text(history,1,shield_txid.c_str(),-1,SQLITE_TRANSIENT),SQLITE_OK);
    ASSERT_EQ(sqlite3_step(history),SQLITE_ROW);
    EXPECT_NEAR(sqlite3_column_double(history,0),-0.006,0.00000001);
    EXPECT_STREQ(reinterpret_cast<const char*>(sqlite3_column_text(history,1)),"send");
    EXPECT_EQ(sqlite3_column_int(history,2),102);
    EXPECT_EQ(sqlite3_step(history),SQLITE_DONE);
    sqlite3_finalize(history);
    ASSERT_EQ(sqlite3_prepare_v2(old_lease->Database(),
        "SELECT amount,is_coinbase FROM utxos WHERE height=? AND is_spent=0",
        -1,&created,nullptr),SQLITE_OK);
    ASSERT_EQ(sqlite3_bind_int(created,1,103),SQLITE_OK);
    ASSERT_EQ(sqlite3_step(created),SQLITE_ROW);
    EXPECT_EQ(sqlite3_column_int64(created,0),int64_t(amount));
    EXPECT_EQ(sqlite3_column_int(created,1),1);
    EXPECT_EQ(sqlite3_step(created),SQLITE_DONE);
    sqlite3_finalize(created);
    old_lease.reset();
    EXPECT_NEAR(use->Wallet().getBalance().total,
        historical_total-0.006+double(amount)/100000000.0,0.00000001);
    auto index=ChainstateService::AcquireWalletIndexUse(f.f.service);
    const auto session=use->Wallet().AcquireDatabaseLease()->Session();
    const auto ordinary=RuntimeOrdinaryDelivery::ReadForWallet(use->Wallet(),session);
    const auto indexed=RuntimeIndexDelivery::ReadForWallet(use->Wallet(),index->Index(),session);
    ASSERT_TRUE(ordinary);ASSERT_TRUE(indexed);
    EXPECT_EQ(ordinary->cursor,indexed->cursor);
    EXPECT_EQ(ordinary->tip_height,103u);
    EXPECT_EQ(ordinary->tip_hash,f.f.service->GetActiveTip()->hash);
    // Later typed blocks must keep this exact transparent-only class usable.
    // A second Start reopens the wallet, so this also checks the durable
    // receipts and maturity height after a fresh database session.
    index.reset();use.reset();
    const auto next_amount=f.MineToWallet();
    ASSERT_EQ(f.f.service->GetActiveTip()->height,104u);
    ASSERT_TRUE(f.wallet->Start());
    use=WalletService::AcquireWalletUse(f.wallet);
    ASSERT_TRUE(use->Wallet().WaitForHeight(104,std::chrono::seconds(20)));
    EXPECT_EQ(use->Wallet().getCurrentWalletName(),"canonical-recovery");
    EXPECT_NEAR(use->Wallet().getBalance().total,
        historical_total-0.006+double(amount+next_amount)/100000000.0,0.00000001);
    index=ChainstateService::AcquireWalletIndexUse(f.f.service);
    const auto reopened=use->Wallet().AcquireDatabaseLease()->Session();
    const auto next_ordinary=RuntimeOrdinaryDelivery::ReadForWallet(use->Wallet(),reopened);
    const auto next_indexed=RuntimeIndexDelivery::ReadForWallet(use->Wallet(),index->Index(),reopened);
    ASSERT_TRUE(next_ordinary);ASSERT_TRUE(next_indexed);
    EXPECT_EQ(next_ordinary->cursor,next_indexed->cursor);
    EXPECT_EQ(next_ordinary->tip_height,104u);
    EXPECT_EQ(next_ordinary->tip_hash,f.f.service->GetActiveTip()->hash);
    ExecutionContext request;request.daemon=&f.context;request.walletName="canonical-recovery";
    din::Json mined_request=din::obj();mined_request["type"]="mined";mined_request["count"]=10;
    const auto mined=rpc_context_wallet_listtransactions(request,mined_request);
    ASSERT_TRUE(mined.isArray()) << mined.toStyledString();
    ASSERT_EQ(mined.size(),2u) << mined.toStyledString();
    for (const auto& entry:mined) {
        EXPECT_TRUE(entry["is_coinbase"].asBool());
        EXPECT_EQ(entry["type"].asString(),"mined");
        EXPECT_EQ(entry["address"].asString(),f.receive_address);
        EXPECT_GT(entry["amount"].asDouble(),0.0);
    }
}
TEST(WalletCanonicalRecovery, V3IndexWritesOnlySelectedWalletAndPreservesForeignSameOutpoint) {
    CanonicalRecoveryFixture f(false,true);
    (void)f.PrepareFundedHistoricalWallet();
    f.DowngradeToPreIdentityTransparentWallet();
    const auto mined_amount=f.MineToWallet();
    ASSERT_EQ(f.f.service->GetActiveTip()->height,102u);
    auto use=WalletService::AcquireWalletUse(f.wallet);
    uint64_t session=0;
    {
        auto lease=use->Wallet().AcquireDatabaseLease();
        session=lease->Session();
        EXPECT_FALSE(lease->BeginLegacyTransparentMigration().empty());
        EXPECT_EQ(lease->LegacyTransparentState(),WalletManager::LegacyTransparentState::Pending);
    }
    auto lifetime=ChainstateService::AcquireWalletIndexUse(f.f.service);
    const auto replay=f.f.service->getRuntimeAccountReplayForWallet(use->Wallet(),session);
    ASSERT_TRUE(replay.ok());
    const auto origin=f.f.service->getRuntimeWalletOrigin(use->Wallet(),session,nullptr,true);
    ASSERT_TRUE(origin.ok());
    const auto coverage=RuntimeWalletCoverageProjection::Build(*origin,*replay);
    ASSERT_EQ(coverage->TipHeight(),102u);
    const auto address=DecodeWitnessAddress(f.receive_address,"rdin");
    ASSERT_TRUE(address.is_valid);
    std::optional<TxOutPoint> owned;
    for(const auto& [point,coin]:coverage->Coins())
        if(coin.height==102 && coin.coinbase && coin.output.scriptPubKey==address.script_pubkey)owned=point;
    ASSERT_TRUE(owned);
    const auto index_path=f.f.path/"v3-wallet-index-only";
    {UTXOIndex v2(index_path.string());ASSERT_TRUE(v2.Initialize());}
    const auto migrated=UpgradeFullArchiveWalletIndexSchemaToV3(index_path);
    ASSERT_TRUE(std::filesystem::is_regular_file(migrated.rollback_backup));
    auto metadata=std::make_unique<UTXOIndex>(index_path.string());
    ASSERT_TRUE(metadata->InitializeFullArchiveV3MetadataOnly());
    const auto id=owned->txid.AsUint256().GetHex();
    sqlite3* raw=nullptr;
    ASSERT_EQ(sqlite3_open_v2(index_path.string().c_str(),&raw,SQLITE_OPEN_READWRITE,nullptr),SQLITE_OK);
    const std::string foreign(32,'B');
    sqlite3_stmt* insert=nullptr;
    ASSERT_EQ(sqlite3_prepare_v2(raw,"INSERT INTO wallet_utxos(wallet_identity,txid,vout,value,spk,path,height,is_coinbase) "
        "VALUES(?,?,?,?,?,'m/86/1448/0/0/0',102,1)",-1,&insert,nullptr),SQLITE_OK);
    ASSERT_EQ(sqlite3_bind_blob(insert,1,foreign.data(),int(foreign.size()),SQLITE_TRANSIENT),SQLITE_OK);
    ASSERT_EQ(sqlite3_bind_text(insert,2,id.c_str(),-1,SQLITE_TRANSIENT),SQLITE_OK);
    ASSERT_EQ(sqlite3_bind_int(insert,3,owned->vout),SQLITE_OK);
    ASSERT_EQ(sqlite3_bind_int(insert,4,1),SQLITE_OK);
    ASSERT_EQ(sqlite3_bind_blob(insert,5,address.script_pubkey.data(),int(address.script_pubkey.size()),SQLITE_TRANSIENT),SQLITE_OK);
    ASSERT_EQ(sqlite3_step(insert),SQLITE_DONE);
    sqlite3_finalize(insert);
    ASSERT_EQ(sqlite3_prepare_v2(raw,"INSERT INTO wallet_index_receipts_v3(wallet_identity,receipt,invalidated) VALUES(?,x'464F524549474E',0)",
        -1,&insert,nullptr),SQLITE_OK);
    ASSERT_EQ(sqlite3_bind_blob(insert,1,foreign.data(),int(foreign.size()),SQLITE_TRANSIENT),SQLITE_OK);
    ASSERT_EQ(sqlite3_step(insert),SQLITE_DONE);
    sqlite3_finalize(insert);
    ASSERT_EQ(sqlite3_close(raw),SQLITE_OK);
    const auto fence=[&](const std::function<void()>& commit) {
        auto selected=f.f.service->AcquireBlockIngressActivationLock();
        const auto tip=f.f.service->GetActiveTip();
        if(!tip || tip->height!=coverage->TipHeight() || tip->hash!=coverage->TipHash())
            throw std::runtime_error("V3 source tip moved");
        commit();
    };
    // Product routing holds activation before wallet/index ownership. The
    // callback's recursive recheck is the final commit fence under that lock.
    auto selected_for_reconcile=f.f.service->AcquireBlockIngressActivationLock();
    EXPECT_THROW(RuntimeIndexDelivery::ReconcileCoverageV3(use->Wallet(),*metadata,*coverage,[](const auto&) {
        throw std::runtime_error("injected source fence failure");
    }),std::runtime_error);
    EXPECT_THROW(RuntimeIndexDelivery::ReconcileCoverageV3(use->Wallet(),*metadata,*coverage,[](const auto&) {
        // A caller cannot publish a receipt after dropping the source lock.
    }),std::runtime_error);
    ASSERT_EQ(sqlite3_open_v2(index_path.string().c_str(),&raw,SQLITE_OPEN_READONLY,nullptr),SQLITE_OK);
    sqlite3_stmt* rolled_back=nullptr;
    ASSERT_EQ(sqlite3_prepare_v2(raw,"SELECT count(*) FROM wallet_utxos WHERE txid=? AND vout=?",
        -1,&rolled_back,nullptr),SQLITE_OK);
    ASSERT_EQ(sqlite3_bind_text(rolled_back,1,id.c_str(),-1,SQLITE_TRANSIENT),SQLITE_OK);
    ASSERT_EQ(sqlite3_bind_int(rolled_back,2,owned->vout),SQLITE_OK);
    ASSERT_EQ(sqlite3_step(rolled_back),SQLITE_ROW);EXPECT_EQ(sqlite3_column_int(rolled_back,0),1);
    sqlite3_finalize(rolled_back);ASSERT_EQ(sqlite3_close(raw),SQLITE_OK);
    const auto result=RuntimeIndexDelivery::ReconcileCoverageV3(use->Wallet(),*metadata,*coverage,fence);
    EXPECT_EQ(result.tip_height,102u);
    EXPECT_EQ(result.tip_hash,coverage->TipHash());
    const auto receipt=RuntimeIndexDelivery::ReadForWalletV3(use->Wallet(),*metadata,session);
    ASSERT_TRUE(receipt);EXPECT_EQ(receipt->tip_height,102u);
    {
        auto lease=use->Wallet().AcquireDatabaseLease();
        struct Reads { int receipts=0; } reads;
        auto* opened=RuntimeV3ReadTestAccess::Connection(*metadata);
        ASSERT_EQ(sqlite3_trace_v2(opened,SQLITE_TRACE_STMT,[](unsigned action,void* context,
                void* statement,void*) {
            auto& observed=*static_cast<Reads*>(context);
            const auto* sql=action==SQLITE_TRACE_STMT
                ? sqlite3_sql(static_cast<sqlite3_stmt*>(statement)):nullptr;
            if(sql && std::string_view(sql).find("FROM wallet_index_receipts_v3")!=std::string_view::npos)
                ++observed.receipts;
            return 0;
        },&reads),SQLITE_OK);
        for(int n=0;n<32;++n)
            EXPECT_TRUE(RuntimeV3IndexWalletTipCurrent(*metadata,*lease,coverage->TipHash(),102));
        ASSERT_EQ(sqlite3_trace_v2(opened,0,nullptr,nullptr),SQLITE_OK);
        EXPECT_EQ(reads.receipts,32) << "Each receipt read must use the verified index connection once";
        EXPECT_FALSE(RuntimeV3IndexWalletTipCurrent(*metadata,*lease,coverage->TipHash(),103));
    }
    ASSERT_EQ(sqlite3_open_v2(index_path.string().c_str(),&raw,SQLITE_OPEN_READWRITE,nullptr),SQLITE_OK);
    sqlite3_stmt* invalidate=nullptr;
    ASSERT_EQ(sqlite3_prepare_v2(raw,"UPDATE wallet_utxos SET height=height WHERE wallet_identity<>? AND txid=? AND vout=?",
        -1,&invalidate,nullptr),SQLITE_OK);
    ASSERT_EQ(sqlite3_bind_blob(invalidate,1,foreign.data(),int(foreign.size()),SQLITE_TRANSIENT),SQLITE_OK);
    ASSERT_EQ(sqlite3_bind_text(invalidate,2,id.c_str(),-1,SQLITE_TRANSIENT),SQLITE_OK);
    ASSERT_EQ(sqlite3_bind_int(invalidate,3,owned->vout),SQLITE_OK);
    ASSERT_EQ(sqlite3_step(invalidate),SQLITE_DONE);
    EXPECT_EQ(sqlite3_changes(raw),1);
    sqlite3_finalize(invalidate);ASSERT_EQ(sqlite3_close(raw),SQLITE_OK);
    EXPECT_THROW((void)RuntimeIndexDelivery::ReadForWalletV3(use->Wallet(),*metadata,session),std::runtime_error);
    {
        auto lease=use->Wallet().AcquireDatabaseLease();
        EXPECT_THROW((void)RuntimeV3IndexWalletTipCurrent(*metadata,*lease,coverage->TipHash(),102),std::runtime_error);
    }
    const auto retried=RuntimeIndexDelivery::ReconcileCoverageV3(use->Wallet(),*metadata,*coverage,fence);
    EXPECT_EQ(retried.tip_height,102u);
    ASSERT_TRUE(RuntimeIndexDelivery::ReadForWalletV3(use->Wallet(),*metadata,session));
    ASSERT_EQ(sqlite3_open_v2(index_path.string().c_str(),&raw,SQLITE_OPEN_READONLY,nullptr),SQLITE_OK);
    sqlite3_stmt* count=nullptr;
    ASSERT_EQ(sqlite3_prepare_v2(raw,"SELECT value FROM wallet_utxos WHERE txid=? AND vout=? ORDER BY value",
        -1,&count,nullptr),SQLITE_OK);
    ASSERT_EQ(sqlite3_bind_text(count,1,id.c_str(),-1,SQLITE_TRANSIENT),SQLITE_OK);
    ASSERT_EQ(sqlite3_bind_int(count,2,owned->vout),SQLITE_OK);
    ASSERT_EQ(sqlite3_step(count),SQLITE_ROW);EXPECT_EQ(sqlite3_column_int64(count,0),1);
    ASSERT_EQ(sqlite3_step(count),SQLITE_ROW);EXPECT_EQ(sqlite3_column_int64(count,0),int64_t(mined_amount));
    EXPECT_EQ(sqlite3_step(count),SQLITE_DONE);sqlite3_finalize(count);
    ASSERT_EQ(sqlite3_prepare_v2(raw,"SELECT hex(receipt),invalidated FROM wallet_index_receipts_v3 WHERE wallet_identity=?",
        -1,&count,nullptr),SQLITE_OK);
    ASSERT_EQ(sqlite3_bind_blob(count,1,foreign.data(),int(foreign.size()),SQLITE_TRANSIENT),SQLITE_OK);
    ASSERT_EQ(sqlite3_step(count),SQLITE_ROW);
    EXPECT_STREQ(reinterpret_cast<const char*>(sqlite3_column_text(count,0)),"464F524549474E");
    EXPECT_EQ(sqlite3_column_int(count,1),0);
    EXPECT_EQ(sqlite3_step(count),SQLITE_DONE);sqlite3_finalize(count);
    ASSERT_EQ(sqlite3_close(raw),SQLITE_OK);
    EXPECT_EQ(use->Wallet().AcquireDatabaseLease()->LegacyTransparentState(),
        WalletManager::LegacyTransparentState::Pending);
    EXPECT_THROW((void)use->Wallet().getBalance(),std::runtime_error);
    int source_commits=0;
    EXPECT_THROW(RuntimeOrdinaryDelivery::ReconcileLegacyTransparentCoverageV3(
        use->Wallet(),*metadata,*coverage,[&](const auto& commit) {
            auto selected=f.f.service->AcquireBlockIngressActivationLock();
            const auto tip=f.f.service->GetActiveTip();
            if(!tip||tip->height!=102||tip->hash!=coverage->TipHash())
                throw std::runtime_error("V3 source tip moved");
            if(++source_commits==2)throw std::runtime_error("injected ordinary commit failure");
            commit();
        }),std::runtime_error);
    EXPECT_EQ(source_commits,2);
    EXPECT_EQ(use->Wallet().AcquireDatabaseLease()->LegacyTransparentState(),
        WalletManager::LegacyTransparentState::Pending);
    EXPECT_FALSE(RuntimeOrdinaryDelivery::ReadForWallet(use->Wallet(),session));
    // Simulate a process restart after index commit and before ordinary seal.
    metadata.reset();
    metadata=std::make_unique<UTXOIndex>(index_path.string());
    ASSERT_TRUE(metadata->InitializeFullArchiveV3MetadataOnly());
    ASSERT_TRUE(RuntimeIndexDelivery::ReadForWalletV3(use->Wallet(),*metadata,session));
    RuntimeOrdinaryDelivery::ReconcileLegacyTransparentCoverageV3(use->Wallet(),*metadata,*coverage,fence);
    EXPECT_EQ(use->Wallet().AcquireDatabaseLease()->LegacyTransparentState(),
        WalletManager::LegacyTransparentState::Complete);
    const auto ordinary=RuntimeOrdinaryDelivery::ReadForWallet(use->Wallet(),session);
    ASSERT_TRUE(ordinary);
    EXPECT_EQ(ordinary->cursor,coverage->Head());
    EXPECT_EQ(ordinary->tip_height,102u);
    EXPECT_EQ(ordinary->tip_hash,coverage->TipHash());
    ASSERT_EQ(sqlite3_open_v2(index_path.string().c_str(),&raw,SQLITE_OPEN_READONLY,nullptr),SQLITE_OK);
    ASSERT_EQ(sqlite3_prepare_v2(raw,"SELECT hex(receipt),invalidated FROM wallet_index_receipts_v3 WHERE wallet_identity=?",
        -1,&count,nullptr),SQLITE_OK);
    ASSERT_EQ(sqlite3_bind_blob(count,1,foreign.data(),int(foreign.size()),SQLITE_TRANSIENT),SQLITE_OK);
    ASSERT_EQ(sqlite3_step(count),SQLITE_ROW);
    EXPECT_STREQ(reinterpret_cast<const char*>(sqlite3_column_text(count,0)),"464F524549474E");
    EXPECT_EQ(sqlite3_column_int(count,1),0);
    EXPECT_EQ(sqlite3_step(count),SQLITE_DONE);sqlite3_finalize(count);
    ASSERT_EQ(sqlite3_close(raw),SQLITE_OK);
    selected_for_reconcile.unlock();
    const auto next_amount=f.MineToWallet();
    ASSERT_EQ(f.f.service->GetActiveTip()->height,103u);
    const auto page=f.f.service->getRuntimeDeliveryPage(coverage->Head(),1);
    ASSERT_TRUE(page.ok());
    ASSERT_EQ((*page)->events.size(),1u);
    const auto event=(*page)->events.front();
    ASSERT_EQ(event.context.height,103u);
    const auto next_fence=[&](const std::function<void()>& commit) {
        auto selected=f.f.service->AcquireBlockIngressActivationLock();
        const auto current=f.f.service->getRuntimeDeliveryPage(coverage->Head(),1);
        if(!current.ok() || (*current)->events.size()!=1 || (*current)->events.front().cursor!=event.cursor ||
           (*current)->head!=event.cursor || !f.f.service->GetActiveTip() ||
           f.f.service->GetActiveTip()->hash!=event.context.block_hash)
            throw std::runtime_error("V3 incremental source moved");
        commit();
    };
    {
        auto selected=f.f.service->AcquireBlockIngressActivationLock();
        EXPECT_THROW(RuntimeIndexDelivery::ApplyForWalletV3(use->Wallet(),*metadata,session,event,
            [](const auto&) {}),std::runtime_error);
        EXPECT_EQ(RuntimeIndexDelivery::ReadForWalletV3(use->Wallet(),*metadata,session)->tip_height,102u);
        const auto advanced=RuntimeIndexDelivery::ApplyForWalletV3(use->Wallet(),*metadata,session,event,next_fence);
        EXPECT_EQ(advanced.cursor,event.cursor);
        EXPECT_EQ(advanced.tip_height,103u);
    }
    EXPECT_EQ(RuntimeIndexDelivery::ReadForWalletV3(use->Wallet(),*metadata,session)->tip_height,103u);
    {
        auto lease=use->Wallet().AcquireDatabaseLease();
        EXPECT_TRUE(RuntimeV3IndexWalletTipCurrent(*metadata,*lease,event.context.block_hash,103));
    }
    EXPECT_EQ(RuntimeOrdinaryDelivery::ReadForWallet(use->Wallet(),session)->tip_height,102u);
    metadata.reset();
    metadata=std::make_unique<UTXOIndex>(index_path.string());
    ASSERT_TRUE(metadata->InitializeFullArchiveV3MetadataOnly());
    {
        auto selected=f.f.service->AcquireBlockIngressActivationLock();
        EXPECT_EQ(RuntimeIndexDelivery::ApplyForWalletV3(use->Wallet(),*metadata,session,event,next_fence).cursor,
            event.cursor);
        EXPECT_EQ(RuntimeOrdinaryDelivery::ApplyForWallet(use->Wallet(),session,event).tip_height,103u);
    }
    const auto published=RuntimeOrdinaryDelivery::ReadForWallet(use->Wallet(),session);
    ASSERT_TRUE(published);
    EXPECT_EQ(published->cursor,event.cursor);
    EXPECT_EQ(published->tip_height,103u);
    EXPECT_EQ(use->Wallet().AcquireDatabaseLease()->LegacyTransparentState(),
        WalletManager::LegacyTransparentState::Complete);
    EXPECT_THROW((void)use->Wallet().getBalance(),std::runtime_error);
    use->Wallet().PublishRecoveredBlockchainHeight(use->Wallet().getBlockchainHeight(),103,session);
    EXPECT_EQ(use->Wallet().getBlockchainHeight(),103u);
    EXPECT_NO_THROW((void)use->Wallet().getBalance());
    ASSERT_EQ(sqlite3_open_v2(index_path.string().c_str(),&raw,SQLITE_OPEN_READONLY,nullptr),SQLITE_OK);
    ASSERT_EQ(sqlite3_prepare_v2(raw,"SELECT count(*),sum(value) FROM wallet_utxos WHERE height=103",
        -1,&count,nullptr),SQLITE_OK);
    ASSERT_EQ(sqlite3_step(count),SQLITE_ROW);
    EXPECT_EQ(sqlite3_column_int(count,0),1);
    EXPECT_EQ(sqlite3_column_int64(count,1),int64_t(next_amount));
    sqlite3_finalize(count);
    ASSERT_EQ(sqlite3_prepare_v2(raw,"SELECT hex(receipt),invalidated FROM wallet_index_receipts_v3 WHERE wallet_identity=?",
        -1,&count,nullptr),SQLITE_OK);
    ASSERT_EQ(sqlite3_bind_blob(count,1,foreign.data(),int(foreign.size()),SQLITE_TRANSIENT),SQLITE_OK);
    ASSERT_EQ(sqlite3_step(count),SQLITE_ROW);
    EXPECT_STREQ(reinterpret_cast<const char*>(sqlite3_column_text(count,0)),"464F524549474E");
    EXPECT_EQ(sqlite3_column_int(count,1),0);
    sqlite3_finalize(count);ASSERT_EQ(sqlite3_close(raw),SQLITE_OK);
    lifetime.reset(); // Test-only source swap requires no outstanding index use.
    struct RestoreSourceIndex {
        ChainstateService& source;
        std::unique_ptr<UTXOIndex>& v3;
        std::unique_ptr<UTXOIndex> original;
        ~RestoreSourceIndex() {
            v3=ShieldedStateStartupTestAccess::SwapWalletIndexForRpc(source,std::move(original));
        }
    };
    {
        RestoreSourceIndex restore{*f.f.service,metadata,
            ShieldedStateStartupTestAccess::SwapWalletIndexForRpc(*f.f.service,std::move(metadata))};
        struct Reads { int receipts=0; } reads;
        auto* active=f.f.service->utxoIndex();
        ASSERT_TRUE(active);
        auto* opened=RuntimeV3ReadTestAccess::Connection(*active);
        ASSERT_EQ(sqlite3_trace_v2(opened,SQLITE_TRACE_STMT,[](unsigned action,void* context,
                void* statement,void*) {
            auto& observed=*static_cast<Reads*>(context);
            const auto* sql=action==SQLITE_TRACE_STMT
                ? sqlite3_sql(static_cast<sqlite3_stmt*>(statement)):nullptr;
            if(sql && std::string_view(sql).find("FROM wallet_index_receipts_v3")!=std::string_view::npos)
                ++observed.receipts;
            return 0;
        },&reads),SQLITE_OK);
        ExecutionContext request;request.daemon=&f.context;request.walletName="canonical-recovery";
        for(int n=0;n<16;++n) {
            const auto balance=rpc_context_wallet_getbalance(request,din::obj());
            EXPECT_FALSE(balance.isMember("error")) << balance.toStyledString();
            EXPECT_GT(balance["total"].asDouble(),0.0) << balance.toStyledString();
        }
        ASSERT_EQ(sqlite3_trace_v2(opened,0,nullptr,nullptr),SQLITE_OK);
        // getbalance fences the selected source before and after assembling
        // its response; each of the 16 calls reads this owner's receipt twice.
        EXPECT_EQ(reads.receipts,32);
        // Both markers were verified when the v3 handle opened. Replacing a
        // marker with another well-formed value after open must not turn the
        // fast per-RPC guard into a new trust anchor.
        const auto refuse_finance=[&] {
            for(const auto& response : {rpc_context_wallet_getbalance(request,din::obj()),
                                        rpc_context_wallet_getwalletinfo(request,din::obj())}) {
                EXPECT_EQ(response["error_code"].asString(),"wallet_balance_not_current")
                    << response.toStyledString();
                for(const auto* amount:{"total","confirmed","balance","confirmed_balance",
                                        "immature","spendable"})
                    EXPECT_FALSE(response.isMember(amount)) << amount << response.toStyledString();
            }
        };
        constexpr const char* backup_key="wallet_index_v3:rollback_backup_digest";
        constexpr const char* guard_key="wallet_index_v3:legacy_guard_digest";
        ASSERT_EQ(sqlite3_open_v2(index_path.string().c_str(),&raw,SQLITE_OPEN_READWRITE,nullptr),SQLITE_OK);
        const auto original_backup=TestV3MetadataText(raw,backup_key);
        const auto substituted_backup=original_backup==std::string(64,'a')
            ?std::string(64,'b'):std::string(64,'a');
        TestV3SetMetadata(raw,backup_key,substituted_backup);
        ASSERT_EQ(sqlite3_close(raw),SQLITE_OK);raw=nullptr;
        refuse_finance();
        ASSERT_EQ(sqlite3_open_v2(index_path.string().c_str(),&raw,SQLITE_OPEN_READWRITE,nullptr),SQLITE_OK);
        TestV3SetMetadata(raw,backup_key,original_backup);
        // A guard definition and its stored digest can be changed together.
        // The digest still self-consistently validates, but no longer matches
        // the exact legacy guard set authenticated at handle open.
        const auto original_guard=TestV3MetadataText(raw,guard_key);
        ASSERT_EQ(sqlite3_exec(raw,
            "CREATE INDEX v3_test_legacy_extra_guard ON wallet_utxos_legacy_v2(path,height)",
            nullptr,nullptr,nullptr),SQLITE_OK);
        TestV3SetMetadata(raw,guard_key,TestV3LegacyGuardDigest(raw));
        ASSERT_EQ(sqlite3_close(raw),SQLITE_OK);raw=nullptr;
        refuse_finance();
        ASSERT_EQ(sqlite3_open_v2(index_path.string().c_str(),&raw,SQLITE_OPEN_READWRITE,nullptr),SQLITE_OK);
        ASSERT_EQ(sqlite3_exec(raw,"DROP INDEX v3_test_legacy_extra_guard",nullptr,nullptr,nullptr),SQLITE_OK);
        TestV3SetMetadata(raw,guard_key,original_guard);
        ASSERT_EQ(sqlite3_close(raw),SQLITE_OK);raw=nullptr;
        const auto restored=rpc_context_wallet_getbalance(request,din::obj());
        EXPECT_FALSE(restored.isMember("error")) << restored.toStyledString();
        EXPECT_GT(restored["total"].asDouble(),0.0) << restored.toStyledString();
    }
    ASSERT_EQ(sqlite3_open_v2(index_path.string().c_str(),&raw,SQLITE_OPEN_READWRITE,nullptr),SQLITE_OK);
    ASSERT_EQ(sqlite3_exec(raw,"DROP TRIGGER runtime_delivery_v3_update",nullptr,nullptr,nullptr),SQLITE_OK);
    ASSERT_EQ(sqlite3_close(raw),SQLITE_OK);
    {
        auto lease=use->Wallet().AcquireDatabaseLease();
        EXPECT_THROW((void)RuntimeV3IndexWalletTipCurrent(*metadata,*lease,event.context.block_hash,103),
            std::runtime_error);
    }
}
TEST(WalletCanonicalRecovery, V3RpcRefusesUnenrolledWalletWithoutOwnerReceipt) {
    CanonicalRecoveryFixture f(false,true);
    (void)f.PrepareFundedHistoricalWallet();
    f.DowngradeToPreIdentityTransparentWallet();
    (void)f.MineToWallet();
    ASSERT_EQ(f.f.service->GetActiveTip()->height,102u);
    f.wallet->get().setBlockchainHeight(102);
    ExecutionContext request;request.daemon=&f.context;request.walletName="canonical-recovery";
    {
        auto lease=f.wallet->get().AcquireDatabaseLease();
        ASSERT_EQ(lease->LegacyTransparentState(),WalletManager::LegacyTransparentState::Absent);
    }
    const auto old=rpc_context_wallet_getbalance(request,din::obj());
    ASSERT_GT(old["total"].asDouble(),0.0) << old.toStyledString();
    const auto index_path=f.f.path/"v3-absent-rpc";
    {UTXOIndex v2(index_path.string());ASSERT_TRUE(v2.Initialize());}
    (void)UpgradeFullArchiveWalletIndexSchemaToV3(index_path);
    auto metadata=std::make_unique<UTXOIndex>(index_path.string());
    ASSERT_TRUE(metadata->InitializeFullArchiveV3MetadataOnly());
    struct Restore {
        ChainstateService& service;
        std::unique_ptr<UTXOIndex> previous;
        ~Restore() {
            auto temporary=ShieldedStateStartupTestAccess::SwapWalletIndexForRpc(
                service,std::move(previous));
            (void)temporary;
        }
    } restore{*f.f.service,ShieldedStateStartupTestAccess::SwapWalletIndexForRpc(
        *f.f.service,std::move(metadata))};
    for (const auto& refused : {rpc_context_wallet_getbalance(request,din::obj()),
                                rpc_context_wallet_getwalletinfo(request,din::obj()),
                                rpc_context_wallet_snapshot(request,din::obj())}) {
        EXPECT_EQ(refused["error_code"].asString(),"wallet_balance_not_current")
            << refused.toStyledString();
        EXPECT_FALSE(refused.isMember("total")) << refused.toStyledString();
        EXPECT_FALSE(refused.isMember("confirmed")) << refused.toStyledString();
    }
}
TEST(WalletCanonicalRecovery, WalletSwitchNeverClaimsAnotherWalletIndexReceipt) {
    CanonicalRecoveryFixture f(false,true);
    (void)f.PrepareFundedHistoricalWallet();
    f.DowngradeToPreIdentityTransparentWallet();
    const auto primary_session=f.Session();
    std::string other_address;
    {
        auto use=WalletService::AcquireWalletUse(f.wallet);
        use->Wallet().create("second-legacy-wallet");
        other_address=use->Wallet().getNewAddress();
    }
    f.DowngradeToPreIdentityTransparentWallet();
    const auto secondary_session=f.Session();
    ASSERT_NE(primary_session,secondary_session);
    ASSERT_NE(other_address,f.receive_address);
    // The two wallets share one selected source but have disjoint watched
    // scripts, histories, receipts, and active-wallet sessions.
    const auto receiver=orchard::WalletKeys::FromSeed(std::array<uint8_t,64>{57},0);
    {
        auto use=WalletService::AcquireWalletUse(f.wallet);
        use->Wallet().open("canonical-recovery");
        // Explicit wallet switches clear the shared index's watched scripts.
        // Rebind the selected wallet's durable script domain as the daemon does.
        use->Wallet().LoadAddressesIntoUTXOIndex();
    }
    (void)f.Mine(f.ShieldHistoricalWalletCoin(receiver)); // H102 spends the first wallet's old coin.
    {
        BlockAssembler assembler(&f.f.db);WireOrchardAssembler(assembler,f.f);
        const auto block=assembler.CreateOrchardBlock(other_address);
        ASSERT_TRUE(bool(block));
        const auto result=f.Submit(block->WireBytes());ASSERT_TRUE(result.accepted()&&result.connected);
    }
    ASSERT_EQ(f.f.service->GetActiveTip()->height,103u);
    const auto primary_amount=f.MineToWallet(); // H104 credits only the first wallet.
    ASSERT_EQ(f.f.service->GetActiveTip()->height,104u);
    ASSERT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(),Recovery::AppliedPrefix);
    {
        auto use=WalletService::AcquireWalletUse(f.wallet);
        EXPECT_EQ(use->Wallet().getCurrentWalletName(),"canonical-recovery");
        EXPECT_EQ(use->Wallet().getBlockchainHeight(),104u);
        const auto primary=use->Wallet().getBalance();
        EXPECT_NEAR(primary.immature,double(primary_amount)/100000000.0,0.00000001);
        ExecutionContext request;request.daemon=&f.context;request.walletName="canonical-recovery";
        din::Json mined_request=din::obj();mined_request["type"]="mined";
        const auto mined=rpc_context_wallet_listtransactions(request,mined_request);
        ASSERT_TRUE(mined.isArray()) << mined.toStyledString();
        ASSERT_EQ(mined.size(),1u);EXPECT_GE(mined[0]["confirmations"].asInt(),1);
        auto lease=use->Wallet().AcquireDatabaseLease();
        EXPECT_EQ(lease->LegacyTransparentState(),WalletManager::LegacyTransparentState::Complete);
    }
    {
        auto use=WalletService::AcquireWalletUse(f.wallet);
        use->Wallet().open("second-legacy-wallet");
        use->Wallet().LoadAddressesIntoUTXOIndex();
        EXPECT_EQ(use->Wallet().getBlockchainHeight(),101u);
        auto lease=use->Wallet().AcquireDatabaseLease();
        EXPECT_EQ(lease->LegacyTransparentState(),WalletManager::LegacyTransparentState::Absent);
    }
    // The node's index currently has one persisted wallet identity. The
    // second wallet must remain pending rather than inherit or overwrite the
    // first wallet's authenticated index receipt or reward rows.
    ASSERT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(),Recovery::Deferred);
    {
        auto use=WalletService::AcquireWalletUse(f.wallet);
        EXPECT_EQ(use->Wallet().getCurrentWalletName(),"second-legacy-wallet");
        EXPECT_EQ(use->Wallet().getBlockchainHeight(),101u);
        ExecutionContext request;request.daemon=&f.context;request.walletName="second-legacy-wallet";
        din::Json mined_request=din::obj();mined_request["type"]="mined";
        const auto mined=rpc_context_wallet_listtransactions(request,mined_request);
        EXPECT_FALSE(mined.isArray()) << mined.toStyledString();
        const auto balance=rpc_context_wallet_getbalance(request,din::obj());
        EXPECT_EQ(balance["error_code"].asString(),"wallet_balance_not_current");
        EXPECT_FALSE(balance.isMember("total"));
        auto lease=use->Wallet().AcquireDatabaseLease();
        EXPECT_EQ(lease->LegacyTransparentState(),WalletManager::LegacyTransparentState::Pending);
    }
    {
        auto use=WalletService::AcquireWalletUse(f.wallet);
        use->Wallet().open("canonical-recovery");
        use->Wallet().LoadAddressesIntoUTXOIndex();
    }
    ASSERT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(),Recovery::AppliedPrefix);
    {
        auto use=WalletService::AcquireWalletUse(f.wallet);
        EXPECT_EQ(use->Wallet().getBlockchainHeight(),104u);
        const auto primary=use->Wallet().getBalance();
        EXPECT_NEAR(primary.immature,double(primary_amount)/100000000.0,0.00000001);
        ExecutionContext request;request.daemon=&f.context;request.walletName="canonical-recovery";
        din::Json mined_request=din::obj();mined_request["type"]="mined";
        const auto mined=rpc_context_wallet_listtransactions(request,mined_request);
        ASSERT_TRUE(mined.isArray()) << mined.toStyledString();
        ASSERT_EQ(mined.size(),1u);
        EXPECT_GE(mined[0]["confirmations"].asInt(),1);
    }
}
TEST(WalletCanonicalRecovery, EmptyPreactivationHistoryRecoversTwoRealCoinbasesAndRestart) {
    CanonicalRecoveryFixture f(false,false);
    f.DowngradeToPreIdentityTransparentWallet();
    const auto first=f.MineToWallet(); // H102, no preactivation wallet coins.
    const auto second=f.MineToWallet(); // H103, independent typed Orchard body.
    ASSERT_EQ(f.f.service->GetActiveTip()->height,103u);
    ASSERT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(),Recovery::AppliedPrefix);
    {
        auto use=WalletService::AcquireWalletUse(f.wallet);
        EXPECT_EQ(use->Wallet().getBlockchainHeight(),103u);
        const auto balance=use->Wallet().getBalance();
        EXPECT_NEAR(balance.confirmed,0.0,0.00000001);
        EXPECT_NEAR(balance.immature,double(first+second)/100000000.0,0.00000001);
        ExecutionContext request;request.daemon=&f.context;request.walletName="canonical-recovery";
        din::Json mined_request=din::obj();mined_request["type"]="mined";
        const auto mined=rpc_context_wallet_listtransactions(request,mined_request);
        ASSERT_TRUE(mined.isArray()) << mined.toStyledString();
        ASSERT_EQ(mined.size(),2u);
        EXPECT_NE(mined[0]["confirmations"].asInt(),mined[1]["confirmations"].asInt());
        for(const auto& row:mined) {
            EXPECT_TRUE(row["is_coinbase"].asBool());
            EXPECT_EQ(row["address"].asString(),f.receive_address);
            EXPECT_GT(row["time"].asInt64(),0);
        }
    }
    {
        auto use=WalletService::AcquireWalletUse(f.wallet);
        use->Wallet().open("canonical-recovery");
        use->Wallet().LoadAddressesIntoUTXOIndex();
    }
    ASSERT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(),Recovery::AppliedPrefix);
    {
        auto use=WalletService::AcquireWalletUse(f.wallet);
        EXPECT_EQ(use->Wallet().getBlockchainHeight(),103u);
        const auto balance=use->Wallet().getBalance();
        EXPECT_NEAR(balance.immature,double(first+second)/100000000.0,0.00000001);
        auto lease=use->Wallet().AcquireDatabaseLease();
        EXPECT_EQ(lease->LegacyTransparentState(),WalletManager::LegacyTransparentState::Complete);
    }
}
TEST(WalletCanonicalRecovery, EncryptedLegacyTransparentWalletWaitsForActualUnlock) {
    CanonicalRecoveryFixture f(true,true);
    (void)f.PrepareFundedHistoricalWallet();
    f.DowngradeToPreIdentityTransparentWallet();
    (void)f.MineToWallet();
    (void)f.MineToWallet();
    ASSERT_EQ(f.f.service->GetActiveTip()->height,103u);
    f.wallet->get().lockWallet();
    EXPECT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(),Recovery::Deferred);
    {
        auto lease=f.wallet->get().AcquireDatabaseLease();
        EXPECT_EQ(lease->LegacyTransparentState(),WalletManager::LegacyTransparentState::Absent);
    }
    // Model the interval after keys unlock but before canonical capture can
    // create Pending. If that capture refuses, the old H101 rows must not be
    // returned as a current H103 balance merely because the marker is absent.
    f.wallet->get().unlockWallet("canonical-fixture-pass",0);
    ExecutionContext request;request.daemon=&f.context;request.walletName="canonical-recovery";
    const auto before_migration=rpc_context_wallet_getbalance(request,din::obj());
    EXPECT_EQ(before_migration["error_code"].asString(),"wallet_balance_not_current")
        << before_migration.toStyledString();
    EXPECT_FALSE(before_migration.isMember("total"));
    din::Json unlock(Json::arrayValue);unlock.append("canonical-fixture-pass");unlock.append(0);
    const auto response=rpc_context_wallet_unlock(request,unlock);
    ASSERT_TRUE(response["success"].asBool()) << response.toStyledString();
    EXPECT_FALSE(response.isMember("recovery_warning")) << response.toStyledString();
    const auto session=f.wallet->get().AcquireDatabaseLease()->Session();
    {
        auto lease=f.wallet->get().AcquireDatabaseLease();
        EXPECT_EQ(lease->LegacyTransparentState(),WalletManager::LegacyTransparentState::Complete);
    }
    const auto receipt=RuntimeOrdinaryDelivery::ReadForWallet(f.wallet->get(),session);
    ASSERT_TRUE(receipt);
    EXPECT_EQ(receipt->tip_height,103u);
    EXPECT_EQ(receipt->tip_hash,f.f.service->GetActiveTip()->hash);
    EXPECT_EQ(f.wallet->get().getBlockchainHeight(),103u);
    EXPECT_GT(f.wallet->get().getBalance().total,0.0);
    din::Json mined_request=din::obj();mined_request["type"]="mined";
    const auto mined=rpc_context_wallet_listtransactions(request,mined_request);
    ASSERT_TRUE(mined.isArray()) << mined.toStyledString();
    ASSERT_EQ(mined.size(),2u);
    for(const auto& entry:mined)EXPECT_GT(entry["time"].asDouble(),0.0);
    f.wallet->get().lockWallet();
    f.wallet->get().open("canonical-recovery");
    EXPECT_TRUE(f.wallet->get().isLocked());
    EXPECT_EQ(f.wallet->get().getBlockchainHeight(),103u);
    EXPECT_GT(f.wallet->get().getBalance().total,0.0);
}
TEST(WalletCanonicalRecovery, EncryptedUnlockReportsSourceGapWithoutPublishingBalance) {
    CanonicalRecoveryFixture f(true,true);
    (void)f.PrepareFundedHistoricalWallet();
    f.DowngradeToPreIdentityTransparentWallet();
    (void)f.MineToWallet();
    ASSERT_EQ(f.f.db.deleteBlock(f.f.token,f.f.blocks[50].GetHash()),Status::Ok);
    f.wallet->get().lockWallet();
    ExecutionContext request;request.daemon=&f.context;request.walletName="canonical-recovery";
    din::Json unlock(Json::arrayValue);unlock.append("canonical-fixture-pass");unlock.append(0);
    const auto response=rpc_context_wallet_unlock(request,unlock);
    EXPECT_TRUE(response["success"].asBool()) << response.toStyledString();
    EXPECT_FALSE(response["recovery_warning"].asString().empty()) << response.toStyledString();
    EXPECT_FALSE(response.isMember("recovery_mode")) << response.toStyledString();
    EXPECT_FALSE(f.wallet->get().isLocked());
    auto lease=f.wallet->get().AcquireDatabaseLease();
    EXPECT_EQ(lease->LegacyTransparentState(),WalletManager::LegacyTransparentState::Pending);
    lease.reset();
    const auto balance=rpc_context_wallet_getbalance(request,din::obj());
    EXPECT_EQ(balance["error_code"].asString(),"wallet_balance_not_current") << balance.toStyledString();
    EXPECT_FALSE(balance.isMember("total"));
    const auto status=rpc_context_wallet_getstatus(request,din::obj());
    EXPECT_FALSE(status["balance_available"].asBool());
    EXPECT_FALSE(status.isMember("total_balance")) << status.toStyledString();
}
TEST(WalletCanonicalRecovery, CompletedTransparentWalletRefusesStaleSelectedTipBalances) {
    CanonicalRecoveryFixture f(false,true);
    (void)f.PrepareFundedHistoricalWallet();
    f.DowngradeToPreIdentityTransparentWallet();
    (void)f.MineToWallet();
    ASSERT_EQ(f.f.service->GetActiveTip()->height,102u);
    ASSERT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(),Recovery::AppliedPrefix);
    // The selected chain advances while the wallet service is not started.
    // A complete receipt for H102 cannot authorize an H103 balance response.
    f.MineEmpty();
    ASSERT_EQ(f.f.service->GetActiveTip()->height,103u);
    ExecutionContext request;request.daemon=&f.context;request.walletName="canonical-recovery";
    for (const auto& read : {rpc_context_wallet_getbalance(request,din::obj()),
                             rpc_context_wallet_getwalletinfo(request,din::obj()),
                             rpc_context_wallet_snapshot(request,din::obj())}) {
        EXPECT_TRUE(read.isMember("error")) << read.toStyledString();
        EXPECT_EQ(read["error_code"].asString(),"wallet_balance_not_current") << read.toStyledString();
        EXPECT_FALSE(read.isMember("total")) << read.toStyledString();
    }
    const auto stale_status=rpc_context_wallet_getstatus(request,din::obj());
    EXPECT_EQ(stale_status["error_code"].asString(),"wallet_balance_not_current")
        << stale_status.toStyledString();
    EXPECT_FALSE(stale_status["balance_available"].asBool());
    EXPECT_FALSE(stale_status.isMember("total_balance")) << stale_status.toStyledString();
    EXPECT_FALSE(stale_status.isMember("spendable_balance")) << stale_status.toStyledString();
    const auto stale_sync=rpc_context_wallet_getsyncstate(request,din::obj());
    EXPECT_EQ(stale_sync["error_code"].asString(),"wallet_balance_not_current")
        << stale_sync.toStyledString();
    EXPECT_FALSE(stale_sync["balance_available"].asBool());
    EXPECT_FALSE(stale_sync.isMember("balance_discovered")) << stale_sync.toStyledString();
    din::Json history_request=din::obj();history_request["type"]="mined";
    const auto refused_history=rpc_context_wallet_listtransactions(request,history_request);
    EXPECT_TRUE(refused_history.isObject()) << refused_history.toStyledString();
    if(refused_history.isObject())
        EXPECT_EQ(refused_history["error_code"].asString(),"wallet_balance_not_current")
            << refused_history.toStyledString();
    din::Json send=din::obj();send["address"]=f.receive_address;
    send["amount_una"]=Json::UInt64(100000);send["preview"]=true;
    const auto ordinary_invalid=[&] {
        auto lease=f.wallet->get().AcquireDatabaseLease();
        sqlite3_stmt* raw=nullptr;
        OrchardAdmissionFixture::Require(sqlite3_prepare_v2(lease->Database(),
            "SELECT runtime_ordinary_invalid FROM wallet_meta WHERE id=1",
            -1,&raw,nullptr)==SQLITE_OK);
        const std::unique_ptr<sqlite3_stmt,decltype(&sqlite3_finalize)> row(raw,sqlite3_finalize);
        OrchardAdmissionFixture::Require(sqlite3_step(row.get())==SQLITE_ROW);
        const int invalid=sqlite3_column_int(row.get(),0);
        OrchardAdmissionFixture::Require(sqlite3_step(row.get())==SQLITE_DONE);
        return invalid;
    };
    EXPECT_EQ(ordinary_invalid(),0);
    const auto pending_before=f.wallet->get().getPendingPayments().size();
    const auto mempool_before=f.f.ingress->mempool().size();
    const auto refused_send=rpc_context_wallet_sendtoaddress(request,send);
    EXPECT_EQ(refused_send["error_code"].asString(),"wallet_balance_not_current")
        << refused_send.toStyledString();
    EXPECT_FALSE(refused_send.isMember("txid"));
    EXPECT_EQ(f.wallet->get().getPendingPayments().size(),pending_before);
    EXPECT_EQ(f.f.ingress->mempool().size(),mempool_before);
    EXPECT_EQ(ordinary_invalid(),0) << "refused send changed the recovery receipt";
    ASSERT_TRUE(f.wallet->Start());
    auto use=WalletService::AcquireWalletUse(f.wallet);
    ASSERT_TRUE(use->Wallet().WaitForHeight(103,std::chrono::seconds(20)));
    const auto current=rpc_context_wallet_getbalance(request,din::obj());
    ASSERT_FALSE(current.isMember("error")) << current.toStyledString();
    EXPECT_EQ(current["walletname"].asString(),"canonical-recovery");
    EXPECT_EQ(current["wallet_scope"].asString(),"legacy_transparent_only");
    EXPECT_FALSE(current["send_ready"].asBool());
    const auto limited_send=rpc_context_wallet_sendtoaddress(request,send);
    EXPECT_EQ(limited_send["error_code"].asString(),"wallet_transparent_only_send_unavailable")
        << limited_send.toStyledString();
    EXPECT_FALSE(limited_send.isMember("txid"));
    EXPECT_EQ(f.wallet->get().getPendingPayments().size(),pending_before);
    EXPECT_EQ(f.f.ingress->mempool().size(),mempool_before);
    EXPECT_EQ(ordinary_invalid(),0) << "limited send changed the recovery receipt";
}
TEST(WalletCanonicalRecovery, MissingHistoricalBodyKeepsLegacyMigrationPending) {
    CanonicalRecoveryFixture f(false,true);
    (void)f.PrepareFundedHistoricalWallet();
    f.DowngradeToPreIdentityTransparentWallet();
    (void)f.MineToWallet();
    ASSERT_EQ(f.f.db.deleteBlock(f.f.token,f.f.blocks[50].GetHash()),Status::Ok);
    ASSERT_EQ(storage::ReadArchivalBlock(f.f.db,f.files.get(),f.f.blocks[50].GetHash()).status(),
        Status::NotFound);
    EXPECT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(),Recovery::Deferred);
    auto use=WalletService::AcquireWalletUse(f.wallet);
    auto lease=use->Wallet().AcquireDatabaseLease();
    EXPECT_EQ(lease->LegacyTransparentState(),WalletManager::LegacyTransparentState::Pending);
    sqlite3_stmt* scan=nullptr;
    ASSERT_EQ(sqlite3_prepare_v2(lease->Database(),
        "SELECT last_scanned_height FROM sync_meta WHERE id=1",-1,&scan,nullptr),SQLITE_OK);
    ASSERT_EQ(sqlite3_step(scan),SQLITE_ROW);
    EXPECT_EQ(sqlite3_column_int(scan,0),101);
    sqlite3_finalize(scan);
    lease.reset();
    EXPECT_THROW((void)use->Wallet().getBalance(),std::runtime_error);
}
TEST(WalletCanonicalRecovery, EarnedPrefixAndEncryptedUnlockRestoreActualNote) {
    CanonicalRecoveryFixture f(true);
    EXPECT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(),Recovery::NotRequired);
    f.MineAndAdopt();
    EXPECT_EQ(f.ReadAccount().account.Delivery().sequence,0u);
    EXPECT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(),Recovery::AppliedPrefix);
    auto recovered=f.ReadAccount();EXPECT_EQ(recovered.account.Scan().BalanceUna(),500000u);
    ASSERT_EQ(recovered.account.Scan().Notes().size(),1u);EXPECT_EQ(recovered.account.Delivery().sequence,1u);
    const auto first_session=f.Session();
    {auto use=WalletService::AcquireWalletUse(f.wallet);use->Wallet().open("canonical-recovery");EXPECT_TRUE(use->Wallet().isLocked());}
    // Match the existing wallet.open service flow: reopen clears the live
    // script map, then checked binding reload restores the recorded inventory.
    ASSERT_TRUE(f.wallet->EnsureRuntimeWalletBindings());
    EXPECT_NE(f.Session(),first_session);EXPECT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(),Recovery::Deferred);
    ExecutionContext context;context.daemon=&f.context;din::Json request=din::arr();request.append("canonical-fixture-pass");
    const auto unlocked=::rpc_context_wallet_unlock(context,request);
    EXPECT_TRUE(unlocked["success"].asBool());EXPECT_FALSE(unlocked.isMember("recovery_warning")) << unlocked["recovery_warning"].asString();
    EXPECT_EQ(f.ReadAccount().account.Scan().BalanceUna(),500000u);
    // A retained canonical disconnect remains authoritative even after the
    // selected tip moves below activation. It must not select legacy scanning.
    ASSERT_TRUE(ShieldedStateStartupTestAccess::DisconnectBoundary(*f.f.service,f.f.service->GetActiveTip()));
    EXPECT_EQ(f.f.service->GetActiveTip()->height,f.parent->height);
    EXPECT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(),Recovery::AppliedPrefix);
    const auto disconnected=f.ReadAccount();
    EXPECT_EQ(disconnected.account.Scan().BalanceUna(),0u);
    EXPECT_EQ(disconnected.account.Delivery().sequence,2u);
}
TEST(WalletCanonicalRecovery, ActualStartupResumesLaggingStoresWithoutHeightReset) {
    CanonicalRecoveryFixture f;f.MineAndAdopt();f.MineEmpty();const auto before=f.Session();
    EXPECT_EQ(f.ReadAccount().account.Delivery().sequence,0u);
    ASSERT_TRUE(f.wallet->Start());EXPECT_NE(f.Session(),before);
    const auto account=f.ReadAccount();EXPECT_EQ(account.account.Scan().BalanceUna(),500000u);
    EXPECT_EQ(account.account.Delivery().sequence,2u);
    auto use=WalletService::AcquireWalletUse(f.wallet);auto index=ChainstateService::AcquireWalletIndexUse(f.f.service);
    const auto session=use->Wallet().AcquireDatabaseLease()->Session();
    const auto ordinary=RuntimeOrdinaryDelivery::ReadForWallet(use->Wallet(),session);
    const auto indexed=RuntimeIndexDelivery::ReadForWallet(use->Wallet(),index->Index(),session);
    ASSERT_TRUE(ordinary);ASSERT_TRUE(indexed);EXPECT_EQ(ordinary->cursor.sequence,2u);EXPECT_EQ(indexed->cursor,ordinary->cursor);
}
TEST(WalletCanonicalRecovery, MissingBaselineNeverEnrollsOrUsesLegacySnapshotPath) {
    CanonicalRecoveryFixture f;f.EnrollAccount();const auto keys=f.Keys();const auto [body,bundle]=f.Shield(keys);(void)f.Mine(body);
    EXPECT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(),Recovery::Deferred);
    EXPECT_FALSE(f.wallet->RecoverActiveWalletFromSnapshotIfNeeded());
    auto use=WalletService::AcquireWalletUse(f.wallet);auto index=ChainstateService::AcquireWalletIndexUse(f.f.service);
    const auto session=use->Wallet().AcquireDatabaseLease()->Session();
    EXPECT_FALSE(RuntimeOrdinaryDelivery::ReadForWallet(use->Wallet(),session));
    EXPECT_FALSE(RuntimeIndexDelivery::ReadForWallet(use->Wallet(),index->Index(),session));
    EXPECT_EQ(f.ReadAccount().account.Delivery().sequence,0u);
}
TEST(WalletCanonicalRecovery, RequiredWriteFailureRetainsPrefixForRetry) {
    CanonicalRecoveryFixture f;f.MineAndAdopt();
    {auto use=WalletService::AcquireWalletUse(f.wallet);auto lease=use->Wallet().AcquireDatabaseLease();
     ASSERT_EQ(sqlite3_exec(lease->Database(),"CREATE TRIGGER refuse_recovery_account BEFORE UPDATE ON orchard_wallet_snapshots BEGIN SELECT RAISE(ABORT,'fixture required write refusal'); END",nullptr,nullptr,nullptr),SQLITE_OK);}
    EXPECT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(),Recovery::Deferred);
    EXPECT_EQ(f.ReadAccount().account.Delivery().sequence,0u);
    {auto use=WalletService::AcquireWalletUse(f.wallet);auto lease=use->Wallet().AcquireDatabaseLease();
     ASSERT_EQ(sqlite3_exec(lease->Database(),"DROP TRIGGER refuse_recovery_account",nullptr,nullptr,nullptr),SQLITE_OK);}
    EXPECT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(),Recovery::AppliedPrefix);
    EXPECT_EQ(f.ReadAccount().account.Scan().BalanceUna(),500000u);
}
#endif
} // namespace dinero
