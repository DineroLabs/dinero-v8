#include "rpc/orchard_account_rpc.h"
#include "daemon/daemon_context.h"
#include "daemon/services/chainstate_service.h"
#include "daemon/services/wallet_service.h"
#include <stdexcept>
#include <algorithm>
#include "util/hex.h"
#if DINERO_WALLET_RAW_ORCHARD
#include "wallet/orchard_account_delivery.h"
#include "wallet/runtime_account_replay.h"
#endif

namespace {
din::Json OrchardAccountRequest(const ExecutionContext& ctx,const din::Json& params,bool creating){
    din::Json result;
    try {
        if(!params.isObject()||params.size()!=1||!params.isMember("account"))
            throw std::runtime_error(creating?"Usage: wallet.orchard.createaccount {account: unsigned_integer}":"Usage: wallet.orchard.getnewaddress {account: unsigned_integer}");
        const auto& number=params["account"];
        if((number.type()!=::Json::uintValue&&number.type()!=::Json::intValue)||
           (number.type()==::Json::intValue&&number.asInt64()<0)||number.asUInt64()>=0x80000000ULL)
            throw std::runtime_error("Orchard account number must be an integer from 0 through 2147483647");
#if DINERO_WALLET_RAW_ORCHARD
        if(!ctx.daemon)throw std::runtime_error("Daemon services unavailable");
        auto source=std::dynamic_pointer_cast<dinero::ChainstateService>(ctx.daemon->chainstate);
        auto wallet=std::dynamic_pointer_cast<dinero::WalletService>(ctx.daemon->wallet);
        auto source_use=dinero::ChainstateService::AcquireWalletIndexUse(source);
        auto wallet_use=dinero::WalletService::AcquireWalletUse(wallet);
        uint64_t session;
        {
            auto lease=wallet_use->Wallet().AcquireDatabaseLease();
            if(!ctx.walletName.empty()&&ctx.walletName!=lease->WalletName())
                throw std::runtime_error("Selected wallet does not match request");
            session=lease->Session();
        }
        if(source->IsInSafeMode())throw std::runtime_error("Orchard wallet issuance unavailable in safe mode");
        // Source capture never runs under a wallet SQLite or key lease.
        // Missing/unset/CSN/backend source refuses; no empty account is made.
        const auto view=source->getRuntimeAccountReplayForWallet(wallet_use->Wallet(),session);
        if(!view.ok())throw std::runtime_error("Authenticated Orchard replay source unavailable");
        const auto& context=(*view)->Event(1).context;
        const dinero::wallet::OrchardAccountDelivery::Profile profile{
            context.domain,context.activation_height,static_cast<uint32_t>(number.asUInt64())};
        const auto issued=creating?
            dinero::wallet::OrchardAccountDelivery::CreateAccountForReplay(wallet_use->Wallet(),session,profile,**view):
            dinero::wallet::OrchardAccountDelivery::IssueCatalogReceiverForReplay(
                wallet_use->Wallet(),session,profile,**view,dinero::orchard::WalletScope::External);
        result["address"]=issued.address;result["account"]=Json::UInt64(profile.account);
        result["revision"]=Json::UInt64(issued.revision);
        if(creating)result["requires_sync"]=true;
#else
        (void)ctx;throw std::runtime_error("Orchard wallet backend unavailable");
#endif
    } catch(const std::exception& e) {result.clear();result["error"]=e.what();}
    return result;
}

} // namespace
din::Json rpc_context_wallet_orchard_getnewaddress(const ExecutionContext& ctx,const din::Json& params){return OrchardAccountRequest(ctx,params,false);}
din::Json rpc_context_wallet_orchard_createaccount(const ExecutionContext& ctx,const din::Json& params){return OrchardAccountRequest(ctx,params,true);}

