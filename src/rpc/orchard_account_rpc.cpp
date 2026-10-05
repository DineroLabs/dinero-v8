#include "rpc/orchard_account_rpc.h"
#include "daemon/daemon_context.h"
#include "daemon/services/chainstate_service.h"
#include "daemon/services/wallet_service.h"
#include <stdexcept>
#include <algorithm>
#include <array>
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
        result.clear();
        result["error"]=e.what();
    }
    return result;
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
        const auto request=ParseSpendRequest(params);
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
        if(source->IsInSafeMode())throw std::runtime_error("Orchard spending unavailable in safe mode");
        // Capture before SQLite/key ownership. The bound request owner rechecks
        // the same wallet session and authenticates the complete catalog.
        const auto view=source->getRuntimeAccountReplayForWallet(wallet_use->Wallet(),session);
        if(!view.ok())throw std::runtime_error("Authenticated Orchard replay source unavailable");
        const auto origin_event=(*view)->Event(1);const auto& context=origin_event->context;
        if(context.domain.network_code>2)throw std::runtime_error("Unsupported Orchard source network");
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
        const dinero::wallet::OrchardAccountDelivery::Profile profile{context.domain,context.activation_height,request.account};
        if(finishing){
            // Retain the configured actual service, not a borrowed raw ingress
            // pointer. SubmitBody owns its pool use and acquires fresh selected
            // state; no wallet SQLite/key owner survives this RPC handoff.
            auto ingress=std::dynamic_pointer_cast<dinero::MempoolService>(ctx.daemon->mempool);
            if(!ingress || ctx.daemon->tx_ingress!=ingress.get())
                throw std::runtime_error("Canonical Orchard transaction ingress unavailable");
            auto& jobs=wallet_use->OrchardProofs();
            const auto captured=dinero::wallet::OrchardAccountDelivery::ReadCatalogRequestProofForReplay(
                wallet_use->Wallet(),session,profile,**view,request.id,payments,outputs,request.fee,jobs);
            using Queue=dinero::wallet::OrchardOperationQueue;
            const auto envelope=[&]{
                if(captured.durable.phase==Queue::Phase::Ready)
                    return dinero::orchard::TransactionEnvelope::DecodeExact(captured.durable.transaction);
                if(captured.durable.phase!=Queue::Phase::Reserved ||
                   captured.state!=dinero::wallet::OrchardProofJobs::State::Succeeded || !captured.proof)
                    throw std::runtime_error("Owned Orchard proof is not available; reservation retained");
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
    } catch(const std::exception& e){result.clear();result["error"]=e.what();}
    return result;
}

} // namespace
din::Json rpc_context_wallet_orchard_queuespend(const ExecutionContext& ctx,const din::Json& params){return OrchardSpendCall(ctx,params,false);}
din::Json rpc_context_wallet_orchard_finishspend(const ExecutionContext& ctx,const din::Json& params){return OrchardSpendCall(ctx,params,true);}

