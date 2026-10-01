#include "vault/wallet_withdrawal_dispatch.h"
#include "rpc/wallet_request_dispatch.h"
#include "daemon/daemon_context.h"
#include "daemon/services/wallet_service.h"
#include "daemon/services/chainstate_service.h"
#include "storage/chain_db.h"
#include "consensus/merkle_root.h"
#include "daemon/mempool_transaction.h"
#include "wallet/wallet_manager.h"
#include "address/addr_codec.h"
#include "external/bech32/bech32.hpp"
#include "consensus/chainparams.h"
#include "primitives/transaction.h"
#include "primitives/uint256.h"
#include "util/hex.h"
#include <openssl/sha.h>
#include <algorithm>
#include <limits>
#include <stdexcept>
#include <condition_variable>
#include <map>
#include <mutex>
#include <thread>

namespace dinero::vault {
namespace {
void Require(bool value) {if(!value)throw std::runtime_error("vault wallet withdrawal owner or payment binding mismatch");}
bool Nonzero(const auto& bytes) {return std::any_of(bytes.begin(),bytes.end(),[](uint8_t v){return v!=0;});}
} // namespace
// The gate mutex is held only for admission/release bookkeeping. It is never
// held during wallet/SQLite/chain/RPC work or while invoking external callbacks.
struct WalletWithdrawalDispatchState : std::enable_shared_from_this<WalletWithdrawalDispatchState> {
    const ExecutionContext ctx;
    const std::shared_ptr<WalletService> wallet;
    const WalletSigningIdentity selected;
    const VaultStateDomain domain;
    std::mutex mutex;
    std::condition_variable drained;
    std::map<std::thread::id,uint64_t> threads;
    uint64_t active{0};
    bool closed{false};
    WalletWithdrawalDispatchState(ExecutionContext c,std::shared_ptr<WalletService> w,
        WalletSigningIdentity s,VaultStateDomain d)
        :ctx(std::move(c)),wallet(std::move(w)),selected(std::move(s)),domain(d) {
        Require(wallet && ctx.daemon && selected.session && !selected.name.empty() &&
                ctx.walletName==selected.name && domain.network<=2 && Nonzero(domain.genesis));
    }
    struct Use {
        std::shared_ptr<WalletWithdrawalDispatchState> state;
        const std::thread::id thread{std::this_thread::get_id()};
        explicit Use(std::shared_ptr<WalletWithdrawalDispatchState> value):state(std::move(value)) {
            std::lock_guard lock(state->mutex);
            if(state->closed)throw std::runtime_error("vault wallet dispatch owner is closed");
            if(state->active==UINT64_MAX)throw std::runtime_error("vault wallet dispatch capacity exhausted");
            auto [it,inserted]=state->threads.try_emplace(thread,0);
            ++it->second;++state->active;
        }
        Use(const Use&)=delete;
        Use& operator=(const Use&)=delete;
        ~Use() {
            if(thread!=std::this_thread::get_id())std::terminate();
            std::lock_guard lock(state->mutex);
            const auto it=state->threads.find(thread);
            if(it==state->threads.end() || !it->second || !state->active)std::terminate();
            if(!--it->second)state->threads.erase(it);
            if(!--state->active)state->drained.notify_all();
        }
    };
    std::unique_ptr<Use> Acquire() {return std::make_unique<Use>(shared_from_this());}
    void Close() {
        std::unique_lock lock(mutex);
        if(threads.contains(std::this_thread::get_id()))
            throw std::logic_error("cannot close vault dispatch owner from its active callback");
        closed=true;
        drained.wait(lock,[this]{return active==0;});
    }
};
namespace {
class WalletWithdrawalDispatcher final : public VaultWithdrawalDispatcher {
public:
    WalletWithdrawalDispatcher(std::shared_ptr<WalletWithdrawalDispatchState> state,VaultIdentity vault)
        :state_(std::move(state)),vault_(vault) {
        const auto use=state_->Acquire();Require(Nonzero(vault_));
    }
private:
    void CheckDomain() const {
        Require(static_cast<uint8_t>(GetActiveChain())==state_->domain.network);
        const auto& text=Params().genesis_hash;
        Require(text.size()==64 && std::all_of(text.begin(),text.end(),[](unsigned char c){
            return (c>='0' && c<='9') || (c>='a' && c<='f') || (c>='A' && c<='F');}));
        uint256 genesis;Require(uint256::FromHex(text,genesis));
        Require(std::equal(state_->domain.genesis.begin(),state_->domain.genesis.end(),genesis.begin()));
    }
    void Authenticate(const WithdrawalRequest& request) const {
        CheckDomain();Require(state_->ctx.daemon->wallet==state_->wallet && request.payment_terms.has_value());
        ValidateWithdrawalPaymentTerms(*request.payment_terms);
        auto use=WalletService::AcquireWalletUse(state_->wallet);
        const auto selected=CaptureWalletSigningIdentity(use->Wallet(),state_->selected.name);
        Require(selected.name==state_->selected.name && selected.session==state_->selected.session);
        auto owner=VaultStateTransaction::OpenExisting(use->Wallet(),state_->selected.session,state_->domain,vault_);
        const auto& rows=owner->Current().state.withdrawals;
        const auto found=std::find_if(rows.begin(),rows.end(),[&](const auto& v){return v.request.request_id==request.request_id;});
        Require(found!=rows.end() && found->request==request &&
                (std::holds_alternative<WithdrawalSigning>(found->state) ||
                 std::holds_alternative<WithdrawalPaymentRetained>(found->state) ||
                 std::holds_alternative<WithdrawalPaymentConfirmed>(found->state)));
        owner->Commit();
        // owner and WalletUse are released before FindRetained or dispatch;
        // recovery-seed owners must never be nested between these operations.
    }
    PendingPaymentIntent Intent(const WithdrawalRequest& request) const {
        Require(request.payment_terms.has_value());ValidateWithdrawalPaymentTerms(*request.payment_terms);
        const auto& script=request.destination_script_pub_key;
        Require(script.size()==34 && script[0]==0x51 && script[1]==0x20);
        PendingPaymentIntent intent;
        intent.address=bech32::Encode(HrpForActiveNetworkRef(),1,
            std::vector<uint8_t>(script.begin()+2,script.end()),bech32::Encoding::BECH32M);
        Require(!intent.address.empty());intent.amount_una=request.amount;
        PendingPaymentRequest binding;binding.owner=vault_;binding.id=request.request_id;
        binding.fee_rate_hint=request.payment_terms->fee_rate_hint;
        binding.maximum_fee_una=request.payment_terms->maximum_fee_una;
        binding.audit_context=request.payment_terms->audit_context;
        intent.request=std::move(binding);return intent;
    }
    std::optional<WithdrawalPaymentRetained> Read(const WithdrawalRequest& request) const {
        Authenticate(request);const auto intent=Intent(request);
        auto use=WalletService::AcquireWalletUse(state_->wallet);
        const auto retained=FindRetainedWalletPayment(use->Wallet(),state_->selected,intent);
        if(!retained)return std::nullopt;
        Require(retained->intent==intent && retained->fee_una<=request.payment_terms->maximum_fee_una);
        Transaction tx;size_t consumed=0;
        Require(TransactionSerializer::Deserialize(tx,retained->signed_body,consumed) &&
                consumed==retained->signed_body.size() &&
                tx.Serialize(TxSerializationMode::WithWitness)==retained->signed_body &&
                tx.GetTxid().AsUint256().GetHex()==retained->txid && !tx.GetTxid().IsNull());
        std::optional<uint32_t> output;
        for(size_t i=0;i<tx.vout.size();++i) {
            const auto& out=tx.vout[i];
            if(!out.is_confidential && out.GetValue()==request.amount && out.scriptPubKey==request.destination_script_pub_key) {
                Require(!output && i<=UINT32_MAX);output=static_cast<uint32_t>(i);
            }
        }
        Require(output.has_value());
        WithdrawalPaymentRetained result;const auto txid=tx.GetTxid().AsUint256();
        std::copy(txid.begin(),txid.end(),result.txid.begin());result.vout=*output;
        SHA256(retained->signed_body.data(),retained->signed_body.size(),result.body_sha256.data());
        result.fee_una=retained->fee_una;Require(Nonzero(result.body_sha256));return result;
    }
    std::optional<WithdrawalPaymentRetained> Resolve(const WithdrawalRequest& request) override {
        const auto use=state_->Acquire();return Read(request);
    }
    std::optional<WithdrawalPaymentRetained> DispatchNew(const WithdrawalRequest& request) override {
        const auto use=state_->Acquire();
        if(auto retained=Read(request))return retained;
        const auto intent=Intent(request);din::Json params,recipients(Json::arrayValue),recipient,binding;
        recipient["address"]=intent.address;recipient["amount_una"]=din::Json::UInt64(intent.amount_una);recipients.append(recipient);
        binding["domain"]="vault_withdrawal";binding["owner"]=util::hex(std::vector<uint8_t>(vault_.begin(),vault_.end()));
        binding["id"]=util::hex(std::vector<uint8_t>(request.request_id.begin(),request.request_id.end()));
        binding["fee_rate_hint"]=din::Json::UInt64(intent.request->fee_rate_hint);
        binding["maximum_fee_una"]=din::Json::UInt64(intent.request->maximum_fee_una);
        binding["audit_context"]=intent.request->audit_context;params["recipients"]=recipients;params["request"]=binding;
        // A handler error, rejection or thrown callback may follow wallet
        // persistence. In every case resolve its authoritative retained body.
        // The JSON status is never promoted to a vault confirmation/ack.
        try {(void)DispatchBoundWalletRequest(state_->ctx,params,state_->wallet,state_->selected);}catch(...) {}
        return Read(request);
    }
    class CanonicalLease final : public VaultWithdrawalObservationLease {
    public:
        CanonicalLease(const std::shared_ptr<WalletWithdrawalDispatchState>& state,
                       std::shared_ptr<ChainstateService> source)
            :dispatch_use_(state->Acquire()),source_(std::move(source)),
             lifetime_(ChainstateService::AcquireWalletIndexUse(source_)),
             selected_(source_->AcquireBlockIngressActivationLock()) {}
        uint64_t Height() const noexcept override {return height;}
        const std::array<uint8_t,32>& Hash() const noexcept override {return hash;}
        const std::vector<VaultWithdrawalObservation>& Rows() const noexcept override {return rows;}
        uint64_t height{0};std::array<uint8_t,32> hash{};
        std::vector<VaultWithdrawalObservation> rows;
    private:
        std::unique_ptr<WalletWithdrawalDispatchState::Use> dispatch_use_;
        const std::shared_ptr<ChainstateService> source_;
        std::unique_ptr<ChainstateService::WalletIndexUse> lifetime_;
        std::unique_lock<AnnotatedRecursiveMutex> selected_;
    };
    std::unique_ptr<VaultWithdrawalObservationLease> CaptureCanonical(
        uint64_t height,const std::vector<VaultWithdrawalQuery>& queries) override {
        const auto source=state_->ctx.daemon->chainstate;
        Require(source && height<=UINT32_MAX);
        auto captured=std::make_unique<CanonicalLease>(state_,source);
        CheckDomain();const auto* chain=source->GetChainDB();Require(chain);
        const auto tip=chain->getTip();Require(tip.ok() && tip->height>=0 && uint64_t(tip->height)==height);
        const auto tip_hash=source->getCanonicalBlockHash(static_cast<uint32_t>(height));
        Require(tip_hash.ok() && *tip_hash==tip->hash);
        captured->height=height;std::copy(tip_hash->begin(),tip_hash->end(),captured->hash.begin());
        captured->rows.reserve(queries.size());
        const auto hash=[](const std::array<uint8_t,32>& bytes) {
            uint256 out;std::copy(bytes.begin(),bytes.end(),out.begin());return out;
        };
        for(const auto& query:queries) {
            // Under source -> wallet ordering, authenticate the PRESENT saved
            // request and full retained payment before any canonical claim.
            // Read releases every wallet/SQLite/key owner before returning.
            const auto payment=Read(query.request);Require(payment && *payment==query.retained);
            VaultWithdrawalObservation observation{query,std::nullopt,false};
            if(query.prior) {
                Require(Nonzero(query.prior->block_hash));
                if(query.prior->height>height)observation.prior_not_canonical=true;
                else {
                    const auto old=source->getCanonicalBlockHash(static_cast<uint32_t>(query.prior->height));
                    Require(old.ok());observation.prior_not_canonical=(*old!=hash(query.prior->block_hash));
                }
            }
            const auto txid=hash(query.retained.txid);
            const auto location=chain->getTxLocation(txid);
            if(location.ok()) {
                const auto at=chain->getBlockHeight(location->first);
                Require(at.ok() && *at>=0 && uint64_t(*at)<=height);
                const auto canonical=source->getCanonicalBlockHash(static_cast<uint32_t>(*at));
                Require(canonical.ok() && *canonical==location->first);
                const auto block=source->getBlockRpcSnapshot(*canonical);
                Require(block.ok() && block->height==uint32_t(*at) && block->header.GetHash()==*canonical);
                std::vector<TxId> ids;ids.reserve(block->transaction_ids.size());
                for(const auto& id:block->transaction_ids)ids.emplace_back(id);
                bool mutated=false;const auto root=consensus::ComputeTransactionMerkleRoot(ids,&mutated);
                Require(!mutated && root==block->header.merkle_root && location->second<ids.size() &&
                        ids[location->second].AsUint256()==txid);
                const auto body=source->getTransactionBody(txid);
                Require(body.ok() && !body->IsOrchard() && !body->Historical().IsCoinbase());
                const auto bytes=body->Serialize(TxSerializationMode::WithWitness);
                std::array<uint8_t,32> digest{};
                Require(SHA256(bytes.data(),bytes.size(),digest.data()) && digest==query.retained.body_sha256);
                const auto output=source->getCanonicalOutputInclusion(txid,query.retained.vout,uint32_t(*at));
                Require(output.ok() && output->block_hash==*canonical &&
                        output->MatchesTransparent(query.request.amount,query.request.destination_script_pub_key));
                AllocationInclusion inclusion;inclusion.height=uint64_t(*at);
                std::copy(canonical->begin(),canonical->end(),inclusion.block_hash.begin());
                Require(!query.prior || observation.prior_not_canonical || *query.prior==inclusion);
                observation.included=inclusion;
            } else Require(location.status()==Status::NotFound);
            captured->rows.push_back(std::move(observation));
        }
        // Selected ownership survives this return. The service releases this
        // lease only after its checked commit and live publication complete.
        return captured;
    }
    const std::shared_ptr<WalletWithdrawalDispatchState> state_;
    const VaultIdentity vault_;
};
}
WalletWithdrawalDispatchOwner::WalletWithdrawalDispatchOwner(ExecutionContext ctx,
    std::shared_ptr<WalletService> wallet,WalletSigningIdentity selected,VaultStateDomain domain)
    :state_(std::make_shared<WalletWithdrawalDispatchState>(std::move(ctx),std::move(wallet),std::move(selected),domain)) {}
WalletWithdrawalDispatchOwner::~WalletWithdrawalDispatchOwner() {
    try {state_->Close();}catch(...) {std::terminate();}
}
void WalletWithdrawalDispatchOwner::Close() {state_->Close();}
VaultWithdrawalDispatcherFactory WalletWithdrawalDispatchOwner::Factory() const {
    // Factory allocation/validation is gated too, without wallet or SQL work.
    const auto use=state_->Acquire();
    return [state=state_](const VaultIdentity& id) {
        return std::make_shared<WalletWithdrawalDispatcher>(state,id);
    };
}
} // namespace dinero::vault
