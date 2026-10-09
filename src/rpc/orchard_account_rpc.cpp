#include "rpc/orchard_account_rpc.h"
#include "consensus/orchard_profile.h"
#include "daemon/daemon_context.h"
#include "daemon/services/chainstate_service.h"
#include "daemon/services/wallet_service.h"
#include <stdexcept>
#include <optional>
#include <algorithm>
#include <array>
#include <set>
#include <iterator>
#include <string_view>
#include <vector>
#include <utility>
#include "util/hex.h"
#if DINERO_WALLET_RAW_ORCHARD
#include "wallet/orchard_account_delivery.h"
#include "rpc/orchard_issuance_retry.h"
#include "daemon/services/mempool_service.h"
#include "consensus/orchard_authorization.h"
#include "address/addr_codec.h"
#include "wallet/address.h"
#include "wallet/p2mr_address.h"
#include "wallet/runtime_account_replay.h"
#endif

namespace {
constexpr const char* OrchardBindingContextKey="orchard.request_wallet_binding";
class OrchardBindingError final:public std::runtime_error {
public:
    const std::string code;
    OrchardBindingError(const char* c,const char* message):std::runtime_error(message),code(c){}
};
class OrchardHistoryIncomplete final:public std::runtime_error {
public:
    OrchardHistoryIncomplete():std::runtime_error("Orchard receipt history requires complete replay"){}
};
void OrchardRpcFailure(din::Json& result,const std::exception& error) {
    result.clear();result["error"]=error.what();
    if(dynamic_cast<const OrchardHistoryIncomplete*>(&error))result["error_code"]="history_incomplete";
    if(const auto* binding=dynamic_cast<const OrchardBindingError*>(&error))result["error_code"]=binding->code;
#if DINERO_WALLET_RAW_ORCHARD
    if(const auto* request=dynamic_cast<const dinero::wallet::OrchardRequestError*>(&error)) {
        using Code=dinero::wallet::OrchardRequestError::Code;
        switch(request->Reason()) {
            case Code::StaleAccountRevision:result["error_code"]="stale_account_revision";break;
            case Code::RequestIdConflict:result["error_code"]="request_id_conflict";break;
            case Code::RequestNotCurrent:result["error_code"]="request_not_current";break;
        }
    }
    if(const auto* proof=dynamic_cast<const dinero::wallet::OrchardProofUnavailable*>(&error)) {
        result["error_code"]="proof_not_ready";
        const auto state=proof->ObservedState();
        const char* value="missing";
        if(state)switch(*state) {
            case dinero::wallet::OrchardProofJobs::State::Queued:value="queued";break;
            case dinero::wallet::OrchardProofJobs::State::Running:value="running";break;
            case dinero::wallet::OrchardProofJobs::State::CancelRequested:value="cancel_requested";break;
            case dinero::wallet::OrchardProofJobs::State::Failed:value="failed";break;
            case dinero::wallet::OrchardProofJobs::State::Cancelled:value="cancelled";break;
            case dinero::wallet::OrchardProofJobs::State::Succeeded:value="result_unavailable";break;
            default:value="unavailable";break;
        }
        result["proof_state"]=value;
        result["reservation_retained"]=true;
    }
#endif
}
bool OrchardBindingShape(const din::Json& value) {
    if(!value.isString())return false;
    const auto text=value.asString();
    return text.size()==64 && std::all_of(text.begin(),text.end(),[](char c){
        return (c>='0'&&c<='9')||(c>='a'&&c<='f');
    });
}
#if DINERO_WALLET_RAW_ORCHARD
std::string OrchardBindingValue(dinero::WalletManager::DatabaseLease& lease) {
    const auto bytes=lease.RpcBinding();
    return util::hex(std::vector<unsigned char>(bytes.begin(),bytes.end()));
}
void CheckOrchardRequestWallet(const ExecutionContext& ctx,dinero::WalletManager::DatabaseLease& lease) {
    if(!ctx.walletName.empty() && ctx.walletName!=lease.WalletName())
        throw std::runtime_error("Selected wallet does not match request");
    // Public registry entry points always set this binding. Existing direct
    // C++ component calls retain their explicit service/session ownership.
    const auto expected=ctx.metadata.find(OrchardBindingContextKey);
    if(expected!=ctx.metadata.end() && expected->second!=OrchardBindingValue(lease))
        throw OrchardBindingError("wallet_binding_mismatch","Wallet selection changed; refresh the wallet binding");
}
#endif
void RegisterBoundOrchard(const RpcMethodMeta& description,RpcHandler handler) {
    auto metadata=description;
    metadata.params.insert(metadata.params.begin(),{"wallet_binding","string",
        "Opaque binding from wallet.orchard.getwalletbinding for the intended current wallet selection.",true});
    metadata.help+=" Public calls require wallet_binding. A stale binding refuses before wallet effects; refresh it explicitly after reopen or wallet switch.";
    g_rpcRegistry.registerHandler(metadata.name,
        [handler=std::move(handler)](const ExecutionContext& ctx,const din::Json& params) {
            din::Json result;
            try {
                if(!params.isObject() || !params.isMember("wallet_binding"))
                    throw OrchardBindingError("wallet_binding_required","Orchard request requires wallet_binding");
                if(!OrchardBindingShape(params["wallet_binding"]))
                    throw OrchardBindingError("wallet_binding_invalid","wallet_binding must be a 32-byte lowercase hexadecimal token");
                auto bound=ctx;bound.metadata[OrchardBindingContextKey]=params["wallet_binding"].asString();
                auto payload=params;payload.removeMember("wallet_binding");
                return handler(bound,payload);
            }catch(const std::exception& error){OrchardRpcFailure(result,error);return result;}
        },metadata,RegisterMode::Overwrite,"orchard-account-owner");
}
}

din::Json rpc_context_wallet_orchard_getwalletbinding(const ExecutionContext& ctx,const din::Json& params) {
    din::Json result;
    try {
        if(!params.isObject() || params.size()!=1 || !params.isMember("wallet_name") ||
            !params["wallet_name"].isString() || params["wallet_name"].asString().empty())
            throw OrchardBindingError("wallet_binding_invalid","Usage: wallet.orchard.getwalletbinding {wallet_name: intended_wallet_name}");
#if DINERO_WALLET_RAW_ORCHARD
        if(!ctx.daemon)throw std::runtime_error("Daemon services unavailable");
        auto wallet=std::dynamic_pointer_cast<dinero::WalletService>(ctx.daemon->wallet);
        auto use=dinero::WalletService::AcquireWalletUse(wallet);
        auto lease=use->Wallet().AcquireDatabaseLease();
        if(params["wallet_name"].asString()!=lease->WalletName() ||
            (!ctx.walletName.empty() && ctx.walletName!=lease->WalletName()))
            throw OrchardBindingError("wallet_binding_mismatch","Selected wallet does not match requested wallet name");
        result["wallet_binding"]=OrchardBindingValue(*lease);result["wallet_name"]=lease->WalletName();
#else
        (void)ctx;throw std::runtime_error("Orchard wallet backend unavailable");
#endif
    }catch(const std::exception& error){OrchardRpcFailure(result,error);}
    return result;
}

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
            CheckOrchardRequestWallet(ctx,*lease);
            session=lease->Session();
        }
        const auto account=static_cast<uint32_t>(number.asUInt64());
        const auto issued=dinero::rpc::detail::RetryOrchardIssuance([&] {
            if(source->IsInSafeMode())throw std::runtime_error("Orchard wallet issuance unavailable in safe mode");
            // Recapture outside wallet ownership on every typed catalog retry.
            // The original session is retained; a wallet switch must refuse.
            const auto view=source->getRuntimeAccountReplayForWallet(wallet_use->Wallet(),session);
            if(!view.ok())throw std::runtime_error("Authenticated Orchard replay source unavailable");
            const auto origin_event=(*view)->Event(1);const auto& context=origin_event->context;
            const dinero::wallet::OrchardAccountDelivery::Profile profile{
                context.domain,context.activation_height,account};
            return creating?
                dinero::wallet::OrchardAccountDelivery::CreateAccountForReplay(wallet_use->Wallet(),session,profile,**view):
                dinero::wallet::OrchardAccountDelivery::IssueCatalogReceiverForReplay(
                    wallet_use->Wallet(),session,profile,**view,dinero::orchard::WalletScope::External);
        });
        result["address"]=issued.address;result["account"]=Json::UInt64(account);
        result["revision"]=Json::UInt64(issued.revision);
        if(creating)result["requires_sync"]=true;
