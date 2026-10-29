#pragma once
#include "crypto/pbkdf2.h"
#include "crypto/wallet_crypto.h"
#include "wallet/taproot_keys.h"
#include "dinero/core/common/AddressCodec.h"
#include <openssl/crypto.h>
#include <openssl/sha.h>
#ifdef DINERO_TEST_ORCHARD_ORIGIN
namespace dinero {
// Read-only, SQL-free observation of actual publication. No fixture can set
// historical authority or enroll a coin through this access.
struct UTXOIndexHistoricalTestAccess {
    static size_t Count(const UTXOIndex& index) {
        std::lock_guard<std::recursive_mutex> lock(index.db_mutex_);return index.historical_scripts_.size();
    }
};
namespace {
struct HistoricalDiscoveryFixture : ShieldReservationFixture {
    // This fixture writes the actual predecessor imported_keys format. It
    // deliberately creates no addresses, watch_scripts or derivation-path row.
    struct Row {
        sqlite3_stmt* value=nullptr;
        Row(sqlite3* db,const char* sql){
            if(sqlite3_prepare_v2(db,sql,-1,&value,nullptr)!=SQLITE_OK){
                const auto error=std::string(sqlite3_errmsg(db));sqlite3_finalize(value);value=nullptr;
                throw std::runtime_error("fixture SQL prepare: "+error+"; statement: "+sql);
            }
        }
        ~Row(){sqlite3_finalize(value);}
        void Text(int i,const std::string& s){Need(sqlite3_bind_text(value,i,s.data(),int(s.size()),SQLITE_TRANSIENT)==SQLITE_OK);}
        void Blob(int i,const std::vector<uint8_t>& s){Need(sqlite3_bind_blob(value,i,s.data(),int(s.size()),SQLITE_TRANSIENT)==SQLITE_OK);}
        void Done(){Need(sqlite3_step(value)==SQLITE_DONE);}
    };
    std::string imported_address;
    std::vector<uint8_t> imported_script;
    explicit HistoricalDiscoveryFixture(bool raw=true) {
        std::array<uint8_t,32> internal{},output{},tweak{};int parity=0;
        struct Secret {
            std::array<uint8_t,32> scalar{},key{};
            std::vector<uint8_t> plaintext;
            ~Secret(){OPENSSL_cleanse(scalar.data(),scalar.size());OPENSSL_cleanse(key.data(),key.size());
                if(!plaintext.empty())OPENSSL_cleanse(plaintext.data(),plaintext.size());}
        } secret;
        secret.scalar.back()=67;
        Need(TaprootKeys::DeriveXOnlyPubkey(secret.scalar,internal,parity));
        std::array<uint8_t,33> material{};std::copy(internal.begin(),internal.end(),material.begin());
        ::SHA256(material.data(),material.size(),tweak.data());
        std::unique_ptr<secp256k1_context,decltype(&secp256k1_context_destroy)> secp(
            secp256k1_context_create(SECP256K1_CONTEXT_VERIFY),secp256k1_context_destroy);
        Need(bool(secp));secp256k1_xonly_pubkey pub{},result{};secp256k1_pubkey tweaked{};
        Need(secp256k1_xonly_pubkey_parse(secp.get(),&pub,internal.data()));
        Need(secp256k1_xonly_pubkey_tweak_add(secp.get(),&tweaked,&pub,tweak.data()));
        Need(secp256k1_xonly_pubkey_from_pubkey(secp.get(),&result,nullptr,&tweaked));
        Need(secp256k1_xonly_pubkey_serialize(secp.get(),output.data(),&result));
        imported_address=AddressCodec::encodeP2TR(Network::MAIN,std::vector<uint8_t>(output.begin(),output.end()));
        imported_script={0x51,0x20};imported_script.insert(imported_script.end(),output.begin(),output.end());
        if(raw)secret.plaintext.assign(secret.scalar.begin(),secret.scalar.end());
        else {auto hex=util::hex(std::vector<uint8_t>(secret.scalar.begin(),secret.scalar.end()));
            secret.plaintext.assign(hex.begin(),hex.end());OPENSSL_cleanse(hex.data(),hex.size());}
        {
            auto use=WalletService::AcquireWalletUse(wallet);auto lease=use->Wallet().AcquireDatabaseLease();
            const auto salt_hex=use->Wallet().getSetting("wallet_salt");Need(!salt_hex.empty());
            std::vector<uint8_t> salt;Need(util::unhex(salt_hex,salt)&&salt.size()==32);
            const std::string password="canonical-fixture-pass";
            crypto::PBKDF2_HMAC_SHA512(reinterpret_cast<const uint8_t*>(password.data()),password.size(),
                salt.data(),salt.size(),210000,secret.key.data(),secret.key.size());
            // One deterministic nonce per newly randomized isolated fixture key.
            // Prefix zero also requires explicit-length binary TEXT handling.
            const std::vector<uint8_t> nonce(12,0);
            auto ciphertext=crypto::encryptAesGcm(secret.plaintext,secret.key,nonce);
            ciphertext.insert(ciphertext.begin(),nonce.begin(),nonce.end());
            InventoryTestTransaction transaction(lease->Database());
            Row row(lease->Database(),"INSERT INTO imported_keys(address,private_key_enc,label) VALUES(?,?,'original historical label')");
            row.Text(1,imported_address);row.Text(2,std::string(ciphertext.begin(),ciphertext.end()));row.Done();
            transaction.Commit();
        }
        // Authenticate the reconstructed predecessor tuple through the actual
        // production resolver before attempting discovery or chain effects.
        {auto use=WalletService::AcquireWalletUse(wallet);
         const auto key=use->Wallet().resolveSigningKeyForScriptPubKey(util::hex(imported_script));
         Need(key&&key->policy==SigningKeyPolicy::TaprootHistoricalImport&&key->script==imported_script);}
    }
    std::string OriginalOwner() {
        auto use=WalletService::AcquireWalletUse(wallet);auto lease=use->Wallet().AcquireDatabaseLease();
        Row row(lease->Database(),"SELECT address,private_key_enc,label FROM imported_keys WHERE address=?");
        row.Text(1,imported_address);Need(sqlite3_step(row.value)==SQLITE_ROW);std::string result;
        for(int col=0;col<3;++col){Need(sqlite3_column_type(row.value,col)==SQLITE_TEXT);
            const auto* bytes=static_cast<const char*>(sqlite3_column_blob(row.value,col));const auto n=sqlite3_column_bytes(row.value,col);
            Need(bytes&&n>0);result+=std::to_string(n)+":";result.append(bytes,n);}
        row.Done();return result;
    }
    void RequireNoInventedOrigin() {
        auto use=WalletService::AcquireWalletUse(wallet);auto lease=use->Wallet().AcquireDatabaseLease();
        for(const auto* table:{"addresses","address_derivation_paths","taproot_keys"}) {
            if(std::string_view(table)=="taproot_keys") {
                // The modern-import table is lazy. Its checked absence proves
                // no modern row was invented; do not create it to inspect it.
                Row exists(lease->Database(),"SELECT count(*) FROM sqlite_schema WHERE type='table' AND name='taproot_keys'");
                Need(sqlite3_step(exists.value)==SQLITE_ROW&&sqlite3_column_type(exists.value,0)==SQLITE_INTEGER);
                const auto count=sqlite3_column_int64(exists.value,0);Need(count==0||count==1);exists.Done();
                if(count==0)continue;
            }
            const auto sql=std::string("SELECT 1 FROM ")+table+" WHERE address=?";Row row(lease->Database(),sql.c_str());
            row.Text(1,imported_address);row.Done();
        }
        Row watched(lease->Database(),"SELECT 1 FROM watch_scripts WHERE script_pubkey=?");watched.Blob(1,imported_script);watched.Done();
        Need(!use->Wallet().getDerivationPath(util::hex(imported_script)));
        Need(!use->Wallet().getWatchScriptPath(imported_script));
    }
    void FundAndDiscover() {
        Need(wallet->EnsureRuntimeWalletBindings());
        Need(wallet->RecoverActiveWalletFromCanonicalSource()==Recovery::AppliedPrefix);
        auto transfer=Input();transfer.tx.vout.clear();
        transfer.tx.vout.emplace_back(AmountUna::Una(40000),imported_script);
        transfer.tx.vout.emplace_back(AmountUna::Una(59000),script);transfer.change_amount=59000;
        PendingPaymentIntent intent{imported_address,40000,"historical shield funding"};Transaction funded;
        {auto use=WalletService::AcquireWalletUse(wallet);auto index=ChainstateService::AcquireWalletIndexUse(f.service);
         const auto signed_payment=f.service->signAndStageWalletPayment(use->Wallet(),Selected(),transfer,intent);
         if(!signed_payment.success)throw std::runtime_error(signed_payment.error);funded=signed_payment.signed_tx.tx;}
        const auto block=Mine(MempoolTransaction(funded));Need(block&&block->Context()->height==103);
        Need(wallet->RecoverActiveWalletFromCanonicalSource()==Recovery::AppliedPrefix);
        funding.txid=funded.GetTxid().AsUint256();funding.vout=0;funding.value=AmountUna::Una(40000);
        funding.height=103;funding.spk=imported_script;funding.path.clear();
        const auto included=f.service->getCanonicalOutputInclusion(funding.txid,0,103);
        Need(included.ok()&&included->MatchesTransparent(40000,imported_script));
        // Neither wallet nor index coins may be manually enrolled here.
        {auto use=WalletService::AcquireWalletUse(wallet);auto lease=use->Wallet().AcquireDatabaseLease();
         Row row(lease->Database(),"SELECT amount,height,is_spent,script_pubkey FROM utxos WHERE txid=? AND vout=0");
         row.Text(1,funding.GetTxIdHex());Need(sqlite3_step(row.value)==SQLITE_ROW);
         for(int col=0;col<3;++col)Need(sqlite3_column_type(row.value,col)==SQLITE_INTEGER);
         Need(sqlite3_column_int64(row.value,0)==40000&&sqlite3_column_int64(row.value,1)==103&&sqlite3_column_int64(row.value,2)==0);
         const auto hex=util::hex(imported_script);Need(sqlite3_column_type(row.value,3)==SQLITE_TEXT&&
             sqlite3_column_bytes(row.value,3)==int(hex.size())&&sqlite3_column_blob(row.value,3)&&
             std::memcmp(sqlite3_column_blob(row.value,3),hex.data(),hex.size())==0);row.Done();}
        {auto index=ChainstateService::AcquireWalletIndexUse(f.service);const auto coin=index->Index().GetUTXO(TxId(funding.txid),0);
         Need(coin&&coin->spk==imported_script&&coin->value.GetUna()==40000&&coin->height==103&&coin->path.empty());}
        RequireNoInventedOrigin();
    }
    auto Outputs(){return std::vector<orchard::TransparentOutput>{{10000,imported_script}};}
    auto QueueHistorical(wallet::OrchardProofJobs& jobs) {
        const auto revision=Account(3).revision;const auto inputs=Inputs();const auto payments=Payments();const auto outputs=Outputs();
        auto use=WalletService::AcquireWalletUse(wallet);auto index=ChainstateService::AcquireWalletIndexUse(f.service);
        return f.service->queueRuntimeWalletShield(use->Wallet(),Selected().session,3,revision,orchard::Hash{81},inputs,payments,outputs,10000,jobs);
    }
    auto FinishHistorical(wallet::OrchardProofJobs& jobs) {
        const auto inputs=Inputs();const auto payments=Payments();const auto outputs=Outputs();
        auto use=WalletService::AcquireWalletUse(wallet);auto index=ChainstateService::AcquireWalletIndexUse(f.service);
        return f.service->finalizeRuntimeWalletShield(use->Wallet(),Selected().session,3,orchard::Hash{81},inputs,payments,outputs,10000,jobs);
    }
};
}
TEST(WalletHistoricalDiscovery, OriginalMainImportDiscoversShieldsAndReopensWithoutPath) {
    for(const bool raw:{true,false}) {
        HistoricalDiscoveryFixture f(raw);const auto original=f.OriginalOwner();f.RequireNoInventedOrigin();f.FundAndDiscover();
        EXPECT_EQ(f.OriginalOwner(),original);std::vector<uint8_t> body;
        {auto use=WalletService::AcquireWalletUse(f.wallet);auto& jobs=use->OrchardProofs();
         const auto queued=f.QueueHistorical(jobs);ASSERT_TRUE(queued);ASSERT_TRUE(queued->enqueued);
         ASSERT_EQ(ServiceProofTerminal(jobs,orchard::Hash{81}),wallet::OrchardProofJobs::State::Succeeded);
         body=f.FinishHistorical(jobs);EXPECT_FALSE(jobs.Query(orchard::Hash{81}));}
        const auto envelope=orchard::TransactionEnvelope::DecodeExact(body);
        ASSERT_EQ(envelope.Inputs().size(),1u);ASSERT_EQ(envelope.Inputs()[0].witness.size(),1u);
        EXPECT_EQ(envelope.Inputs()[0].witness[0].size(),64u);f.RequireNoInventedOrigin();EXPECT_EQ(f.OriginalOwner(),original);
        f.Reopen();ASSERT_TRUE(f.wallet->EnsureRuntimeWalletBindings());wallet::OrchardProofJobs absent;
        EXPECT_EQ(f.FinishHistorical(absent),body);EXPECT_EQ(f.OriginalOwner(),original);
        const auto block=f.Mine(MempoolTransaction::FromOrchard(envelope));ASSERT_TRUE(block);EXPECT_EQ(block->Context()->height,104u);
        ASSERT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(),Recovery::AppliedPrefix);
        EXPECT_EQ(f.Account(17).account.Scan().BalanceUna(),20000u);EXPECT_EQ(f.f.ingress->mempool().size(),0u);
        f.RequireNoInventedOrigin();EXPECT_EQ(f.OriginalOwner(),original);
    }
}
TEST(WalletHistoricalDiscovery, CorruptPresentOwnerRefusesBeforeRecoveryEffects) {
    HistoricalDiscoveryFixture f;f.FundAndDiscover();
    {auto use=WalletService::AcquireWalletUse(f.wallet);auto lease=use->Wallet().AcquireDatabaseLease();
     HistoricalDiscoveryFixture::Row row(lease->Database(),"UPDATE imported_keys SET private_key_enc='truncated' WHERE address=?");
     row.Text(1,f.imported_address);row.Done();ASSERT_EQ(sqlite3_changes(lease->Database()),1);}
    const auto before=f.Snapshot();
    EXPECT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(),Recovery::Deferred);EXPECT_EQ(f.Snapshot(),before);
    wallet::OrchardProofJobs jobs;EXPECT_THROW(f.QueueHistorical(jobs),std::runtime_error);
    EXPECT_FALSE(jobs.Query(orchard::Hash{81}));EXPECT_EQ(f.Snapshot(),before);f.RequireNoInventedOrigin();
}

