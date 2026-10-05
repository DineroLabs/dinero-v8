#include "wallet/wallet_transaction_signer.h"
#include "wallet/wallet_manager.h"
#include "wallet/wallet_key_provider.h"
#include "wallet/v7_p2mr_store.h"
#include "consensus/pq/p2mr_consensus.h"
#include "util/hex.h"
#include <openssl/crypto.h>
#include <set>
#include <stdexcept>
#include <sqlite3.h>
#include <type_traits>
#include "wallet/orchard_account_catalog.h"
namespace dinero {
WalletSigningIdentity CaptureWalletSigningIdentity(WalletManager& wallet,const std::string& requested_name) {
    auto lease=wallet.AcquireDatabaseLease();
    if(!lease->Database() || lease->WalletName().empty() ||
       (!requested_name.empty() && requested_name!=lease->WalletName()))
        throw std::runtime_error("Selected wallet does not match signing request");
    return {lease->WalletName(),lease->Session()};
}
SignResult WalletTransactionOwner::SignBeforeActivation(WalletManager& manager,
        const WalletSigningIdentity& identity,const UnsignedTransaction& input,const PendingPaymentIntent& payment){
    SignResult refused;
    try {
        auto lease=manager.AcquireDatabaseLease();auto* db=lease->Database();
        if(!db||!identity.session||identity.name.empty()||lease->Session()!=identity.session||
           lease->WalletName()!=identity.name||!sqlite3_get_autocommit(db))
            throw std::runtime_error("Pre-activation payment owner unavailable");
        const auto check=[](bool value){if(!value)throw std::runtime_error("Pre-activation payment inventory unavailable");};
        struct Statement {sqlite3_stmt* p=nullptr;~Statement(){sqlite3_finalize(p);}};
        check(sqlite3_exec(db,"PRAGMA synchronous=FULL",nullptr,nullptr,nullptr)==SQLITE_OK);
        {Statement q;check(sqlite3_prepare_v2(db,"PRAGMA synchronous",-1,&q.p,nullptr)==SQLITE_OK);
         check(sqlite3_step(q.p)==SQLITE_ROW&&sqlite3_column_type(q.p,0)==SQLITE_INTEGER&&sqlite3_column_int64(q.p,0)==2&&sqlite3_step(q.p)==SQLITE_DONE);}
        check(sqlite3_exec(db,"BEGIN IMMEDIATE",nullptr,nullptr,nullptr)==SQLITE_OK);
        struct Rollback {sqlite3* db;bool done=false;~Rollback(){if(!done&&!sqlite3_get_autocommit(db)&&sqlite3_exec(db,"ROLLBACK",nullptr,nullptr,nullptr)!=SQLITE_OK&&!sqlite3_get_autocommit(db))std::terminate();}} transaction{db};
        {
            auto pin=lease->CopyRecoverySeed(identity.session);
            const auto catalog=wallet::OrchardAccountCatalog::Read(db,pin->Bytes());
            if(catalog&&!catalog->accounts.empty())throw std::runtime_error("Recorded Orchard accounts require their canonical replay source");
            // Unknown old/recovery catalogs retain ordinary pre-activation
            // semantics only. No completeness certificate, enrollment or
            // new-pool permission is inferred from absent rows.
            for(const auto* name:{"orchard_wallet_snapshots","orchard_wallet_retained"}){
                Statement schema;check(sqlite3_prepare_v2(db,"SELECT 1 FROM sqlite_master WHERE type='table' AND name=?",-1,&schema.p,nullptr)==SQLITE_OK);
                check(sqlite3_bind_text(schema.p,1,name,-1,SQLITE_STATIC)==SQLITE_OK);const auto rc=sqlite3_step(schema.p);
                if(rc==SQLITE_DONE)continue;check(rc==SQLITE_ROW&&sqlite3_step(schema.p)==SQLITE_DONE);
                Statement row;const auto sql=std::string("SELECT 1 FROM ")+name;
                check(sqlite3_prepare_v2(db,sql.c_str(),-1,&row.p,nullptr)==SQLITE_OK);check(sqlite3_step(row.p)==SQLITE_DONE);
            }
        }
        auto result=Sign(manager,identity,input,&payment,true,true);
        if(!result.success)throw std::runtime_error(result.error);
        static_assert(std::is_nothrow_move_constructible_v<SignResult>);
        check(sqlite3_exec(db,"COMMIT",nullptr,nullptr,nullptr)==SQLITE_OK);transaction.done=true;return result;
    }catch(const std::exception& e){refused.error=e.what();return refused;}
}

SignResult WalletTransactionOwner::Sign(WalletManager& manager,const WalletSigningIdentity& identity,
                                     const UnsignedTransaction& input,const PendingPaymentIntent* payment,bool retain,bool caller_transaction) {
    SignResult result;
    try {
        if(input.tx.vin.empty() || input.tx.vin.size()!=input.selected_utxos.size())
            throw std::runtime_error("Signing input metadata is incomplete");
        std::set<std::string> outpoints;
        for(size_t i=0;i<input.tx.vin.size();++i) {
            const auto& coin=input.selected_utxos[i];const auto& prev=input.tx.vin[i].prevout;
            if(prev.txid!=TxId(coin.txid) || prev.vout!=coin.vout || coin.spk.empty() ||
               !outpoints.insert(coin.GetOutpointString()).second)
                throw std::runtime_error("Signing inputs do not match selected outpoints");
        }
        auto lease=manager.AcquireDatabaseLease();
        if(!lease->Database() || identity.name.empty() || identity.session==0 ||
           lease->WalletName()!=identity.name || lease->Session()!=identity.session)
            throw std::runtime_error("Selected wallet signing session changed");
        auto pin=lease->CopyRecoverySeed(identity.session);
        if (payment && payment->request && (caller_transaction ?
            lease->FindPaymentRequestInTransaction(*pin,*payment) : lease->FindPaymentRequest(*pin,*payment)))
            throw std::runtime_error("Payment request already retained; resolve its existing body");
        if(payment && payment->request && input.fee>payment->request->maximum_fee_una)
            throw std::runtime_error("Payment exceeds explicit request fee limit");
        auto transaction=input;
        wallet::WalletKeyProvider::Config config;
        struct ClearConfig {wallet::WalletKeyProvider::Config& c;~ClearConfig(){OPENSSL_cleanse(c.master_key.data(),c.master_key.size());}} clear_config{config};
        bool needs_pq=false;
        for(auto& coin:transaction.selected_utxos) {
            if(consensus::pq::IsP2MRScript(coin.spk)){needs_pq=true;continue;}
            const auto script=util::hex(coin.spk);
            auto key=caller_transaction ? lease->ResolveSigningKeyInTransaction(script,*pin) :
                                         lease->ResolveSigningKey(script,*pin);
            if(!key || key->secret.size()!=32)
                throw std::runtime_error("Selected input signing key is unavailable");
            if(key->policy==SigningKeyPolicy::TaprootCanonical && coin.path.empty()) {
                auto path=manager.getDerivationPath(script);
                if(!path)path=manager.getWatchScriptPath(coin.spk);
                if(path)coin.path=*path;
            }
            config.signing_keys_by_input.emplace(coin.GetOutpointString(),std::move(*key));
        }
        wallet::V7P2MRStore store;
        if(needs_pq) {
            struct Master {std::optional<std::array<uint8_t,32>> value;~Master(){if(value)OPENSSL_cleanse(value->data(),value->size());}} master{manager.GetV7PqMasterKey()};
            if(!master.value)throw std::runtime_error("Selected wallet PQ master is unavailable");
            const auto path=manager.GetV7P2MRStorePath();
            if(path.empty() || store.OpenExistingReadOnly(path)!=wallet::V7P2MRStore::OpenResult::Ok)
                throw std::runtime_error("Selected wallet PQ store is unavailable");
            config.master_key=*master.value;config.p2mr_store=&store;config.wallet_id=1;
        }
        wallet::WalletKeyProvider provider(std::move(config));
        auto signed_result=TransactionSigner::Sign(transaction,provider);
        if(!signed_result.success) {
            result.error=std::move(signed_result.error);
            return result;
        }
        if(payment && retain) {
            if(caller_transaction)lease->StagePaymentInTransaction(*pin,transaction,signed_result.signed_tx.tx,*payment);
            else lease->StagePayment(*pin,transaction,signed_result.signed_tx.tx,*payment);
        }
        return signed_result;
    } catch(const std::exception& e) {
        result.error=e.what();return result;
    }
}
SignResult SignWalletTransaction(WalletManager& manager,const WalletSigningIdentity& identity,
                                 const UnsignedTransaction& input) {
    return WalletTransactionOwner::Sign(manager,identity,input,nullptr,false);
}
SignResult SignWalletRequestPreview(WalletManager& manager,const WalletSigningIdentity& identity,
                                   const UnsignedTransaction& input,const PendingPaymentIntent& payment) {
    if(!payment.request) {SignResult result;result.error="Explicit payment request required";return result;}
    return WalletTransactionOwner::Sign(manager,identity,input,&payment,false);
}
std::optional<PendingPayment> FindRetainedWalletPayment(
    WalletManager& manager, const WalletSigningIdentity& identity, const PendingPaymentIntent& intent) {
    auto lease = manager.AcquireDatabaseLease();
    if (!lease->Database() || identity.name.empty() || identity.session == 0 ||
        lease->WalletName() != identity.name || lease->Session() != identity.session)
        throw std::runtime_error("Selected wallet payment request session changed");
    auto pin = lease->CopyRecoverySeed(identity.session);
    return lease->FindPaymentRequest(*pin, intent);
}
SignResult SignAndStageWalletPayment(WalletManager& manager,const WalletSigningIdentity& identity,
                                     const UnsignedTransaction& input,const PendingPaymentIntent& payment) {
    return WalletTransactionOwner::Sign(manager,identity,input,&payment,true);
}
}