#else
        (void)ctx;throw std::runtime_error("Orchard wallet backend unavailable");
#endif
    } catch(const std::exception& e) {OrchardRpcFailure(result,e);}
    return result;
}

} // namespace
din::Json rpc_context_wallet_orchard_getnewaddress(const ExecutionContext& ctx,const din::Json& params){return OrchardAccountRequest(ctx,params,false);}
din::Json rpc_context_wallet_orchard_createaccount(const ExecutionContext& ctx,const din::Json& params){return OrchardAccountRequest(ctx,params,true);}

namespace {
enum class OrchardAccountReadMode { Operations, Balance, Accounts, Received };
din::Json OrchardAccountRead(const ExecutionContext& ctx,const din::Json& params,OrchardAccountReadMode mode) {
    din::Json result;
    try {
        const bool balance=mode==OrchardAccountReadMode::Balance;
        const bool listing=mode==OrchardAccountReadMode::Accounts;
        const bool received=mode==OrchardAccountReadMode::Received;
        uint64_t offset=0,limit=100;
        std::optional<uint64_t> expected_revision;
        std::optional<uint32_t> requested;
        if(listing) {
            if(!params.isObject() || !params.empty())
                throw std::runtime_error("Usage: wallet.orchard.listaccounts {}");
        } else {
            if(received) {
                if(!params.isObject())
                    throw std::runtime_error("Usage: wallet.orchard.listreceived {account, offset?, limit?, expected_revision?}");
                const auto optional_count=params.isMember("offset")+params.isMember("limit")+params.isMember("expected_revision");
                if(!params.isMember("account") || params.size()!=1+optional_count)
                    throw std::runtime_error("Usage: wallet.orchard.listreceived {account, offset?, limit?, expected_revision?}");
                const auto integer=[](const din::Json& value,uint64_t maximum,bool nonzero) {
                    if((value.type()!=::Json::intValue && value.type()!=::Json::uintValue) ||
                       (value.type()==::Json::intValue && value.asInt64()<0) ||
                       value.asUInt64()>maximum || (nonzero && value.asUInt64()==0))
                        throw std::runtime_error("Invalid Orchard history page parameter");
                    return value.asUInt64();
                };
                if(params.isMember("offset"))offset=integer(params["offset"],65536,false);
                if(params.isMember("limit"))limit=integer(params["limit"],1000,true);
                if(params.isMember("expected_revision"))expected_revision=integer(params["expected_revision"],UINT64_MAX,true);
                if(offset && !expected_revision)
                    throw std::runtime_error("Further Orchard history pages require expected_revision");
            } else if (!params.isObject() || params.size()!=1 || !params.isMember("account"))
                throw std::runtime_error(balance?"Usage: wallet.orchard.getbalance {account: unsigned_integer}":"Usage: wallet.orchard.listoperations {account: unsigned_integer}");
            const auto& number=params["account"];
            if ((number.type()!=::Json::uintValue && number.type()!=::Json::intValue) ||
                (number.type()==::Json::intValue && number.asInt64()<0) || number.asUInt64()>=0x80000000ULL)
                throw std::runtime_error("Orchard account number must be an integer from 0 through 2147483647");
            requested=static_cast<uint32_t>(number.asUInt64());
        }
#if DINERO_WALLET_RAW_ORCHARD
        if (!ctx.daemon) throw std::runtime_error("Daemon services unavailable");
        auto source=std::dynamic_pointer_cast<dinero::ChainstateService>(ctx.daemon->chainstate);
        auto wallet=std::dynamic_pointer_cast<dinero::WalletService>(ctx.daemon->wallet);
        auto source_use=dinero::ChainstateService::AcquireWalletIndexUse(source);
        auto wallet_use=dinero::WalletService::AcquireWalletUse(wallet);
        uint64_t session;
        {
            auto lease=wallet_use->Wallet().AcquireDatabaseLease();
            CheckOrchardRequestWallet(ctx,*lease);
            session=lease->Session();
        }
        // A detached read can overlap ordinary provider delivery. Retry only
        // the typed comparison of two authenticated snapshots, at most four
        // attempts. Each attempt recaptures the immutable source before wallet
        // ownership and authenticates/restores/rechecks the entire catalog.
        // Session, identity, SQL, decryption and restoration failures propagate.
        using Observation=std::pair<std::shared_ptr<const dinero::RuntimeAccountReplay>,
            dinero::wallet::OrchardAccountDelivery::CatalogEnrolled>;
        const auto observation=[&]() -> Observation {
            for(unsigned attempt=0;;++attempt){
                if(source->IsInSafeMode())throw std::runtime_error("Orchard wallet source unavailable in safe mode");
                const auto captured=source->getRuntimeAccountReplayForWallet(wallet_use->Wallet(),session);
                if(!captured.ok())throw std::runtime_error("Authenticated Orchard replay source unavailable");
                try {
                    auto catalog=dinero::wallet::OrchardAccountDelivery::ReadCatalogForReplay(
                        wallet_use->Wallet(),session,**captured);
                    return {*captured,std::move(catalog)};
                }catch(const dinero::wallet::OrchardAccountDelivery::CatalogChanged&){
                    if(attempt==3)throw;
                }
            }
        }();
        const auto& view=observation.first;
        const auto& inventory=observation.second;
        if(listing) {
            din::Json accounts=din::arr();
            for(const auto& entry:inventory.accounts) {
                const auto checkpoint=entry.state.account.Delivery();
                din::Json account;account["account"]=Json::UInt64(entry.number);
                account["account_revision"]=Json::UInt64(entry.state.revision);
                account["account_sequence"]=Json::UInt64(checkpoint.sequence);
                account["account_digest"]=checkpoint.digest.GetHex();
                account["checkpoint_height"]=Json::UInt(entry.state.account.Scan().Checkpoint().height);
                account["checkpoint_hash"]=entry.state.account.Scan().Checkpoint().block_hash.GetHex();
                accounts.append(std::move(account));
            }
            result["captured_source_sequence"]=Json::UInt64(view->Head().sequence);
            result["captured_source_digest"]=view->Head().digest.GetHex();
            result["accounts"]=std::move(accounts);
            return result;
        }
        if(!requested)throw std::runtime_error("Missing Orchard account selector");
        const auto found=std::find_if(inventory.accounts.begin(),inventory.accounts.end(),
            [&](const auto& entry){return entry.number==*requested;});
        if (found==inventory.accounts.end()) throw std::runtime_error("Orchard account is not declared in this wallet");
        if(received) {
            const auto& account=found->state.account;
            if(expected_revision && *expected_revision!=found->state.revision)
                throw dinero::wallet::OrchardRequestError(dinero::wallet::OrchardRequestError::Code::StaleAccountRevision);
            if(!account.Scan().HasCompleteReceiptHistory())throw OrchardHistoryIncomplete();
            const auto& receipts=account.Scan().CompleteReceipts();
            if(offset>receipts.size())throw std::runtime_error("Orchard history offset exceeds the captured history");
            const auto end=offset+std::min<uint64_t>(limit,receipts.size()-offset);
            din::Json rows=din::arr();
            for(uint64_t i=offset;i<end;++i) {
                const auto& receipt=receipts[i];
                const auto note=account.Scan().DecryptReceipt(i);const auto& facts=note.Facts();
                din::Json row;dinero::uint256 txid;const auto id=receipt.origin->Orchard().Txid();
                std::copy(id.begin(),id.end(),txid.begin());
                row["txid"]=txid.GetHex();row["action_index"]=Json::UInt(receipt.action_index);
                row["scope"]=receipt.scope==dinero::orchard::WalletScope::External?"external":"internal";
                row["amount_una"]=Json::UInt64(facts.amount);
                row["recipient_hex"]=util::hex(std::vector<unsigned char>(std::begin(facts.recipient),std::end(facts.recipient)));
                row["memo_hex"]=util::hex(std::vector<unsigned char>(std::begin(facts.memo),std::end(facts.memo)));
                row["height"]=Json::UInt(receipt.created_height);row["block_hash"]=receipt.created_block.GetHex();
                rows.append(std::move(row));
            }
            const auto checkpoint=account.Delivery();
            result["account"]=Json::UInt64(found->number);result["account_revision"]=Json::UInt64(found->state.revision);
            result["account_sequence"]=Json::UInt64(checkpoint.sequence);result["account_digest"]=checkpoint.digest.GetHex();
            result["captured_source_sequence"]=Json::UInt64(view->Head().sequence);result["captured_source_digest"]=view->Head().digest.GetHex();
            result["checkpoint_height"]=Json::UInt(account.Scan().Checkpoint().height);result["checkpoint_hash"]=account.Scan().Checkpoint().block_hash.GetHex();
            result["account_caught_up_to_captured_source"]=(checkpoint.sequence==view->Head().sequence && checkpoint.digest==view->Head().digest);
            result["history_complete"]=true;result["total_count"]=Json::UInt64(receipts.size());
            result["offset"]=Json::UInt64(offset);result["limit"]=Json::UInt64(limit);result["next_offset"]=din::Json();
            if(end<receipts.size())result["next_offset"]=Json::UInt64(end);
            result["received"]=std::move(rows);return result;
        }
        if (balance) {
            // Both notes and reservations belong to the same restored account.
            // Match actual owned note nullifiers: queue nullifiers also include
            // padding, and completed/conflicted intents may remain reserved.
            const auto& account=found->state.account;
            std::set<dinero::orchard::Hash> reserved;
            for (const auto& [id,entry]:account.Operations().Entries())
                reserved.insert(entry.nullifiers.begin(),entry.nullifiers.end());
            uint64_t confirmed=0,reserved_confirmed=0;
            for (const auto& owned:account.Scan().Notes()) {
                if (!owned.note) throw std::runtime_error("Authenticated Orchard note unavailable");
                const auto& facts=owned.note->Facts();
                if (facts.amount>dinero::orchard::kMaxMoneyUna-confirmed)
                    throw std::runtime_error("Orchard balance exceeds money range");
                confirmed+=facts.amount;
                dinero::orchard::Hash nullifier;
                std::copy(std::begin(facts.nullifier),std::end(facts.nullifier),nullifier.begin());
                if (reserved.contains(nullifier)) reserved_confirmed+=facts.amount;
            }
            if (confirmed!=account.Scan().BalanceUna() || reserved_confirmed>confirmed)
                throw std::runtime_error("Authenticated Orchard balance differs from notes");
            const auto checkpoint=account.Delivery();
            result["account"]=Json::UInt64(found->number);
            result["account_revision"]=Json::UInt64(found->state.revision);
            result["account_sequence"]=Json::UInt64(checkpoint.sequence);
            result["account_digest"]=checkpoint.digest.GetHex();
            result["captured_source_sequence"]=Json::UInt64(view->Head().sequence);
            result["captured_source_digest"]=view->Head().digest.GetHex();
            result["checkpoint_height"]=Json::UInt(account.Scan().Checkpoint().height);
            result["checkpoint_hash"]=account.Scan().Checkpoint().block_hash.GetHex();
            result["account_caught_up_to_captured_source"]=(checkpoint.sequence==view->Head().sequence && checkpoint.digest==view->Head().digest);
            result["confirmed_una"]=Json::UInt64(confirmed);
            result["reserved_confirmed_una"]=Json::UInt64(reserved_confirmed);
            result["unreserved_confirmed_una"]=Json::UInt64(confirmed-reserved_confirmed);
            return result;
        }
        din::Json operations=din::arr();
        for (const auto& [id,entry]:found->state.account.Operations().Entries()) {
            din::Json operation;
            operation["operation_id"]=util::hex(std::vector<unsigned char>(id.begin(),id.end()));
            // These optional details were restored with the authenticated
            // operation. Never infer a completion method from an input count,
            // transaction shape or caller label. Old intents remain visible
            // without an advertised completion method.
            if (entry.shield_request && entry.spend_request)
                throw std::runtime_error("Ambiguous Orchard stored request kind");
            if (entry.shield_request || entry.spend_request) {
                if (!entry.request_commitment)
                    throw std::runtime_error("Stored Orchard request commitment unavailable");
                operation["completion_method"]=entry.shield_request
                    ? "wallet.orchard.finishshield" : "wallet.orchard.finishspend";
            }
            switch (entry.phase) {
                case dinero::wallet::OrchardOperationQueue::Phase::Reserved:
                    operation["durable_state"]="reserved";
                    break;
                case dinero::wallet::OrchardOperationQueue::Phase::Ready:
                    operation["durable_state"]="signed";
                    {
                        const auto id=dinero::orchard::TransactionEnvelope::DecodeExact(entry.transaction).Txid();
                        dinero::uint256 txid;std::copy(id.begin(),id.end(),txid.begin());
                        operation["txid"]=txid.GetHex();
                    }
                    break;
                default: throw std::runtime_error("Unsupported Orchard operation state");
            }
            // This observation belongs to the authenticated account checkpoint
            // reported below. A lagging checkpoint is not current chain status.
            operation["chain_observation"]=din::Json();
            const auto observed=found->state.account.Observations().find(id);
            if(observed!=found->state.account.Observations().end()) {
                const auto& value=observed->second;din::Json observation;
                switch(value.outcome) {
                    case dinero::wallet::OrchardAccountState::OperationOutcome::Confirmed:
                        observation["outcome"]="confirmed";break;
                    case dinero::wallet::OrchardAccountState::OperationOutcome::Conflicted:
                        observation["outcome"]="conflicted";break;
                    default:throw std::runtime_error("Unsupported Orchard operation observation");
                }
                dinero::uint256 transaction;std::copy(value.transaction_id.begin(),value.transaction_id.end(),transaction.begin());
                observation["height"]=Json::UInt(value.height);
                observation["block_hash"]=value.block_hash.GetHex();
                observation["transaction_id"]=transaction.GetHex();
                operation["chain_observation"]=std::move(observation);
            }
            operations.append(std::move(operation));
        }
        const auto checkpoint=found->state.account.Delivery();
        result["account"]=Json::UInt64(found->number);
        result["account_revision"]=Json::UInt64(found->state.revision);
        result["account_sequence"]=Json::UInt64(checkpoint.sequence);
        result["account_digest"]=checkpoint.digest.GetHex();
        result["captured_source_sequence"]=Json::UInt64(view->Head().sequence);
        result["captured_source_digest"]=view->Head().digest.GetHex();
        result["operations"]=std::move(operations);
#else
        (void)ctx;
        throw std::runtime_error("Orchard wallet backend unavailable");
#endif
    } catch (const std::exception& e) {
        OrchardRpcFailure(result,e);
    }
    return result;
}

} // namespace
din::Json rpc_context_wallet_orchard_listoperations(const ExecutionContext& ctx,const din::Json& params) {
    return OrchardAccountRead(ctx,params,OrchardAccountReadMode::Operations);
}
din::Json rpc_context_wallet_orchard_listaccounts(const ExecutionContext& ctx,const din::Json& params) {
    return OrchardAccountRead(ctx,params,OrchardAccountReadMode::Accounts);
}
din::Json rpc_context_wallet_orchard_listreceived(const ExecutionContext& ctx,const din::Json& params) {
    return OrchardAccountRead(ctx,params,OrchardAccountReadMode::Received);
}
din::Json rpc_context_wallet_orchard_getbalance(const ExecutionContext& ctx,const din::Json& params) {
    return OrchardAccountRead(ctx,params,OrchardAccountReadMode::Balance);
}

