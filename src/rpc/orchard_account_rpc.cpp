#include "rpc/orchard_account_rpc.h"
#include "daemon/daemon_context.h"
#include "daemon/services/chainstate_service.h"
#include "daemon/services/wallet_service.h"
#include <stdexcept>
#include <algorithm>
#include <array>
#include <string_view>
#include <vector>
#include "util/hex.h"
#if DINERO_WALLET_RAW_ORCHARD
#include "wallet/orchard_account_delivery.h"
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

din::Json rpc_context_wallet_orchard_queuespend(const ExecutionContext& ctx,const din::Json& params){
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
        const auto& context=(*view)->Event(1).context;
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

void RegisterOrchardAccountRpc(){
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
