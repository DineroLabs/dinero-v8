#include "pool/wallet_payment_backend.h"
#include "pool/canonical_payment.h"
#include "daemon/daemon_context.h"
#include "daemon/services/chainstate_service.h"
#include "daemon/services/wallet_service.h"
#include "wallet/wallet_manager.h"
#include "wallet/wallet_transaction_signer.h"
#include "rpc/wallet_request_dispatch.h"
#include "consensus/chainparams.h"
#include "address/addr_codec.h"
#include "primitives/transaction.h"
#include "util/hex.h"
#include <sqlite3.h>
#include <openssl/sha.h>
#include <algorithm>
#include <set>
#include <stdexcept>
namespace dinero::pool {
namespace {
void RequirePoolWallet(bool ok) {if(!ok)throw std::runtime_error("pool funding wallet or retained body binding unavailable");}
bool NonzeroPoolWallet(const auto& v) {return std::any_of(v.begin(),v.end(),[](uint8_t b){return b!=0;});}
PoolPaymentWalletBinding CaptureBinding(WalletManager& manager,const WalletSigningIdentity& selected,const PoolPaymentFunding& funding) {
    RequirePoolWallet(!funding.wallet_name.empty() && selected.name==funding.wallet_name && selected.session);
    auto lease=manager.AcquireDatabaseLease();RequirePoolWallet(lease->WalletName()==selected.name && lease->Session()==selected.session);
    auto seed=lease->CopyRecoverySeed(selected.session);RequirePoolWallet(seed->Bytes().size()==64);
    auto* db=lease->Database();RequirePoolWallet(db && sqlite3_get_autocommit(db));
    sqlite3_stmt* raw=nullptr;const int prepared=sqlite3_prepare_v2(db,"SELECT runtime_delivery_id FROM wallet_meta WHERE id=1",-1,&raw,nullptr);
    std::unique_ptr<sqlite3_stmt,decltype(&sqlite3_finalize)> row(raw,sqlite3_finalize);RequirePoolWallet(prepared==SQLITE_OK);
    RequirePoolWallet(sqlite3_step(raw)==SQLITE_ROW && sqlite3_column_type(raw,0)==SQLITE_BLOB && sqlite3_column_bytes(raw,0)==32);
    const auto* bytes=static_cast<const uint8_t*>(sqlite3_column_blob(raw,0));RequirePoolWallet(bytes!=nullptr);
    PoolPaymentWalletBinding out;out.funding=funding;std::copy(bytes,bytes+32,out.wallet.begin());RequirePoolWallet(sqlite3_step(raw)==SQLITE_DONE && NonzeroPoolWallet(out.wallet));
    out.network=static_cast<uint8_t>(GetActiveChain());RequirePoolWallet(out.network<=2);
    const auto& text=Params().genesis_hash;RequirePoolWallet(text.size()==64 && std::all_of(text.begin(),text.end(),[](unsigned char c){return (c>='0' && c<='9') || (c>='a' && c<='f') || (c>='A' && c<='F');}));
    uint256 genesis;RequirePoolWallet(uint256::FromHex(text,genesis));std::copy(genesis.begin(),genesis.end(),out.genesis.begin());RequirePoolWallet(NonzeroPoolWallet(out.genesis));return out;
}
} // namespace
class WalletPoolDispatcher final : public PoolPaymentDispatcher {
public:
    WalletPoolDispatcher(DaemonContext& context,const PoolPaymentFunding& funding):ctx_(context) {
        wallet_=std::dynamic_pointer_cast<WalletService>(ctx_.wallet);RequirePoolWallet(wallet_!=nullptr);
        auto use=WalletService::AcquireWalletUse(wallet_);
        selected_=CaptureWalletSigningIdentity(use->Wallet(),funding.wallet_name);
        binding_=CaptureBinding(use->Wallet(),selected_,funding);
    }
private:
    const PoolPaymentWalletBinding& Binding() const override {return binding_;}
    PendingPaymentIntent Intent(const PoolPaymentAttempt& a) const {
        RequirePoolWallet(a.binding==binding_ && NonzeroPoolWallet(a.id) && !a.members.empty() && a.members.size()<=256);
        PendingPaymentIntent intent;intent.address=a.address;intent.amount_una=a.amount;
        PendingPaymentRequest request;request.domain=PendingPaymentRequestDomain::PoolPayout;request.owner=a.members.front().origin;request.id=a.id;
        request.fee_rate_hint=binding_.funding.fee_rate_hint;request.maximum_fee_una=binding_.funding.maximum_fee_una;
        uint64_t total=0;std::set<uint64_t> ids;
        for(const auto& m:a.members) {
            RequirePoolWallet(m.payout_id && ids.insert(m.payout_id).second && m.amount && m.amount<=a.amount-total && NonzeroPoolWallet(m.origin) && (request.pool_origins.empty() || request.pool_origins.back()<m.origin));
            total+=m.amount;request.pool_origins.push_back(m.origin);
        }
        RequirePoolWallet(total==a.amount && total);intent.request=std::move(request);return intent;
    }
    std::optional<PoolPaymentRetained> Read(const PoolPaymentAttempt& a) {
        const auto intent=Intent(a);RequirePoolWallet(ctx_.wallet==wallet_);
        {
            auto use=WalletService::AcquireWalletUse(wallet_);
            RequirePoolWallet(CaptureBinding(use->Wallet(),selected_,binding_.funding)==binding_);
        }
        // Identity/seed owner is released before acquiring the authenticated
        // pending-payment owner. Signing and admission never run under it.
        auto use=WalletService::AcquireWalletUse(wallet_);
        auto retained=FindRetainedWalletPayment(use->Wallet(),selected_,intent);if(!retained)return std::nullopt;
        RequirePoolWallet(retained->intent==intent && retained->fee_una<=binding_.funding.maximum_fee_una);
        Transaction tx;size_t consumed=0;RequirePoolWallet(TransactionSerializer::Deserialize(tx,retained->signed_body,consumed) && consumed==retained->signed_body.size() && tx.Serialize(TxSerializationMode::WithWitness)==retained->signed_body && tx.GetTxid().AsUint256().GetHex()==retained->txid && !tx.GetTxid().IsNull());
        const auto script=DecodeWitnessAddress(intent.address,HrpForActiveNetworkRef());RequirePoolWallet(script.is_valid && !script.script_pubkey.empty());
        std::optional<uint32_t> output;
        for(size_t i=0;i<tx.vout.size();++i)if(!tx.vout[i].is_confidential && tx.vout[i].GetValue()==a.amount && tx.vout[i].scriptPubKey==script.script_pubkey) {
            RequirePoolWallet(!output && i<=UINT32_MAX);output=static_cast<uint32_t>(i);
        }
        RequirePoolWallet(output.has_value());PoolPaymentRetained out;const auto id=tx.GetTxid().AsUint256();std::copy(id.begin(),id.end(),out.txid.begin());
        RequirePoolWallet(SHA256(retained->signed_body.data(),retained->signed_body.size(),out.body_sha256.data())!=nullptr && NonzeroPoolWallet(out.body_sha256));
        out.fee_una=retained->fee_una;out.vout=*output;return out;
    }
    std::optional<PoolPaymentRetained> Resolve(const PoolPaymentAttempt& a) override {return Read(a);}
    bool Reconcile(PoolDB& db,const PoolPaymentAttempt& a) override {
        RequirePoolWallet(a.retained && Read(a)==a.retained);
        // All wallet/seed owners are released before selected chain -> pool DB.
        try {return PoolPaymentCanonicalOwner::Reconcile(ctx_.chainstate,db,a);}
        catch(const ChainstateService::WalletIndexUnavailable&) {
            // Retention already succeeded. A source that has not started or
            // has stopped defers settlement; it is not proof of absence, PAID
            // status or permission to dispatch another body. Canonical read,
            // binding and SQLite failures still propagate to the caller.
            return false;
        }
    }
    std::optional<PoolPaymentRetained> DispatchNew(const PoolPaymentAttempt& a) override {
        if(auto retained=Read(a))return retained;
        const auto intent=Intent(a);din::Json params,recipients(Json::arrayValue),recipient,request;
        recipient["address"]=intent.address;recipient["amount_una"]=din::Json::UInt64(intent.amount_una);recipients.append(recipient);
        const auto& b=*intent.request;request["domain"]="pool_payout";request["owner"]=util::hex(std::vector<uint8_t>(b.owner.begin(),b.owner.end()));
        request["id"]=util::hex(std::vector<uint8_t>(b.id.begin(),b.id.end()));request["fee_rate_hint"]=din::Json::UInt64(b.fee_rate_hint);
        request["maximum_fee_una"]=din::Json::UInt64(b.maximum_fee_una);request["audit_context"]=b.audit_context;request["pool_origins"]=din::Json(Json::arrayValue);
        for(const auto& origin:b.pool_origins)request["pool_origins"].append(util::hex(std::vector<uint8_t>(origin.begin(),origin.end())));
        params["request"]=request;params["recipients"]=recipients;ExecutionContext ctx;ctx.daemon=&ctx_;ctx.walletName=selected_.name;
        try {(void)DispatchBoundWalletRequest(ctx,params,wallet_,selected_);}catch(...) {}
        return Read(a); // JSON status is never payment settlement or permission to retry
    }
    DaemonContext& ctx_;std::shared_ptr<WalletService> wallet_;WalletSigningIdentity selected_;PoolPaymentWalletBinding binding_;
};
namespace {
class WalletPoolBackend final : public PoolPaymentBackend {
public: explicit WalletPoolBackend(DaemonContext& ctx):ctx_(ctx) {}
private:
    std::unique_ptr<PoolPaymentDispatcher> Bind(const PoolPaymentFunding& f) override {return std::make_unique<WalletPoolDispatcher>(ctx_,f);}
    DaemonContext& ctx_;
};
}
std::unique_ptr<PoolPaymentBackend> MakeWalletPoolPaymentBackend(DaemonContext& ctx) {return std::make_unique<WalletPoolBackend>(ctx);}
} // namespace dinero::pool