din::Json rpc_context_wallet_orchard_listoperations(const ExecutionContext& ctx,const din::Json& params) {
    din::Json result;
    try {
        if (!params.isObject() || params.size()!=1 || !params.isMember("account"))
            throw std::runtime_error("Usage: wallet.orchard.listoperations {account: unsigned_integer}");
        const auto& number=params["account"];
        if ((number.type()!=::Json::uintValue && number.type()!=::Json::intValue) ||
            (number.type()==::Json::intValue && number.asInt64()<0) || number.asUInt64()>=0x80000000ULL)
            throw std::runtime_error("Orchard account number must be an integer from 0 through 2147483647");
#if DINERO_WALLET_RAW_ORCHARD
        if (!ctx.daemon) throw std::runtime_error("Daemon services unavailable");
        auto source=std::dynamic_pointer_cast<dinero::ChainstateService>(ctx.daemon->chainstate);
        auto wallet=std::dynamic_pointer_cast<dinero::WalletService>(ctx.daemon->wallet);
        auto source_use=dinero::ChainstateService::AcquireWalletIndexUse(source);
        auto wallet_use=dinero::WalletService::AcquireWalletUse(wallet);
        uint64_t session;
        {
            auto lease=wallet_use->Wallet().AcquireDatabaseLease();
            if (!ctx.walletName.empty() && ctx.walletName!=lease->WalletName())
                throw std::runtime_error("Selected wallet does not match request");
            session=lease->Session();
        }
        if (source->IsInSafeMode()) throw std::runtime_error("Orchard wallet source unavailable in safe mode");
        // Capture the immutable selected source before wallet/SQLite ownership.
        // Authenticate every declared current and retained owner before exposing
        // any operation. No executor lookup, schema creation, sync or queue write.
        const auto view=source->getRuntimeAccountReplayForWallet(wallet_use->Wallet(),session);
        if (!view.ok()) throw std::runtime_error("Authenticated Orchard replay source unavailable");
        const auto inventory=dinero::wallet::OrchardAccountDelivery::ReadCatalogForReplay(
            wallet_use->Wallet(),session,**view);
        const auto found=std::find_if(inventory.accounts.begin(),inventory.accounts.end(),
            [&](const auto& entry){return entry.number==number.asUInt64();});
        if (found==inventory.accounts.end()) throw std::runtime_error("Orchard account is not declared in this wallet");
        din::Json operations=din::arr();
        for (const auto& [id,entry]:found->state.account.Operations().Entries()) {
            din::Json operation;
            operation["operation_id"]=util::hex(std::vector<unsigned char>(id.begin(),id.end()));
            switch (entry.phase) {
                case dinero::wallet::OrchardOperationQueue::Phase::Reserved:
                    operation["durable_state"]="reserved";
                    break;
                case dinero::wallet::OrchardOperationQueue::Phase::Ready:
                    operation["durable_state"]="signed";
                    break;
                default: throw std::runtime_error("Unsupported Orchard operation state");
            }
            operations.append(std::move(operation));
        }
        const auto checkpoint=found->state.account.Delivery();
        result["account"]=Json::UInt64(found->number);
        result["account_revision"]=Json::UInt64(found->state.revision);
        result["account_sequence"]=Json::UInt64(checkpoint.sequence);
        result["account_digest"]=checkpoint.digest.GetHex();
        result["captured_source_sequence"]=Json::UInt64((*view)->Head().sequence);
        result["captured_source_digest"]=(*view)->Head().digest.GetHex();
        result["operations"]=std::move(operations);
#else
        (void)ctx;
        throw std::runtime_error("Orchard wallet backend unavailable");
#endif
    } catch (const std::exception& e) {
        result.clear();
        result["error"]=e.what();
    }
    return result;
}

void RegisterOrchardAccountRpc(){
    const RpcMethodMeta metadata{
        "wallet.orchard.getnewaddress","wallet",
        "Persist and return an external receiving address for an existing authenticated Orchard account.",
        {{"account","integer","Existing account number (0 through 2147483647).",true}},
        {"object","Address, account number and committed account revision."},
        "Requires an unlocked wallet, authenticated generated account catalog and complete current/reached account history. Unknown recovery inventory refuses. Does not create an account or report spend readiness."};
    g_rpcRegistry.registerHandler(metadata.name,rpc_context_wallet_orchard_getnewaddress,
        metadata,RegisterMode::Overwrite,"orchard-account-owner");
    const RpcMethodMeta operations{
        "wallet.orchard.listoperations","wallet",
        "List authenticated durable pending Orchard operations for an existing account.",
        {{"account","integer","Existing account number (0 through 2147483647).",true}},
        {"object","Account revision, captured progress and operation IDs with durable states."},
        "Requires an unlocked wallet and complete authenticated generated account catalog. Reserved and signed describe stored wallet states, not prover, mempool or confirmation status. Captured progress may lag the chain. Does not return signed bytes or change reservations."};
    g_rpcRegistry.registerHandler(operations.name,rpc_context_wallet_orchard_listoperations,
        operations,RegisterMode::Overwrite,"orchard-account-owner");
    const RpcMethodMeta creation{
        "wallet.orchard.createaccount","wallet",
        "Create a new catalog-owned Orchard account and return its first receiving address.",
        {{"account","integer","Previously unused account number (0 through 2147483647).",true}},
        {"object","Address, account number, committed account revision and synchronization requirement."},
        "Requires an unlocked generated wallet with authenticated complete account ownership and an available checked source. Recovery or legacy unknown inventory refuses. The account must synchronize before spending."};
    g_rpcRegistry.registerHandler(creation.name,rpc_context_wallet_orchard_createaccount,
        creation,RegisterMode::Overwrite,"orchard-account-owner");

}