namespace {
uint64_t SpendInteger(const din::Json& value,const char* field,bool nonzero=false){
    if((value.type()!=::Json::uintValue&&value.type()!=::Json::intValue)||
       (value.type()==::Json::intValue&&value.asInt64()<0))
        throw std::runtime_error(std::string(field)+" must be an unsigned integer");
    const auto number=value.asUInt64();
    if(nonzero&&number==0)throw std::runtime_error(std::string(field)+" must be nonzero");
    return number;
}
std::string SpendText(const din::Json& value,const char* field,size_t maximum,bool empty=false){
    if(!value.isString())throw std::runtime_error(std::string(field)+" must be a string");
    auto text=value.asString();
    if((!empty&&text.empty())||text.size()>maximum||text.find('\0')!=std::string::npos)
        throw std::runtime_error(std::string(field)+" has invalid length or bytes");
    return text;
}
std::vector<uint8_t> SpendHex(const std::string& text){
    if(text.size()%2)throw std::runtime_error("Hex fields require complete bytes");
    const auto digit=[](char c)->uint8_t {
        if(c>='0'&&c<='9')return c-'0';
        if(c>='a'&&c<='f')return c-'a'+10;
        if(c>='A'&&c<='F')return c-'A'+10;
        throw std::runtime_error("Hex field contains an invalid digit");
    };
    std::vector<uint8_t> bytes(text.size()/2);
    for(size_t i=0;i<bytes.size();++i)bytes[i]=uint8_t((digit(text[2*i])<<4)|digit(text[2*i+1]));
    return bytes;
}
struct SpendRecipient { std::string address;uint64_t amount;std::array<uint8_t,512> memo{}; };
struct SpendRequest {
    uint32_t account;uint64_t revision;uint64_t fee;std::array<uint8_t,32> id{};
    std::vector<SpendRecipient> payments,outputs;
};
SpendRequest ParseStoredFinishRequest(const din::Json& params){
    if(!params.isObject()||params.size()!=2||!params.isMember("account")||!params.isMember("request_id"))
        throw std::runtime_error("Stored Orchard completion requires account and request_id only");
    const auto account=SpendInteger(params["account"],"account");
    if(account>=0x80000000ULL)throw std::runtime_error("Orchard account number is out of range");
    SpendRequest request{static_cast<uint32_t>(account),0,0};
    const auto id=SpendHex(SpendText(params["request_id"],"request_id",64));
    if(id.size()!=request.id.size()||std::none_of(id.begin(),id.end(),[](auto n){return n!=0;}))
        throw std::runtime_error("request_id must be nonzero 32-byte hex");
    std::copy(id.begin(),id.end(),request.id.begin());return request;
}
SpendRequest ParseSpendRequest(const din::Json& params){
    constexpr std::array<std::string_view,6> fields{
        "account","request_id","expected_revision","payments","outputs","fee_una"};
    if(!params.isObject()||params.size()!=fields.size())
        throw std::runtime_error("Usage: wallet.orchard.queuespend {account, request_id, expected_revision, payments, outputs, fee_una}");
    for(const auto field:fields)if(!params.isMember(std::string(field)))
        throw std::runtime_error("Missing Orchard spend request field");
    const auto account=SpendInteger(params["account"],"account");
    if(account>=0x80000000ULL)throw std::runtime_error("Orchard account number is out of range");
    SpendRequest request{static_cast<uint32_t>(account),SpendInteger(params["expected_revision"],"expected_revision",true),
        SpendInteger(params["fee_una"],"fee_una")};
    const auto id=SpendHex(SpendText(params["request_id"],"request_id",64));
    if(id.size()!=request.id.size()||std::none_of(id.begin(),id.end(),[](auto n){return n!=0;}))
        throw std::runtime_error("request_id must be nonzero 32-byte hex");
    std::copy(id.begin(),id.end(),request.id.begin());
    const auto parse=[&](const din::Json& list,bool shielded){
        // Matches the bounded request owner. These are input limits, not a
        // guarantee that a plan with this many recipients has enough actions.
        if(!list.isArray()||list.size()>(shielded?8u:1024u))
            throw std::runtime_error("Recipient array has invalid type or size");
        auto& recipients=shielded?request.payments:request.outputs;
        recipients.reserve(list.size());
        for(const auto& entry:list){
            if(!entry.isObject()||!entry.isMember("address")||!entry.isMember("amount_una")||
               entry.size()!=(shielded&&entry.isMember("memo_hex")?3u:2u))
                throw std::runtime_error("Invalid recipient fields");
            SpendRecipient recipient{SpendText(entry["address"],"address",256),SpendInteger(entry["amount_una"],"amount_una",true)};
            if(shielded&&entry.isMember("memo_hex")){
                const auto memo=SpendHex(SpendText(entry["memo_hex"],"memo_hex",1024,true));
                std::copy(memo.begin(),memo.end(),recipient.memo.begin());
            }
            recipients.push_back(std::move(recipient));
        }
    };
    parse(params["payments"],true);parse(params["outputs"],false);
    if(request.payments.empty()&&request.outputs.empty())throw std::runtime_error("At least one recipient is required");
    return request;
}
SpendRequest ParseShieldRequest(const din::Json& params){
    constexpr std::array<std::string_view,5> fields{
        "account","request_id","expected_revision","payments","fee_una"};
    if(!params.isObject()||params.size()!=fields.size())
        throw std::runtime_error("Usage: wallet.orchard.queueshield {account, request_id, expected_revision, payments, fee_una}");
    for(const auto field:fields)if(!params.isMember(std::string(field)))
        throw std::runtime_error("Missing Orchard shield request field");
    auto normalized=params;normalized["outputs"]=din::Json(Json::arrayValue);
    return ParseSpendRequest(normalized); // Same strict integer/address/memo/id parsing.
}
#if DINERO_WALLET_RAW_ORCHARD
std::vector<uint8_t> SpendOutputScript(const std::string& address,uint8_t network){
    if(network>2)throw std::runtime_error("Unsupported Orchard source network");
    const std::string hrp=network==0?"din":network==1?"tdin":"rdin";
    const auto witness=dinero::DecodeWitnessAddress(address,hrp);
    if(witness.is_valid){
        const bool supported=witness.witness_version==0||
            ((witness.witness_version==1||witness.witness_version==dinero::wallet::P2MR_WITNESS_VERSION)&&
             witness.witness_program.size()==32);
        if(!supported)throw std::runtime_error("Unsupported transparent witness destination");
        return witness.script_pubkey;
    }
    // Decode the exact historical destination type. Never derive a replacement
    // script from address text, an HD path, or the current import convention.
    std::vector<uint8_t> payload;
    if(address.size()>35||!dinero::Address::decodeBase58Check(address,payload)||payload.size()!=21||
       dinero::Address::encodeBase58Check(payload)!=address)
        throw std::runtime_error("Invalid transparent address for the selected network");
    const auto pkh=network==0?dinero::AddressVersion::DINERO_P2PKH:dinero::AddressVersion::DINERO_TESTNET_P2PKH;
    const auto sh=network==0?dinero::AddressVersion::DINERO_P2SH:dinero::AddressVersion::DINERO_TESTNET_P2SH;
    std::vector<uint8_t> script;
    if(payload[0]==pkh)script={0x76,0xa9,0x14};
    else if(payload[0]==sh)script={0xa9,0x14};
    else throw std::runtime_error("Transparent address is for another network");
    script.insert(script.end(),payload.begin()+1,payload.end());
    if(payload[0]==pkh)script.insert(script.end(),{0x88,0xac});
    else script.push_back(0x87);
    return script;
}
#endif
} // namespace