namespace {
din::Json OrchardShieldCall(const ExecutionContext& ctx,const din::Json& params,bool finishing){
    din::Json result;
    try{
        const auto request=ParseShieldRequest(params);
#if DINERO_WALLET_RAW_ORCHARD
        if(!ctx.daemon)throw std::runtime_error("Daemon services unavailable");
        auto source=std::dynamic_pointer_cast<dinero::ChainstateService>(ctx.daemon->chainstate);
        auto wallet=std::dynamic_pointer_cast<dinero::WalletService>(ctx.daemon->wallet);
        auto source_use=dinero::ChainstateService::AcquireWalletIndexUse(source);
        auto wallet_use=dinero::WalletService::AcquireWalletUse(wallet);
        uint64_t session;
        {auto lease=wallet_use->Wallet().AcquireDatabaseLease();
         if(!ctx.walletName.empty()&&ctx.walletName!=lease->WalletName())throw std::runtime_error("Selected wallet does not match request");
         session=lease->Session();}
        if(source->IsInSafeMode())throw std::runtime_error("Orchard shielding unavailable in safe mode");
        const auto view=source->getRuntimeAccountReplayForWallet(wallet_use->Wallet(),session);
        if(!view.ok())throw std::runtime_error("Authenticated shield replay source unavailable");
        const auto origin_event=(*view)->Event(1);const auto& context=origin_event->context;
        if(context.domain.network_code>2||request.fee>dinero::orchard::kMaxMoneyUna)
            throw std::runtime_error("Shield network or fee outside supported range");
        uint64_t total=request.fee;std::vector<dinero::orchard::WalletPayment> payments;
        for(const auto& item:request.payments){
            if(!item.amount||item.amount>dinero::orchard::kMaxMoneyUna-total)throw std::runtime_error("Shield amount outside money range");
            total+=item.amount;
            payments.push_back({item.amount,dinero::orchard::WalletReceiver::DecodeAddress(item.address,
                static_cast<dinero::orchard::WalletNetwork>(context.domain.network_code)),item.memo});
        }
        const dinero::wallet::OrchardAccountDelivery::Profile profile{context.domain,context.activation_height,request.account};
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
    }catch(const std::exception& e){result.clear();result["error"]=e.what();}
    return result;
}
}
din::Json rpc_context_wallet_orchard_queueshield(const ExecutionContext& ctx,const din::Json& params){return OrchardShieldCall(ctx,params,false);}
din::Json rpc_context_wallet_orchard_finishshield(const ExecutionContext& ctx,const din::Json& params){return OrchardShieldCall(ctx,params,true);}

void RegisterOrchardAccountRpc(){
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
    g_rpcRegistry.registerHandler(shielding.name,rpc_context_wallet_orchard_queueshield,shielding,RegisterMode::Overwrite,"orchard-account-owner");
    const RpcMethodMeta shield_completion{
        "wallet.orchard.finishshield","wallet",
        "Sign an existing owned shield proof, commit its exact Ready bytes, then submit through current admission.",
        shielding.params,
        {"object","Durable signed transaction ID and separate admission result."},
        "Resend the same original queue intent. Completion loads the retained selected inputs/change; it never selects replacements or regenerates missing proofs. Signing and full source verification precede Ready commit; submission follows outside owners. Failed admission retains the same bytes for retry. Archived requests are not resubmitted."};
    g_rpcRegistry.registerHandler(shield_completion.name,rpc_context_wallet_orchard_finishshield,shield_completion,RegisterMode::Overwrite,"orchard-account-owner");

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
    g_rpcRegistry.registerHandler(spending.name,rpc_context_wallet_orchard_queuespend,
        spending,RegisterMode::Overwrite,"orchard-account-owner");

    const RpcMethodMeta completion{
        "wallet.orchard.finishspend","wallet",
        "Commit the owned proof of an existing Orchard request, then submit its exact signed transaction.",
        {{"account","integer","Existing source account number.",true},
         {"request_id","string","Nonzero 32-byte hex request ID; reuse only for the same payment.",true},
         {"expected_revision","integer","Original request revision; completion never reserves a new request.",true},
         {"payments","array","Ordered Orchard recipients: address, amount_una and optional memo_hex (up to 512 bytes).",true},
         {"outputs","array","Ordered transparent recipients: address and amount_una.",true},
         {"fee_una","integer","Explicit fee in atomic units.",true}},
        {"object","Operation ID, signed transaction ID, durable revision and separate submission result."},
        "Resend the exact original request fields. Requires a current authenticated request and its completed owned proof, or previously committed signed bytes. Missing jobs and unknown or archived IDs refuse without regeneration. Signed bytes commit before fresh selected-chain and mempool admission; rejection retains them for retry. Admission does not confirm delivery to peers or inclusion in a block."};
    g_rpcRegistry.registerHandler(completion.name,rpc_context_wallet_orchard_finishspend,
        completion,RegisterMode::Overwrite,"orchard-account-owner");

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
        "Requires an unlocked wallet and complete authenticated generated account catalog. Reserved and signed describe stored wallet states. Signed entries include their transaction ID. chain_observation is null when no outcome is recorded, otherwise it identifies the confirmed or conflicting transaction and block at the reported account checkpoint. Compare account and captured source progress before interpreting it as current. Does not return signed bytes or change reservations."};
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