TEST(WalletHistoricalDiscovery, CapturedOriginalScriptsAuthenticateWithoutCompanionWrites) {
    for(const bool raw:{true,false}) {
        HistoricalDiscoveryFixture f(raw);const auto original=f.OriginalOwner();
        for(int attempt=0;attempt<2;++attempt) {
            {auto use=WalletService::AcquireWalletUse(f.wallet);auto lease=use->Wallet().AcquireDatabaseLease();
             InventoryTestTransaction transaction(lease->Database());const auto changes=sqlite3_total_changes64(lease->Database());
             auto pin=lease->CopyRecoverySeed(lease->Session());
             const auto scripts=lease->ReadHistoricalImportScriptsInTransaction(pin.get());
             ASSERT_EQ(scripts.size(),1u);EXPECT_EQ(scripts.at(f.imported_script),f.imported_address);
             EXPECT_EQ(sqlite3_total_changes64(lease->Database()),changes);
             EXPECT_EQ(sqlite3_get_autocommit(lease->Database()),0);transaction.Commit();}
            f.RequireNoInventedOrigin();EXPECT_EQ(f.OriginalOwner(),original);
            if(attempt==0)f.Reopen();
        }
    }
}
TEST(WalletHistoricalDiscovery, CapturedScriptsRefuseLateMalformedOwnerWithoutPrefix) {
    HistoricalDiscoveryFixture f;const auto original=f.OriginalOwner();
    {auto use=WalletService::AcquireWalletUse(f.wallet);auto lease=use->Wallet().AcquireDatabaseLease();
     InventoryTestTransaction transaction(lease->Database());
     HistoricalDiscoveryFixture::Row malformed(lease->Database(),
         "INSERT INTO imported_keys(address,private_key_enc,label) VALUES('zz-late-malformed','invalid','retained')");
     malformed.Done();const auto changes=sqlite3_total_changes64(lease->Database());
     EXPECT_THROW(lease->ReadHistoricalImportScriptsInTransaction(),std::runtime_error);
     EXPECT_EQ(sqlite3_get_autocommit(lease->Database()),0);
     EXPECT_EQ(sqlite3_total_changes64(lease->Database()),changes);
     // The fixture owns this rollback. Production capture must leave it open.
    }
    {auto use=WalletService::AcquireWalletUse(f.wallet);auto lease=use->Wallet().AcquireDatabaseLease();
     InventoryTestTransaction transaction(lease->Database());const auto scripts=lease->ReadHistoricalImportScriptsInTransaction();
     EXPECT_EQ(scripts.size(),1u);EXPECT_EQ(scripts.at(f.imported_script),f.imported_address);transaction.Commit();}
    EXPECT_EQ(f.OriginalOwner(),original);f.RequireNoInventedOrigin();
}
TEST(WalletHistoricalDiscovery, CapturedScriptsRequireTransactionAndPreserveDeniedReadOwner) {
    HistoricalDiscoveryFixture f;const auto original=f.OriginalOwner();
    {auto use=WalletService::AcquireWalletUse(f.wallet);auto lease=use->Wallet().AcquireDatabaseLease();
     EXPECT_THROW(lease->ReadHistoricalImportScriptsInTransaction(),std::runtime_error);
     InventoryTestTransaction transaction(lease->Database());const auto changes=sqlite3_total_changes64(lease->Database());
     struct Authorizer {
         sqlite3* db;
         explicit Authorizer(sqlite3* value):db(value) {
             if(sqlite3_set_authorizer(db,[](void*,int action,const char* table,const char*,const char*,const char*) {
                 return action==SQLITE_READ&&table&&std::strcmp(table,"imported_keys")==0?SQLITE_DENY:SQLITE_OK;
             },nullptr)!=SQLITE_OK)throw std::runtime_error("Fixture read authorizer unavailable");
         }
         ~Authorizer(){sqlite3_set_authorizer(db,nullptr,nullptr);}
     };
     {Authorizer denied(lease->Database());EXPECT_THROW(lease->ReadHistoricalImportScriptsInTransaction(),std::runtime_error);}
     EXPECT_EQ(sqlite3_get_autocommit(lease->Database()),0);
     EXPECT_EQ(sqlite3_total_changes64(lease->Database()),changes);
     const auto scripts=lease->ReadHistoricalImportScriptsInTransaction();
     ASSERT_EQ(scripts.size(),1u);EXPECT_EQ(scripts.at(f.imported_script),f.imported_address);transaction.Commit();}
    EXPECT_EQ(f.OriginalOwner(),original);f.RequireNoInventedOrigin();
}
TEST(WalletHistoricalDiscovery, CoverageCommitRefusalKeepsAuthorityUnpublishedThenRetries) {
    HistoricalDiscoveryFixture f;const auto original=f.OriginalOwner();
    const auto source=CaptureScriptCoverage(f);ASSERT_TRUE(source.ok());const auto before=f.Snapshot();
    auto use=WalletService::AcquireWalletUse(f.wallet);auto index=ChainstateService::AcquireWalletIndexUse(f.f.service);
    auto* db=use->Wallet().getCurrentDatabase();ASSERT_EQ(UTXOIndexHistoricalTestAccess::Count(index->Index()),0u);
    struct Observe {
        sqlite3* db;UTXOIndex& index;int commits=0;bool absent=true;int refusal=1;
        ~Observe(){sqlite3_commit_hook(db,nullptr,nullptr);}
    } observed{db,index->Index()};
    sqlite3_commit_hook(db,[](void* p){auto& state=*static_cast<Observe*>(p);++state.commits;
        state.absent &= UTXOIndexHistoricalTestAccess::Count(state.index)==0;return state.refusal;},&observed);
    EXPECT_NE(f.f.service->reconcileRuntimeWalletCoverage(use->Wallet(),index->Index(),**source),Status::Ok);
    sqlite3_commit_hook(db,nullptr,nullptr);
    EXPECT_EQ(observed.commits,1);EXPECT_TRUE(observed.absent);EXPECT_EQ(f.Snapshot(),before);
    EXPECT_EQ(UTXOIndexHistoricalTestAccess::Count(index->Index()),0u);
    EXPECT_THROW((void)RuntimeIndexDelivery::ReadForWallet(use->Wallet(),index->Index(),f.Selected().session),std::runtime_error);
    const auto retry=f.f.service->getRuntimeWalletCoverage(use->Wallet(),f.Selected().session,index->Index());ASSERT_TRUE(retry.ok());
    observed.commits=0;observed.refusal=0;
    sqlite3_commit_hook(db,[](void* p){auto& state=*static_cast<Observe*>(p);++state.commits;
        state.absent &= UTXOIndexHistoricalTestAccess::Count(state.index)==0;return state.refusal;},&observed);
    ASSERT_EQ(f.f.service->reconcileRuntimeWalletCoverage(use->Wallet(),index->Index(),**retry),Status::Ok);
    sqlite3_commit_hook(db,nullptr,nullptr);
    // Successful preparation and ordinary publication each commit; both
    // observations must precede publication of the historical recognition map.
    EXPECT_EQ(observed.commits,2);EXPECT_TRUE(observed.absent);EXPECT_EQ(UTXOIndexHistoricalTestAccess::Count(index->Index()),1u);
    const auto progress=RuntimeIndexDelivery::ReadForWallet(use->Wallet(),index->Index(),f.Selected().session);
    ASSERT_TRUE(progress);EXPECT_EQ(progress->cursor,(*retry)->Head());
    const auto completed=f.Snapshot(),completed_index=CoverageIndexSnapshot(f);
    EXPECT_EQ(f.f.service->reconcileRuntimeWalletCoverage(use->Wallet(),index->Index(),**retry),Status::Ok);
    EXPECT_EQ(f.Snapshot(),completed);EXPECT_EQ(CoverageIndexSnapshot(f),completed_index);
    EXPECT_EQ(UTXOIndexHistoricalTestAccess::Count(index->Index()),1u);
    f.RequireNoInventedOrigin();EXPECT_EQ(f.OriginalOwner(),original);
}
TEST(WalletHistoricalDiscovery, WalletBoundIndexReadsReauthenticateAndPreserveBorrowedOwner) {
    HistoricalDiscoveryFixture f;f.FundAndDiscover();const auto original=f.OriginalOwner();
    const auto view=f.View();const auto event_handle=view->Event(view->Head().sequence);const auto& event=*event_handle;
    const auto before=f.Snapshot(),indexed=CoverageIndexSnapshot(f);
    auto use=WalletService::AcquireWalletUse(f.wallet);auto index=ChainstateService::AcquireWalletIndexUse(f.f.service);
    auto* db=use->Wallet().getCurrentDatabase();const auto session=f.Selected().session;
    struct Reset {sqlite3* db;~Reset(){sqlite3_set_authorizer(db,nullptr,nullptr);}} reset{db};
    ASSERT_EQ(sqlite3_set_authorizer(db,[](void*,int action,const char* table,const char*,const char*,const char*) {
        return action==SQLITE_READ&&table&&std::strcmp(table,"imported_keys")==0?SQLITE_DENY:SQLITE_OK;
    },nullptr),SQLITE_OK);
    EXPECT_THROW((void)RuntimeIndexDelivery::ReadForWallet(use->Wallet(),index->Index(),session),std::runtime_error);
    EXPECT_THROW((void)RuntimeIndexDelivery::ApplyForWallet(use->Wallet(),index->Index(),session,event),std::runtime_error);
    sqlite3_set_authorizer(db,nullptr,nullptr);
    EXPECT_TRUE(sqlite3_get_autocommit(db));EXPECT_EQ(f.Snapshot(),before);EXPECT_EQ(CoverageIndexSnapshot(f),indexed);
    ASSERT_EQ(sqlite3_exec(db,"BEGIN IMMEDIATE",nullptr,nullptr,nullptr),SQLITE_OK);
    EXPECT_THROW((void)RuntimeIndexDelivery::ReadForWallet(use->Wallet(),index->Index(),session),std::runtime_error);
    EXPECT_THROW((void)RuntimeIndexDelivery::ApplyForWallet(use->Wallet(),index->Index(),session,event),std::runtime_error);
    EXPECT_FALSE(sqlite3_get_autocommit(db));ASSERT_EQ(sqlite3_exec(db,"ROLLBACK",nullptr,nullptr,nullptr),SQLITE_OK);
    EXPECT_TRUE(RuntimeIndexDelivery::ReadForWallet(use->Wallet(),index->Index(),session));
    const auto repeat=RuntimeIndexDelivery::ApplyForWallet(use->Wallet(),index->Index(),session,event);
    EXPECT_EQ(repeat.cursor,view->Head());EXPECT_EQ(f.Snapshot(),before);EXPECT_EQ(CoverageIndexSnapshot(f),indexed);
    const auto coin=index->Index().GetUTXO(TxId(f.funding.txid),0);ASSERT_TRUE(coin);
    EXPECT_EQ(coin->owner_kind,WalletOutputOwner::HistoricalImport);EXPECT_EQ(coin->owner_reference,f.imported_address);
    EXPECT_TRUE(coin->path.empty());EXPECT_THROW((void)index->Index().IsOurScript(f.imported_script),std::runtime_error);
    f.RequireNoInventedOrigin();EXPECT_EQ(f.OriginalOwner(),original);
}
TEST(WalletHistoricalDiscovery, CapturedOwnerChangeRefusesBeforeIndexPublication) {
    HistoricalDiscoveryFixture f;const auto source=CaptureScriptCoverage(f);ASSERT_TRUE(source.ok());
    {auto use=WalletService::AcquireWalletUse(f.wallet);auto lease=use->Wallet().AcquireDatabaseLease();
     HistoricalDiscoveryFixture::Row row(lease->Database(),"UPDATE imported_keys SET private_key_enc='truncated' WHERE address=?");
     row.Text(1,f.imported_address);row.Done();ASSERT_EQ(sqlite3_changes(lease->Database()),1);}
    const auto before=f.Snapshot(),indexed=CoverageIndexSnapshot(f);
    auto use=WalletService::AcquireWalletUse(f.wallet);auto index=ChainstateService::AcquireWalletIndexUse(f.f.service);
    EXPECT_NE(f.f.service->reconcileRuntimeWalletCoverage(use->Wallet(),index->Index(),**source),Status::Ok);
    EXPECT_EQ(f.Snapshot(),before);EXPECT_EQ(CoverageIndexSnapshot(f),indexed);
    EXPECT_EQ(UTXOIndexHistoricalTestAccess::Count(index->Index()),0u);f.RequireNoInventedOrigin();
}
TEST(WalletHistoricalDiscovery, SignedShieldDisconnectReopenAndReconnectPreserveHistoricalOwner) {
    HistoricalDiscoveryFixture f;f.FundAndDiscover();const auto original=f.OriginalOwner();std::vector<uint8_t> body;
    {auto use=WalletService::AcquireWalletUse(f.wallet);auto& jobs=use->OrchardProofs();
     const auto queued=f.QueueHistorical(jobs);ASSERT_TRUE(queued&&queued->enqueued);
     ASSERT_EQ(ServiceProofTerminal(jobs,orchard::Hash{81}),wallet::OrchardProofJobs::State::Succeeded);
     body=f.FinishHistorical(jobs);EXPECT_FALSE(jobs.Query(orchard::Hash{81}));}
    const auto envelope=orchard::TransactionEnvelope::DecodeExact(body);const auto wire_id=envelope.Txid();uint256 hash;
    std::copy(wire_id.begin(),wire_id.end(),hash.begin());const auto txid=TxId(hash);
    const auto history=[&] {
        auto use=WalletService::AcquireWalletUse(f.wallet);auto lease=use->Wallet().AcquireDatabaseLease();
        HistoricalDiscoveryFixture::Row row(lease->Database(),
            "SELECT address,amount,category,label,time,height,confirmations FROM transactions WHERE txid=?");
        row.Text(1,hash.GetHex());HistoricalDiscoveryFixture::Need(sqlite3_step(row.value)==SQLITE_ROW);
        std::vector<std::string> result;
        for(int c=0;c<7;++c){const auto* bytes=static_cast<const char*>(sqlite3_column_blob(row.value,c));const int n=sqlite3_column_bytes(row.value,c);
            HistoricalDiscoveryFixture::Need(n>=0&&(bytes||!n));result.emplace_back(n?std::string(bytes,n):std::string());}
        row.Done();return result;
    };
    const auto retained=history();ASSERT_EQ(retained.size(),7u);EXPECT_EQ(retained[2],"shield");
    const auto first=f.Mine(MempoolTransaction::FromOrchard(envelope));ASSERT_TRUE(first);EXPECT_EQ(first->Context()->height,104u);
    ASSERT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(),Recovery::AppliedPrefix);
    {auto index=ChainstateService::AcquireWalletIndexUse(f.f.service);
     const auto input=index->Index().GetUTXO(TxId(f.funding.txid),0),change=index->Index().GetUTXO(txid,0);
     ASSERT_TRUE(input&&change);EXPECT_EQ(input->spend_height,104);EXPECT_EQ(change->value.GetUna(),10000u);
     for(const auto* coin:{&*input,&*change}){EXPECT_EQ(coin->owner_kind,WalletOutputOwner::HistoricalImport);
         EXPECT_EQ(coin->owner_reference,f.imported_address);EXPECT_TRUE(coin->path.empty());}}
    EXPECT_EQ(f.Account(17).account.Scan().BalanceUna(),20000u);
    // Completion archival is a distinct authenticated transition, not a
    // side effect of replay. Keep the historical owner throughout that phase.
    f.ArchiveConfirmedShield();
    const auto archived=f.Stored(f.Payments());ASSERT_TRUE(archived&&archived->archived&&archived->durable);
    EXPECT_EQ(archived->durable->transaction,body);
    const auto confirmed=history();for(size_t i=0;i<5;++i)EXPECT_EQ(confirmed[i],retained[i]);EXPECT_EQ(confirmed[5],"104");
    f.Disconnect();ASSERT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(),Recovery::AppliedPrefix);
    {auto index=ChainstateService::AcquireWalletIndexUse(f.f.service);const auto input=index->Index().GetUTXO(TxId(f.funding.txid),0);
     ASSERT_TRUE(input);EXPECT_FALSE(input->spend_height);EXPECT_EQ(input->owner_reference,f.imported_address);
     EXPECT_EQ(input->owner_kind,WalletOutputOwner::HistoricalImport);EXPECT_TRUE(input->path.empty());
     EXPECT_FALSE(index->Index().GetUTXO(txid,0));}
    EXPECT_EQ(f.Account(17).account.Scan().BalanceUna(),0u);
    const auto undone=history();for(size_t i=0;i<5;++i)EXPECT_EQ(undone[i],retained[i]);EXPECT_EQ(undone[5],"0");EXPECT_EQ(undone[6],"0");
    const auto restored=f.Stored(f.Payments());ASSERT_TRUE(restored&&restored->durable);EXPECT_EQ(restored->durable->transaction,body);
    f.Reopen();ASSERT_TRUE(f.wallet->EnsureRuntimeWalletBindings());
    ASSERT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(),Recovery::AppliedPrefix);
    const auto reopened=f.Stored(f.Payments());ASSERT_TRUE(reopened&&reopened->durable);EXPECT_EQ(reopened->durable->transaction,body);
    EXPECT_EQ(history(),undone);f.RequireNoInventedOrigin();EXPECT_EQ(f.OriginalOwner(),original);
    // Direct disconnect retains the original eligible block. Reinclude that
    // exact body, rather than create an equal-work competitor to it.
    const auto again=f.Submit(first->Orchard().WireBytes());ASSERT_TRUE(again.accepted()&&again.connected)<<again.reason;
    ASSERT_EQ(f.f.service->GetActiveTip()->hash,first->Orchard().Header().GetHash());
    EXPECT_EQ(f.f.service->GetActiveTip()->height,104);
    ASSERT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(),Recovery::AppliedPrefix);
    {auto index=ChainstateService::AcquireWalletIndexUse(f.f.service);
     const auto input=index->Index().GetUTXO(TxId(f.funding.txid),0),change=index->Index().GetUTXO(txid,0);
     ASSERT_TRUE(input&&change);EXPECT_EQ(input->spend_height,104);EXPECT_EQ(change->value.GetUna(),10000u);
     EXPECT_EQ(change->owner_kind,WalletOutputOwner::HistoricalImport);EXPECT_EQ(change->owner_reference,f.imported_address);EXPECT_TRUE(change->path.empty());}
    EXPECT_EQ(f.Account(17).account.Scan().BalanceUna(),20000u);
    const auto reconfirmed=history();for(size_t i=0;i<5;++i)EXPECT_EQ(reconfirmed[i],retained[i]);EXPECT_EQ(reconfirmed[5],"104");
    f.RequireNoInventedOrigin();EXPECT_EQ(f.OriginalOwner(),original);
}
} // namespace dinero
#endif