namespace {
din::Json OrchardSpendCall(const ExecutionContext& ctx,const din::Json& params,bool finishing){
    din::Json result;
    try {
        const bool stored_only=finishing&&params.isObject()&&params.size()==2;
        auto request=stored_only?ParseStoredFinishRequest(params):ParseSpendRequest(params);
#if DINERO_WALLET_RAW_ORCHARD
        if(!ctx.daemon)throw std::runtime_error("Daemon services unavailable");
        auto source=std::dynamic_pointer_cast<dinero::ChainstateService>(ctx.daemon->chainstate);
        auto wallet=std::dynamic_pointer_cast<dinero::WalletService>(ctx.daemon->wallet);
        auto source_use=dinero::ChainstateService::AcquireWalletIndexUse(source);
        auto wallet_use=dinero::WalletService::AcquireWalletUse(wallet);
        uint64_t session;
        {
            auto lease=wallet_use->Wallet().AcquireDatabaseLease();
            CheckOrchardRequestWallet(ctx,*lease);
            session=lease->Session();
        }
        if(source->IsInSafeMode())throw std::runtime_error("Orchard spending unavailable in safe mode");
        // Capture before SQLite/key ownership. The bound request owner rechecks
        // the same wallet session and authenticates the complete catalog.
        const auto view=source->getRuntimeAccountReplayForWallet(wallet_use->Wallet(),session);
        if(!view.ok())throw std::runtime_error("Authenticated Orchard replay source unavailable");
        const auto origin_event=(*view)->Event(1);const auto& context=origin_event->context;
        if(context.domain.network_code>2)throw std::runtime_error("Unsupported Orchard source network");
        const dinero::wallet::OrchardAccountDelivery::Profile profile{context.domain,context.activation_height,request.account};
        std::unique_ptr<dinero::wallet::OrchardAccountDelivery::RequestProof> stored;
        if(stored_only){
            stored=std::make_unique<dinero::wallet::OrchardAccountDelivery::RequestProof>(
                dinero::wallet::OrchardAccountDelivery::ReadStoredCatalogRequestProofForReplay(
                    wallet_use->Wallet(),session,profile,**view,request.id,false,wallet_use->OrchardProofs(),true));
            request.fee=stored->durable.spend_request->fee_una;
        }
        if(request.fee>dinero::orchard::kMaxMoneyUna)throw std::runtime_error("Fee exceeds money range");
        uint64_t total=request.fee;
        const auto amount=[&](uint64_t value){
            if(value>dinero::orchard::kMaxMoneyUna-total)throw std::runtime_error("Spend exceeds money range");
            total+=value;
        };
        std::vector<dinero::orchard::WalletPayment> payments;
        std::vector<dinero::orchard::TransparentOutput> outputs;
        payments.reserve(request.payments.size());outputs.reserve(request.outputs.size());
        for(const auto& item:request.payments){
            amount(item.amount);
            payments.push_back({item.amount,dinero::orchard::WalletReceiver::DecodeAddress(item.address,
                static_cast<dinero::orchard::WalletNetwork>(context.domain.network_code)),item.memo});
        }
        for(const auto& item:request.outputs){amount(item.amount);outputs.push_back({item.amount,SpendOutputScript(item.address,context.domain.network_code)});}
        if(stored){
            const auto& details=*stored->durable.spend_request;
            for(const auto& item:details.payments){amount(item.amount_una);
                payments.push_back({item.amount_una,dinero::orchard::WalletReceiver::DecodeAddress(item.address,
                    static_cast<dinero::orchard::WalletNetwork>(context.domain.network_code)),item.memo});}
            for(const auto& item:details.outputs){amount(item.amount_una);outputs.push_back(item);}
        }
        if(finishing){
            // Retain the configured actual service, not a borrowed raw ingress
            // pointer. SubmitBody owns its pool use and acquires fresh selected
            // state; no wallet SQLite/key owner survives this RPC handoff.
            auto ingress=std::dynamic_pointer_cast<dinero::MempoolService>(ctx.daemon->mempool);
            if(!ingress || ctx.daemon->tx_ingress!=ingress.get())
                throw std::runtime_error("Canonical Orchard transaction ingress unavailable");
            auto& jobs=wallet_use->OrchardProofs();
            const auto captured=stored?std::move(*stored):dinero::wallet::OrchardAccountDelivery::ReadCatalogRequestProofForReplay(
                wallet_use->Wallet(),session,profile,**view,request.id,payments,outputs,request.fee,jobs,true);
            using Queue=dinero::wallet::OrchardOperationQueue;
            const auto envelope=[&]{
                if(captured.durable.phase==Queue::Phase::Ready)
                    return dinero::orchard::TransactionEnvelope::DecodeExact(captured.durable.transaction);
                if(captured.durable.phase!=Queue::Phase::Reserved)
                    throw std::runtime_error("Unsupported Orchard durable operation state");
                if(captured.state!=dinero::wallet::OrchardProofJobs::State::Succeeded || !captured.proof)
                    throw dinero::wallet::OrchardProofUnavailable(captured.state,
                        "Owned Orchard proof is not available; reservation retained");
                return dinero::orchard::TransactionEnvelope::Create(0,{},outputs,request.fee,captured.proof->Bytes());
            }();
            const auto authorization=source->AuthorizeOrchardWalletTransaction(envelope,profile.domain,profile.activation);
            if(!authorization)throw std::runtime_error("Orchard wallet authorizations unavailable");
            const auto finalized=dinero::wallet::OrchardAccountDelivery::FinalizeCatalogProofForReplay(
                wallet_use->Wallet(),session,profile,**view,request.id,*authorization,jobs);
            // Finalize reauthenticates ownership and commits these exact bytes
            // before retiring the job. Admission may fail or be interrupted;
            // retry retains the same Ready transaction, never a fresh proof.
            const auto body=dinero::MempoolTransaction::FromOrchard(authorization->Transaction());
            const auto submitted=ingress->SubmitBody(body,dinero::TxOrigin::WALLET);
            result["operation_id"]=util::hex(std::vector<unsigned char>(request.id.begin(),request.id.end()));
            result["account"]=Json::UInt64(request.account);result["account_revision"]=Json::UInt64(finalized.state.revision);
            result["durable_state"]="signed";result["txid"]=body.GetTxid().AsUint256().GetHex();
            result["admitted"]=submitted.accepted();
            result["already_in_mempool"]=submitted.code==dinero::TxRejectCode::ALREADY_IN_MEMPOOL;
            result["submission_code"]=dinero::TxRejectCodeToString(submitted.code);
            result["submission_message"]=submitted.message;
            return result;
        }
        const auto queued=dinero::wallet::OrchardAccountDelivery::QueueCatalogRequestForReplay(
            wallet_use->Wallet(),session,profile,request.revision,**view,request.id,payments,outputs,request.fee,wallet_use->OrchardProofs());
        if(!queued->durable)throw std::runtime_error("Missing committed Orchard request");
        result["operation_id"]=util::hex(std::vector<unsigned char>(queued->operation_id.begin(),queued->operation_id.end()));
        result["account"]=Json::UInt64(request.account);result["account_revision"]=Json::UInt64(queued->revision);
        result["proof_queued"]=queued->enqueued;result["existing_request"]=queued->existing_request;result["archived"]=queued->archived;
        switch(queued->durable->phase){
            case dinero::wallet::OrchardOperationQueue::Phase::Reserved:result["durable_state"]="reserved";break;
            case dinero::wallet::OrchardOperationQueue::Phase::Ready:result["durable_state"]="signed";break;
            default:throw std::runtime_error("Unsupported Orchard durable operation state");
        }
#else
        (void)ctx;(void)request;throw std::runtime_error("Orchard wallet backend unavailable");
#endif
    } catch(const std::exception& e){OrchardRpcFailure(result,e);}
    return result;
}

} // namespace
din::Json rpc_context_wallet_orchard_queuespend(const ExecutionContext& ctx,const din::Json& params){return OrchardSpendCall(ctx,params,false);}
din::Json rpc_context_wallet_orchard_finishspend(const ExecutionContext& ctx,const din::Json& params){return OrchardSpendCall(ctx,params,true);}

