#include "wallet/wallet_transaction_signer.h"
#include "wallet/wallet_manager.h"
#include "wallet/wallet_key_provider.h"
#include "wallet/v7_p2mr_store.h"
#include "consensus/pq/p2mr_consensus.h"
#include "util/hex.h"
#include <openssl/crypto.h>
#include <set>
#include <stdexcept>
namespace dinero {
WalletSigningIdentity CaptureWalletSigningIdentity(WalletManager& wallet,const std::string& requested_name) {
    auto lease=wallet.AcquireDatabaseLease();
    if(!lease->Database() || lease->WalletName().empty() ||
       (!requested_name.empty() && requested_name!=lease->WalletName()))
        throw std::runtime_error("Selected wallet does not match signing request");
    return {lease->WalletName(),lease->Session()};
}
namespace {
SignResult SignWalletTransactionOwned(WalletManager& manager,const WalletSigningIdentity& identity,
                                     const UnsignedTransaction& input,const PendingPaymentIntent* payment) {
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
        if (payment && payment->request && lease->FindPaymentRequest(*pin, *payment))
            throw std::runtime_error("Payment request already retained; resolve its existing body");
        auto transaction=input;
        wallet::WalletKeyProvider::Config config;
        struct ClearConfig {wallet::WalletKeyProvider::Config& c;~ClearConfig(){OPENSSL_cleanse(c.master_key.data(),c.master_key.size());}} clear_config{config};
        bool needs_pq=false;
        for(auto& coin:transaction.selected_utxos) {
            if(consensus::pq::IsP2MRScript(coin.spk)){needs_pq=true;continue;}
            const auto script=util::hex(coin.spk);
            auto key=lease->ResolveSigningKey(script,*pin);
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
        if(payment)lease->StagePayment(*pin,transaction,signed_result.signed_tx.tx,*payment);
        return signed_result;
    } catch(const std::exception& e) {
        result.error=e.what();return result;
    }
}
} // namespace
SignResult SignWalletTransaction(WalletManager& manager,const WalletSigningIdentity& identity,
                                 const UnsignedTransaction& input) {
    return SignWalletTransactionOwned(manager,identity,input,nullptr);
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
    return SignWalletTransactionOwned(manager,identity,input,&payment);
}
}