namespace {
din::Json OrchardShieldCall(const ExecutionContext& ctx,const din::Json& params,bool finishing){
    din::Json result;
    try{
        const bool stored_only=finishing&&params.isObject()&&params.size()==2;
        auto request=stored_only?ParseStoredFinishRequest(params):ParseShieldRequest(params);
#if DINERO_WALLET_RAW_ORCHARD
        if(!ctx.daemon)throw std::runtime_error("Daemon services unavailable");
        auto source=std::dynamic_pointer_cast<dinero::ChainstateService>(ctx.daemon->chainstate);
        auto wallet=std::dynamic_pointer_cast<dinero::WalletService>(ctx.daemon->wallet);
        auto source_use=dinero::ChainstateService::AcquireWalletIndexUse(source);
        auto wallet_use=dinero::WalletService::AcquireWalletUse(wallet);
        uint64_t session;
        {auto lease=wallet_use->Wallet().AcquireDatabaseLease();
         CheckOrchardRequestWallet(ctx,*lease);
         session=lease->Session();}
        if(source->IsInSafeMode())throw std::runtime_error("Orchard shielding unavailable in safe mode");
        const auto view=source->getRuntimeAccountReplayForWallet(wallet_use->Wallet(),session);
        if(!view.ok())throw std::runtime_error("Authenticated shield replay source unavailable");
        const auto origin_event=(*view)->Event(1);const auto& context=origin_event->context;
        const dinero::wallet::OrchardAccountDelivery::Profile profile{context.domain,context.activation_height,request.account};
        if(stored_only){
            const auto stored=dinero::wallet::OrchardAccountDelivery::ReadStoredCatalogRequestProofForReplay(
                wallet_use->Wallet(),session,profile,**view,request.id,true,wallet_use->OrchardProofs());
            const auto& details=*stored.durable.shield_request;request.fee=details.fee_una;
            for(const auto& item:details.payments)request.payments.push_back({item.address,item.amount_una,item.memo});
        }
        if(context.domain.network_code>2||request.fee>dinero::orchard::kMaxMoneyUna)
            throw std::runtime_error("Shield network or fee outside supported range");
        uint64_t total=request.fee;std::vector<dinero::orchard::WalletPayment> payments;
        for(const auto& item:request.payments){
            if(!item.amount||item.amount>dinero::orchard::kMaxMoneyUna-total)throw std::runtime_error("Shield amount outside money range");
            total+=item.amount;
            payments.push_back({item.amount,dinero::orchard::WalletReceiver::DecodeAddress(item.address,
                static_cast<dinero::orchard::WalletNetwork>(context.domain.network_code)),item.memo});
        }
        result["operation_id"]=util::hex(std::vector<unsigned char>(request.id.begin(),request.id.end()));
        result["account"]=Json::UInt64(request.account);
        auto& jobs=wallet_use->OrchardProofs();
        if(finishing){
            auto ingress=std::dynamic_pointer_cast<dinero::MempoolService>(ctx.daemon->mempool);
            if(!ingress||ctx.daemon->tx_ingress!=ingress.get())throw std::runtime_error("Canonical Orchard transaction ingress unavailable");
            const auto stored=dinero::wallet::OrchardAccountDelivery::FindStoredShieldRequestForReplay(
                wallet_use->Wallet(),session,profile,**view,request.id,payments,request.fee);
            if(!stored||!stored->durable)throw std::runtime_error("Unknown shield request; completion does not select inputs");
            if(stored->archived)throw std::runtime_error("Shield request is archived; no new submission");
            const auto bytes=source->finalizeRuntimeWalletShield(wallet_use->Wallet(),session,request.account,request.id,
                stored->durable->inputs,payments,stored->transparent_outputs,request.fee,jobs);
            const auto envelope=dinero::orchard::TransactionEnvelope::DecodeExact(bytes);
            const auto body=dinero::MempoolTransaction::FromOrchard(envelope);
            // Ready already committed. No wallet SQLite or selected owner is
            // held here; admission captures its own current source/pool state.
            const auto submitted=ingress->SubmitBody(body,dinero::TxOrigin::WALLET);
            result["durable_state"]="signed";result["txid"]=body.GetTxid().AsUint256().GetHex();
            result["admitted"]=submitted.accepted();
            result["already_in_mempool"]=submitted.code==dinero::TxRejectCode::ALREADY_IN_MEMPOOL;
            result["submission_code"]=dinero::TxRejectCodeToString(submitted.code);result["submission_message"]=submitted.message;
            return result;
        }
        const auto queued=source->queueRuntimeWalletShieldPayment(wallet_use->Wallet(),session,request.account,
            request.revision,request.id,payments,request.fee,jobs);
        if(!queued||!queued->durable||!queued->durable->shield_request)throw std::runtime_error("Missing committed complete shield request");
        result["account_revision"]=Json::UInt64(queued->revision);result["proof_queued"]=queued->enqueued;
        result["existing_request"]=queued->existing_request;result["archived"]=queued->archived;
        result["input_count"]=Json::UInt64(queued->durable->inputs.size());
        switch(queued->durable->phase){
            case dinero::wallet::OrchardOperationQueue::Phase::Reserved:result["durable_state"]="reserved";break;
            case dinero::wallet::OrchardOperationQueue::Phase::Ready:result["durable_state"]="signed";break;
            default:throw std::runtime_error("Unsupported Orchard shield operation phase");
        }
#else
        (void)ctx;(void)request;(void)finishing;throw std::runtime_error("Orchard wallet backend unavailable");
#endif
    }catch(const std::exception& e){OrchardRpcFailure(result,e);}
    return result;
}
}
din::Json rpc_context_wallet_orchard_queueshield(const ExecutionContext& ctx,const din::Json& params){return OrchardShieldCall(ctx,params,false);}
din::Json rpc_context_wallet_orchard_finishshield(const ExecutionContext& ctx,const din::Json& params){return OrchardShieldCall(ctx,params,true);}

din::Json rpc_context_orchard_getactivationstatus(const ExecutionContext& ctx,const din::Json& params) {
    din::Json result;
    try {
        if(!params.isNull() && !((params.isObject() || params.isArray()) && params.empty()))
            throw OrchardBindingError("invalid_parameters","orchard.getactivationstatus takes no parameters");
        // Parameters are selected once at process startup. The source capture
        // owns the service lifetime and verifies one selected live/durable tip.
        // It returns no tuple when shutdown, safe mode or inconsistent storage
        // prevents that read. This call never acquires a wallet or changes rules.
        const auto profile=dinero::Params();
        if(!dinero::consensus::ReleaseProfileConfigurationValid(profile) ||
           !dinero::consensus::OrchardProfileConfigurationValid(profile))
            throw OrchardBindingError("activation_profile_invalid","Orchard release profile is inconsistent");
        if(!ctx.daemon)throw OrchardBindingError("activation_status_unavailable","Selected chainstate unavailable");
        auto source=std::dynamic_pointer_cast<dinero::ChainstateService>(ctx.daemon->chainstate);
        if(!source)throw OrchardBindingError("activation_status_unavailable","Selected chainstate unavailable");
        const auto captured=source->getUtreexoRpcSnapshot();
        if(!captured.ok())throw OrchardBindingError("activation_status_unavailable","Selected chainstate observation unavailable");
        const bool configured=profile.orchard_activation_height!=UINT32_MAX;
        const bool active=configured && captured->height>=profile.orchard_activation_height;
        const uint64_t next=uint64_t(captured->height)+1;
        result["network"]=profile.name;
        result["activation_state"]=!configured?"unscheduled":active?"active":"scheduled";
        result["activation_height"]=configured?din::Json(Json::UInt64(profile.orchard_activation_height)):din::Json{};
        result["branch_id"]=configured?din::Json(Json::UInt64(profile.orchard_branch_id)):din::Json{};
        result["tip_height"]=Json::UInt64(captured->height);
        result["tip_hash"]=captured->block_hash.GetHex();
        result["next_block_height"]=Json::UInt64(next);
        result["rule_active_at_tip"]=active;
        result["rule_active_for_next_block"]=configured && next>=profile.orchard_activation_height;
        result["storage_mode"]=captured->compact?"compact":"full";
#if DINERO_WALLET_RAW_ORCHARD
        result["wallet_backend_compiled"]=true;
#else
        result["wallet_backend_compiled"]=false;
#endif
    } catch(const std::exception& error) {OrchardRpcFailure(result,error);}
    return result;
}

void RegisterOrchardAccountRpc(){
    const RpcMethodMeta activation{
        "orchard.getactivationstatus","blockchain",
        "Read the configured Orchard rule at one verified selected-chain observation.",
        {},{"object","Network, optional activation height and branch ID, selected tip and separate tip/next-block rule flags."},
        "No wallet is selected or unlocked. Unscheduled height and branch ID are null. Errors return no activation or tip fields. This is an as-of observation of configured consensus rules, not synchronization, release, wallet, proof, mempool or spend readiness. A boundary parent is scheduled at its tip even when the next block uses Orchard. No wall-clock activation estimate is returned."};
    g_rpcRegistry.registerHandler(activation.name,rpc_context_orchard_getactivationstatus,activation,RegisterMode::Overwrite,"orchard-activation-observation");
    const RpcMethodMeta binding{
        "wallet.orchard.getwalletbinding","wallet",
        "Read a binding for the explicitly named current wallet selection.",
        {{"wallet_name","string","Intended wallet name; this call never switches wallets.",true}},
        {"object","Opaque wallet_binding and selected wallet_name."},
        "No keys, balances or readiness are returned. Requires an existing database identity; never enrolls a wallet. Binding changes on reopen, switch or manager/process replacement. RPC authentication remains required independently."};
    g_rpcRegistry.registerHandler(binding.name,rpc_context_wallet_orchard_getwalletbinding,binding,RegisterMode::Overwrite,"orchard-account-owner");
    const RpcMethodMeta shielding{
        "wallet.orchard.queueshield","wallet",
        "Select owned mature transparent coins and durably reserve a shield request before queueing its proof.",
        {{"account","integer","Existing Orchard operation owner account.",true},
         {"request_id","string","Nonzero 32-byte hex request ID; reuse only for the same intent.",true},
         {"expected_revision","integer","Current account revision for new selection; original revision for retry.",true},
         {"payments","array","Ordered Orchard recipients: address, amount_una and optional memo_hex (up to 512 bytes).",true},
         {"fee_una","integer","Explicit fee in atomic units.",true}},
        {"object","Durable request state, selected input count, account revision and proof queue status."},
        "Requires an unlocked wallet, complete authenticated catalog and checked selected source. Automatically selects supported owned coins, excludes retained reservations/manual locks and uses real wallet change issuance. Matching retries retain the original inputs/change without requeueing. Old requests with unknown details refuse. Queueing is not submission or confirmation."};
    RegisterBoundOrchard(shielding,rpc_context_wallet_orchard_queueshield);
    const RpcMethodMeta shield_completion{
        "wallet.orchard.finishshield","wallet",
        "Sign an existing owned shield proof, commit its exact Ready bytes, then submit through current admission.",
        {{"account","integer","Existing Orchard operation owner account.",true},
         {"request_id","string","Existing nonzero 32-byte hex request ID.",true},
         {"expected_revision","integer","Original revision when resending the complete intent.",false},
         {"payments","array","Original recipients when resending the complete intent.",false},
         {"fee_una","integer","Original fee when resending the complete intent.",false}},
        {"object","Durable signed transaction ID and separate admission result."},
        "Provide account and request_id only to use authenticated stored details, or resend the complete original queue intent. Completion loads the retained selected inputs/change; it never selects replacements or regenerates missing proofs. Signing and full source verification precede Ready commit; submission follows outside owners. Failed admission retains the same bytes for retry. Archived requests are not resubmitted."};
    RegisterBoundOrchard(shield_completion,rpc_context_wallet_orchard_finishshield);

    const RpcMethodMeta spending{
        "wallet.orchard.queuespend","wallet",
        "Reserve an Orchard-funded payment and queue its proof under an exact durable request ID.",
        {{"account","integer","Existing source account number.",true},
         {"request_id","string","Nonzero 32-byte hex request ID; reuse only for the same payment.",true},
         {"expected_revision","integer","Current account revision for a new request; original revision for a retry.",true},
         {"payments","array","Ordered Orchard recipients: address, amount_una and optional memo_hex (up to 512 bytes).",true},
         {"outputs","array","Ordered transparent recipients: address and amount_una.",true},
         {"fee_una","integer","Explicit fee in atomic units.",true}},
        {"object","Operation ID, committed account revision, durable state and whether this call queued a proof."},
        "Requires an unlocked wallet, complete authenticated account catalog and checked replay source. Amounts are integers in atomic units. Matching retries preserve the original reservation without requeueing. Queued and reserved do not mean sent, admitted or confirmed. No cancellation, transparent-input shielding or automatic submission is provided by this method."};
    RegisterBoundOrchard(spending,rpc_context_wallet_orchard_queuespend);

    const RpcMethodMeta completion{
        "wallet.orchard.finishspend","wallet",
        "Commit the owned proof of an existing Orchard request, then submit its exact signed transaction.",
        {{"account","integer","Existing source account number.",true},
         {"request_id","string","Nonzero 32-byte hex request ID; reuse only for the same payment.",true},
         {"expected_revision","integer","Original request revision; completion never reserves a new request.",false},
         {"payments","array","Ordered Orchard recipients: address, amount_una and optional memo_hex (up to 512 bytes).",false},
         {"outputs","array","Ordered transparent recipients: address and amount_una.",false},
         {"fee_una","integer","Explicit fee in atomic units.",false}},
        {"object","Operation ID, signed transaction ID, durable revision and separate submission result."},
        "Provide account and request_id only to use authenticated stored details, or resend the exact original request fields. Requires a current authenticated request and its completed owned proof, or previously committed signed bytes. Missing jobs and unknown or archived IDs refuse without regeneration. Signed bytes commit before fresh selected-chain and mempool admission; rejection retains them for retry. Admission does not confirm delivery to peers or inclusion in a block."};
    RegisterBoundOrchard(completion,rpc_context_wallet_orchard_finishspend);

    const RpcMethodMeta metadata{
        "wallet.orchard.getnewaddress","wallet",
        "Persist and return an external receiving address for an existing authenticated Orchard account.",
        {{"account","integer","Existing account number (0 through 2147483647).",true}},
        {"object","Address, account number and committed account revision."},
        "Requires an unlocked wallet, authenticated generated account catalog and complete current/reached account history. Unknown recovery inventory refuses. Does not create an account or report spend readiness."};
    RegisterBoundOrchard(metadata,rpc_context_wallet_orchard_getnewaddress);
    const RpcMethodMeta balance{
        "wallet.orchard.getbalance","wallet",
        "Read authenticated confirmed Orchard note amounts and durable reservations at an account checkpoint.",
        {{"account","integer","Existing account number (0 through 2147483647).",true}},
        {"object","Integer atomic-unit balances, account revision, checkpoint and captured source progress."},
        "Requires an unlocked wallet and complete authenticated generated account catalog. confirmed_una includes notes unspent at the reported checkpoint; reserved_confirmed_una counts those notes held by durable operations; unreserved_confirmed_una is their difference. A lagging checkpoint is not current balance. account_caught_up_to_captured_source refers only to this account and captured source, not lasting readiness or other accounts. Unreserved funds are not a spendability, fee, proof, mempool or relay guarantee. Errors return no balances. No pending incoming amount is inferred."};
    RegisterBoundOrchard(balance,rpc_context_wallet_orchard_getbalance);
    const RpcMethodMeta history{
        "wallet.orchard.listreceived","wallet",
        "Read a revision-bound page of authenticated selected-chain Orchard receipts, including spent notes and internal change.",
        {{"account","integer","Existing account number.",true},
         {"offset","integer","Zero-based receipt offset; defaults to zero.",false},
         {"limit","integer","Page size from1 through1000; defaults to100.",false},
         {"expected_revision","integer","Required for nonzero offset; stale revisions refuse.",false}},
        {"object","Receipt page, explicit next offset, total count, account revision and captured checkpoints."},
        "Requires an unlocked bound wallet and whole authenticated catalog restoration. Old snapshots without complete receipt history return history_incomplete; no unspent-only fallback is reported as history. Each row contains the exact txid/action, scope, amount_una, recipient_hex, memo_hex, height and block_hash. Internal change is explicitly scoped. A receipt does not establish an external sender, current unspentness, spendability or broadcast status. Completeness is through the account checkpoint, not a lagging source tip. Continue with next_offset and the same account_revision as expected_revision, or restart pagination after stale_account_revision. No writes or fee/label inference."};
    RegisterBoundOrchard(history,rpc_context_wallet_orchard_listreceived);
    const RpcMethodMeta account_list{
        "wallet.orchard.listaccounts","wallet",
        "List all authenticated catalog-owned Orchard accounts and their observed checkpoints.",
        {},
        {"object","Accounts with revisions/checkpoints and one captured source position."},
        "Requires an unlocked bound wallet and the same complete authenticated catalog restoration used by account reads. Missing, corrupt or incomplete catalog state refuses without an accounts prefix. No account is created or inferred from a default number. An empty list is only an authenticated catalog observation, not authorization to regenerate historical owners or a spend-readiness certificate."};
    RegisterBoundOrchard(account_list,rpc_context_wallet_orchard_listaccounts);
    const RpcMethodMeta operations{
        "wallet.orchard.listoperations","wallet",
        "List authenticated durable pending Orchard operations for an existing account.",
        {{"account","integer","Existing account number (0 through 2147483647).",true}},
        {"object","Account revision, captured progress and operation IDs with durable states."},
        "Requires an unlocked wallet and complete authenticated generated account catalog. Reserved and signed describe stored wallet states. Signed entries include their transaction ID. chain_observation is null when no outcome is recorded, otherwise it identifies the confirmed or conflicting transaction and block at the reported account checkpoint. Compare account and captured source progress before interpreting it as current. Does not return signed bytes or change reservations."};
    RegisterBoundOrchard(operations,rpc_context_wallet_orchard_listoperations);
    const RpcMethodMeta creation{
        "wallet.orchard.createaccount","wallet",
        "Create a new catalog-owned Orchard account and return its first receiving address.",
        {{"account","integer","Previously unused account number (0 through 2147483647).",true}},
        {"object","Address, account number, committed account revision and synchronization requirement."},
        "Requires an unlocked generated wallet with authenticated complete account ownership and an available checked source. Recovery or legacy unknown inventory refuses. The account must synchronize before spending."};
    RegisterBoundOrchard(creation,rpc_context_wallet_orchard_createaccount);

}
