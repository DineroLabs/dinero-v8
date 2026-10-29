#include "vault/state_store.h"
#include "wallet/wallet_manager.h"
#include "daemon/services/wallet_service.h"
#include "primitives/transaction.h"
#include "util/hex.h"
#include "external/bech32/bech32.hpp"
#include <map>
#include <unordered_map>
#include <sqlite3.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <algorithm>
#include <climits>
#include <limits>
#include <stdexcept>
#include <type_traits>

namespace dinero::vault {
namespace {
constexpr size_t MaxState=64U*1024U*1024U;
void Check(bool v) {if(!v)throw std::runtime_error("vault durable owner or state unavailable");}
bool Nonzero(std::span<const uint8_t> v) {return std::any_of(v.begin(),v.end(),[](uint8_t b){return b!=0;});}
struct Sensitive {
    std::vector<uint8_t> bytes;
    ~Sensitive(){OPENSSL_cleanse(bytes.data(),bytes.size());}
};
struct Key {
    std::array<uint8_t,32> bytes{};
    ~Key(){OPENSSL_cleanse(bytes.data(),bytes.size());}
};
struct Statement {
    sqlite3* db;
    std::unique_ptr<sqlite3_stmt,decltype(&sqlite3_finalize)> p{nullptr,sqlite3_finalize};
    Statement(sqlite3* d,const char* sql):db(d) {sqlite3_stmt* raw=nullptr;int rc=sqlite3_prepare_v2(db,sql,-1,&raw,nullptr);p.reset(raw);Check(rc==SQLITE_OK);}
    void Blob(int i,std::span<const uint8_t> v) {Check(v.size()<=INT_MAX);Check(sqlite3_bind_blob(p.get(),i,v.data(),static_cast<int>(v.size()),SQLITE_TRANSIENT)==SQLITE_OK);}
    void Int(int i,uint64_t v) {Check(v<=INT64_MAX);Check(sqlite3_bind_int64(p.get(),i,static_cast<int64_t>(v))==SQLITE_OK);}
    void Done(bool changed=false) {Check(sqlite3_step(p.get())==SQLITE_DONE);if(changed)Check(sqlite3_changes(db)==1);}
};
void Exec(sqlite3* db,const char* sql) {Check(sqlite3_exec(db,sql,nullptr,nullptr,nullptr)==SQLITE_OK);}
std::vector<uint8_t> BlobColumn(sqlite3_stmt* p,int column,size_t min,size_t max) {
    Check(sqlite3_column_type(p,column)==SQLITE_BLOB);const int n=sqlite3_column_bytes(p,column);Check(n>=0 && static_cast<size_t>(n)>=min && static_cast<size_t>(n)<=max);
    const auto* v=static_cast<const uint8_t*>(sqlite3_column_blob(p,column));Check(v || n==0);if(!n)return {};return {v,v+n};
}
std::array<uint8_t,32> Hash(std::span<const uint8_t> v) {std::array<uint8_t,32> out{};Check(SHA256(v.data(),v.size(),out.data())!=nullptr);return out;}
void Append(std::vector<uint8_t>& out,std::span<const uint8_t> v) {out.insert(out.end(),v.begin(),v.end());}
void U64(std::vector<uint8_t>& out,uint64_t v) {for(unsigned i=0;i<8;++i)out.push_back(static_cast<uint8_t>(v>>(8*i)));}
std::vector<uint8_t> Associated(const VaultStateDomain& domain,const std::array<uint8_t,32>& wallet,const VaultIdentity& vault,uint64_t revision,const std::array<uint8_t,32>& previous) {
    const std::string label="Dinero vault snapshot owner v1";std::vector<uint8_t> out(label.begin(),label.end());out.push_back(domain.network);Append(out,domain.genesis);Append(out,wallet);Append(out,vault);U64(out,revision);Append(out,previous);return out;
}
std::vector<uint8_t> Seal(const Key& key,std::span<const uint8_t> aad,std::span<const uint8_t> plain) {
    Check(plain.size()<=MaxState && aad.size()<=INT_MAX);std::vector<uint8_t> sealed(12+plain.size()+16);Check(RAND_bytes(sealed.data(),12)==1);
    std::unique_ptr<EVP_CIPHER_CTX,decltype(&EVP_CIPHER_CTX_free)> ctx(EVP_CIPHER_CTX_new(),EVP_CIPHER_CTX_free);Check(bool(ctx));
    Check(EVP_EncryptInit_ex(ctx.get(),EVP_aes_256_gcm(),nullptr,key.bytes.data(),sealed.data())==1);
    int n=0,total=0;Check(EVP_EncryptUpdate(ctx.get(),nullptr,&n,aad.data(),static_cast<int>(aad.size()))==1);
    Check(EVP_EncryptUpdate(ctx.get(),sealed.data()+12,&n,plain.data(),static_cast<int>(plain.size()))==1);total=n;
    Check(EVP_EncryptFinal_ex(ctx.get(),sealed.data()+12+total,&n)==1);total+=n;Check(static_cast<size_t>(total)==plain.size());
    Check(EVP_CIPHER_CTX_ctrl(ctx.get(),EVP_CTRL_GCM_GET_TAG,16,sealed.data()+12+total)==1);return sealed;
}
void Unseal(const Key& key,std::span<const uint8_t> aad,std::span<const uint8_t> sealed,Sensitive& out) {
    // Every state has a nonempty versioned frame. Refuse an empty cipher
    // before forming an output pointer for the GCM final operation.
    Check(sealed.size()>28 && sealed.size()<=MaxState+28 && aad.size()<=INT_MAX);const size_t nplain=sealed.size()-28;out.bytes.resize(nplain);
    std::unique_ptr<EVP_CIPHER_CTX,decltype(&EVP_CIPHER_CTX_free)> ctx(EVP_CIPHER_CTX_new(),EVP_CIPHER_CTX_free);Check(bool(ctx));
    Check(EVP_DecryptInit_ex(ctx.get(),EVP_aes_256_gcm(),nullptr,key.bytes.data(),sealed.data())==1);
    int n=0,total=0;Check(EVP_DecryptUpdate(ctx.get(),nullptr,&n,aad.data(),static_cast<int>(aad.size()))==1);
    Check(EVP_DecryptUpdate(ctx.get(),out.bytes.data(),&n,sealed.data()+12,static_cast<int>(nplain))==1);total=n;
    Check(EVP_CIPHER_CTX_ctrl(ctx.get(),EVP_CTRL_GCM_SET_TAG,16,const_cast<uint8_t*>(sealed.data()+12+nplain))==1);
    Check(EVP_DecryptFinal_ex(ctx.get(),out.bytes.data()+total,&n)==1);total+=n;Check(static_cast<size_t>(total)==nplain);
}
}

struct VaultStateTransaction::Impl {
    std::unique_ptr<WalletManager::DatabaseLease> lease;
    std::unique_ptr<WalletManager::RecoverySeed> seed;
    sqlite3* db=nullptr;
    VaultStateDomain domain;
    std::array<uint8_t,32> wallet{};
    Key key;
    StoredVaultState current;
    std::optional<StoredVaultState> next;
    std::vector<uint8_t> current_sealed;
    bool owns=false,committed=false,created=false;

    Impl(WalletManager& manager,uint64_t session,const VaultStateDomain& d)
        :lease(manager.AcquireDatabaseLease()),domain(d) {
        Check(session!=0 && lease->Session()==session && lease->Database());db=lease->Database();
        Check(domain.network<=2 && Nonzero(domain.genesis));
        seed=lease->CopyRecoverySeed(session);Check(seed->Bytes().size()==64);
        Check(sqlite3_get_autocommit(db));Exec(db,"PRAGMA synchronous=FULL");
        {Statement p(db,"PRAGMA synchronous");Check(sqlite3_step(p.p.get())==SQLITE_ROW);Check(sqlite3_column_type(p.p.get(),0)==SQLITE_INTEGER && sqlite3_column_int64(p.p.get(),0)==2);p.Done();}
        Exec(db,"BEGIN IMMEDIATE");owns=true;
        try {
            // Existing wallet identity is part of this same snapshot. Missing
            // metadata must not generate a replacement identity during reads
            // or turn a historical vault into a newly owned empty vault.
            Statement q(db,"SELECT runtime_delivery_id FROM wallet_meta WHERE id=1");
            Check(sqlite3_step(q.p.get())==SQLITE_ROW);
            const auto id=BlobColumn(q.p.get(),0,32,32);q.Done();
            std::copy(id.begin(),id.end(),wallet.begin());Check(Nonzero(wallet));
        } catch(...) {
            if(!sqlite3_get_autocommit(db) && sqlite3_exec(db,"ROLLBACK",nullptr,nullptr,nullptr)!=SQLITE_OK &&
               !sqlite3_get_autocommit(db))std::terminate();
            owns=false;throw;
        }
    }
    ~Impl() {
        if(owns && !sqlite3_get_autocommit(db) && sqlite3_exec(db,"ROLLBACK",nullptr,nullptr,nullptr)!=SQLITE_OK && !sqlite3_get_autocommit(db))std::terminate();
    }
    void SelectIdentity(const VaultIdentity& identity) {
        Check(Nonzero(identity));current=StoredVaultState{};current.identity=identity;
        current_sealed.clear();OPENSSL_cleanse(key.bytes.data(),key.bytes.size());
        Sensitive material;const std::string label="Dinero vault snapshot key v1";
        material.bytes.assign(label.begin(),label.end());Append(material.bytes,seed->Bytes());
        material.bytes.push_back(domain.network);Append(material.bytes,domain.genesis);
        Append(material.bytes,wallet);Append(material.bytes,identity);key.bytes=Hash(material.bytes);
    }
    void ValidateRetainedPayments(const VaultStateSnapshot& state) const {
        const bool needed=std::any_of(state.withdrawals.begin(),state.withdrawals.end(),[](const auto& row) {
            return std::holds_alternative<WithdrawalPaymentRetained>(row.state) ||
                   std::holds_alternative<WithdrawalPaymentConfirmed>(row.state);
        });
        if(!needed)return;
        // The vault row, full authenticated wallet payment envelope and bound
        // history rows are read in this same FULL transaction and seed owner.
        // No nested seed copy/transaction, external callback or chain read.
        const auto payments=lease->ReadPendingPaymentsInTransaction(*seed);
        std::map<WithdrawalId,const PendingPayment*> by_request;
        for(const auto& payment:payments) {
            // The full envelope is authenticated above. Only vault-domain
            // requests can bind this vault's retained withdrawals; another
            // domain may legitimately use the same owner and request bytes.
            if(!payment.intent.request ||
               payment.intent.request->domain!=PendingPaymentRequestDomain::VaultWithdrawal ||
               payment.intent.request->owner!=current.identity)continue;
            Check(by_request.emplace(payment.intent.request->id,&payment).second);
        }
        static constexpr const char* hrps[]={"din","tdin","rdin"};
        Check(domain.network<3);
        for(const auto& row:state.withdrawals) {
            const auto* retained=std::get_if<WithdrawalPaymentRetained>(&row.state);
            if(const auto* confirmed=std::get_if<WithdrawalPaymentConfirmed>(&row.state))retained=&confirmed->payment;
            if(!retained)continue;
            const auto& request=row.request;Check(request.payment_terms.has_value());
            ValidateWithdrawalPaymentTerms(*request.payment_terms);
            const auto found=by_request.find(request.request_id);Check(found!=by_request.end());
            const auto& payment=*found->second;const auto& binding=*payment.intent.request;
            const auto& script=request.destination_script_pub_key;
            Check(script.size()==34 && script[0]==0x51 && script[1]==0x20);
            const auto address=bech32::Encode(hrps[domain.network],1,
                std::vector<uint8_t>(script.begin()+2,script.end()),bech32::Encoding::BECH32M);
            Check(!address.empty() && payment.intent.address==address &&
                  payment.intent.amount_una==request.amount && payment.intent.label.empty() &&
                  payment.intent.additional_recipients.empty() &&
                  binding.fee_rate_hint==request.payment_terms->fee_rate_hint &&
                  binding.maximum_fee_una==request.payment_terms->maximum_fee_una &&
                  binding.audit_context==request.payment_terms->audit_context &&
                  payment.fee_una==retained->fee_una && payment.fee_una<=binding.maximum_fee_una);
            Transaction tx;size_t consumed=0;
            Check(TransactionSerializer::Deserialize(tx,payment.signed_body,consumed) &&
                  consumed==payment.signed_body.size() && tx.Serialize(TxSerializationMode::WithWitness)==payment.signed_body);
            const auto txid=tx.GetTxid().AsUint256();
            Check(std::equal(retained->txid.begin(),retained->txid.end(),txid.begin()) &&
                  Hash(payment.signed_body)==retained->body_sha256 && retained->vout<tx.vout.size());
            size_t matching=0;
            for(size_t i=0;i<tx.vout.size();++i) {
                const auto& output=tx.vout[i];
                if(!output.is_confidential && output.GetValue()==request.amount && output.scriptPubKey==script) {
                    Check(i==retained->vout);++matching;
                }
            }
            Check(matching==1);
        }
    }
    void Read() {
        Statement q(db,"SELECT revision,predecessor,sealed FROM wallet_vault_states WHERE vault_id=?");q.Blob(1,current.identity);
        Check(sqlite3_step(q.p.get())==SQLITE_ROW && sqlite3_column_type(q.p.get(),0)==SQLITE_INTEGER);const auto rev=sqlite3_column_int64(q.p.get(),0);Check(rev>=0);
        auto previous=BlobColumn(q.p.get(),1,32,32);std::copy(previous.begin(),previous.end(),current.predecessor.begin());Check((rev==0)==!Nonzero(current.predecessor));
        current_sealed=BlobColumn(q.p.get(),2,28,MaxState+28);q.Done();
        auto aad=Associated(domain,wallet,current.identity,static_cast<uint64_t>(rev),current.predecessor);Sensitive plain;Unseal(key,aad,current_sealed,plain);
        current.state=DecodeVaultState(plain.bytes);Check(current.state.revision==static_cast<uint64_t>(rev));
        (void)ReplayVaultStateLedger(current.state);ValidateRetainedPayments(current.state);current.digest=Hash(current_sealed);
    }
};
VaultStateTransaction::VaultStateTransaction(std::unique_ptr<Impl> p):impl_(std::move(p)){}
VaultStateTransaction::~VaultStateTransaction()=default;
std::unique_ptr<VaultStateTransaction> VaultStateTransaction::CreateNew(WalletManager& wallet,uint64_t session,const VaultStateDomain& domain,const VaultServiceConfig& config) {
    return CreateNewImpl(wallet,session,domain,config,false);
}
std::unique_ptr<VaultStateTransaction> VaultStateTransaction::CreateNewOwned(WalletManager& wallet,uint64_t session,const VaultStateDomain& domain,const VaultServiceConfig& config) {
    return CreateNewImpl(wallet,session,domain,config,true);
}
std::unique_ptr<VaultStateTransaction> VaultStateTransaction::CreateNewImpl(WalletManager& wallet,uint64_t session,const VaultStateDomain& domain,const VaultServiceConfig& config,bool require_operator) {
    VaultIdentity identity{};Check(RAND_bytes(identity.data(),static_cast<int>(identity.size()))==1 && Nonzero(identity));
    auto p=std::make_unique<Impl>(wallet,session,domain);p->SelectIdentity(identity);
    p->current.state.config=config;p->created=true;
    if(require_operator) {
        Check(config.operator_binding.has_value());ValidateVaultOperatorBinding(*config.operator_binding);
        auto key=p->lease->ResolveSigningKeyInTransaction(util::hex(config.operator_binding->script_pub_key),*p->seed);
        Check(key && key->secret.size()==32 && key->script==config.operator_binding->script_pub_key &&
              (key->policy==SigningKeyPolicy::TaprootCanonical || key->policy==SigningKeyPolicy::TaprootHistoricalImport));
    }
    (void)ReplayVaultStateLedger(p->current.state);
    Exec(p->db,"CREATE TABLE IF NOT EXISTS wallet_vault_states(vault_id BLOB PRIMARY KEY NOT NULL,revision INTEGER NOT NULL,predecessor BLOB NOT NULL,sealed BLOB NOT NULL)");
    Sensitive plain;plain.bytes=EncodeVaultState(p->current.state);auto aad=Associated(domain,p->wallet,identity,0,p->current.predecessor);p->current_sealed=Seal(p->key,aad,plain.bytes);p->current.digest=Hash(p->current_sealed);
    Statement insert(p->db,"INSERT INTO wallet_vault_states(vault_id,revision,predecessor,sealed) VALUES(?,0,?,?)");insert.Blob(1,identity);insert.Blob(2,p->current.predecessor);insert.Blob(3,p->current_sealed);insert.Done(true);
    return std::unique_ptr<VaultStateTransaction>(new VaultStateTransaction(std::move(p)));
}
std::unique_ptr<VaultStateTransaction> VaultStateTransaction::OpenExisting(WalletManager& wallet,uint64_t session,const VaultStateDomain& domain,const VaultIdentity& identity) {
    auto p=std::make_unique<Impl>(wallet,session,domain);p->SelectIdentity(identity);p->Read();return std::unique_ptr<VaultStateTransaction>(new VaultStateTransaction(std::move(p)));
}
std::vector<VaultStateSummary> VaultStateTransaction::ListExisting(
    WalletManager& wallet,uint64_t session,const VaultStateDomain& domain) {
    auto p=std::make_unique<Impl>(wallet,session,domain);
    std::vector<VaultIdentity> identities;size_t bytes=0;
    {
        Statement q(p->db,"SELECT vault_id,length(sealed) FROM wallet_vault_states ORDER BY vault_id");
        int rc;
        while((rc=sqlite3_step(q.p.get()))==SQLITE_ROW) {
            Check(identities.size()<4096);auto raw=BlobColumn(q.p.get(),0,32,32);
            VaultIdentity id{};std::copy(raw.begin(),raw.end(),id.begin());Check(Nonzero(id));
            Check(identities.empty() || identities.back()<id);
            Check(sqlite3_column_type(q.p.get(),1)==SQLITE_INTEGER);
            const auto length=sqlite3_column_int64(q.p.get(),1);
            Check(length>28 && static_cast<uint64_t>(length)<=MaxState+28 &&
                  static_cast<uint64_t>(length)<=MaxState-bytes);
            bytes+=static_cast<size_t>(length);identities.push_back(id);
        }
        Check(rc==SQLITE_DONE);
    }
    std::vector<VaultStateSummary> result;result.reserve(identities.size());
    for(const auto& id:identities) {
        p->SelectIdentity(id);p->Read();
        result.push_back({id,p->current.state.revision,p->current.state.config.operator_binding,p->current.state.config.creation_anchor});
    }
    // No prefix is returned for a late row failure or unsuccessful commit.
    Exec(p->db,"COMMIT");p->owns=false;p->committed=true;return result;
}
const StoredVaultState& VaultStateTransaction::Current() const noexcept {return impl_->current;}
void VaultStateTransaction::Stage(const VaultStateSnapshot& successor) {
    auto& p=*impl_;Check(!p.created && !p.committed && !p.next && p.owns && !sqlite3_get_autocommit(p.db));
    Check(p.current.state.revision<INT64_MAX && successor.revision==p.current.state.revision+1);
    // Ordinary state updates cannot add, remove or relabel routing authority.
    // In particular an older unbound snapshot is not permission to infer it.
    Check(successor.config.operator_binding==p.current.state.config.operator_binding);
    // A historical owner cannot acquire or move a creation boundary through
    // an ordinary successor, including when it is otherwise still empty.
    Check(successor.config.creation_anchor==p.current.state.config.creation_anchor);
    const auto allocated=[](const VaultStateSnapshot& state) {
        return std::any_of(state.entries.begin(),state.entries.end(),IsCreditAllocationEntry);
    };
    if(allocated(p.current.state) || allocated(successor)) {
        // Attribution is append-only. A newly authenticated successor cannot
        // erase or relabel the prefix that established exact source ownership.
        const auto& before=p.current.state;
        Check(successor.entries.size()>=before.entries.size() &&
              std::equal(before.entries.begin(),before.entries.end(),successor.entries.begin()));
        std::map<WithdrawalId,const WithdrawalRequest*> requests;
        for(const auto& row:successor.withdrawals)Check(requests.emplace(row.request.request_id,&row.request).second);
        for(const auto& row:before.withdrawals) {
            const auto found=requests.find(row.request.request_id);
            Check(found!=requests.end() && *found->second==row.request);
        }
        std::unordered_map<OutpointId,const TrackedDeposit*> deposits;
        for(const auto& row:successor.deposits)Check(deposits.emplace(row.deposit.outpoint,&row.deposit).second);
        for(const auto& row:before.deposits) {
            const auto found=deposits.find(row.deposit.outpoint);Check(found!=deposits.end());
            const auto& next=*found->second;
            Check(next.account==row.deposit.account && next.amount==row.deposit.amount &&
                  next.deposit_height==row.deposit.deposit_height);
        }
    }
    (void)ReplayVaultStateLedger(successor);p.ValidateRetainedPayments(successor);
    StoredVaultState prepared;prepared.identity=p.current.identity;prepared.predecessor=p.current.digest;prepared.state=successor;
    Sensitive plain;plain.bytes=EncodeVaultState(successor);auto aad=Associated(p.domain,p.wallet,prepared.identity,successor.revision,prepared.predecessor);auto sealed=Seal(p.key,aad,plain.bytes);prepared.digest=Hash(sealed);
    Statement update(p.db,"UPDATE wallet_vault_states SET revision=?,predecessor=?,sealed=? WHERE vault_id=? AND revision=? AND sealed=?");
    update.Int(1,successor.revision);update.Blob(2,prepared.predecessor);update.Blob(3,sealed);update.Blob(4,prepared.identity);update.Int(5,p.current.state.revision);update.Blob(6,p.current_sealed);update.Done(true);
    static_assert(std::is_nothrow_move_constructible_v<StoredVaultState>);
    p.next.emplace(std::move(prepared));
}
void VaultStateTransaction::Commit() {
    auto& p=*impl_;Check(p.owns && !p.committed && !sqlite3_get_autocommit(p.db));Exec(p.db,"COMMIT");p.owns=false;p.committed=true;
}
const StoredVaultState& VaultStateTransaction::Committed() const {Check(impl_->committed);return impl_->next?*impl_->next:impl_->current;}

namespace {
class WalletStateWrite final : public VaultStateWrite {
public:
    WalletStateWrite(std::shared_ptr<WalletService> wallet,uint64_t session,
                     const VaultStateDomain& domain,const VaultIdentity& identity)
        :use_(WalletService::AcquireWalletUse(std::move(wallet))),
         transaction_(VaultStateTransaction::OpenExisting(use_->Wallet(),session,domain,identity)) {}
    const VaultStateSnapshot& Base() const noexcept override {return transaction_->Current().state;}
    void Commit(const VaultStateSnapshot& successor) override {
        transaction_->Stage(successor);
        transaction_->Commit();
    }
private:
    // Reverse destruction releases the SQLite/seed owner before WalletUse.
    std::unique_ptr<WalletService::WalletUse> use_;
    std::unique_ptr<VaultStateTransaction> transaction_;
};
}
WalletVaultStateOwner::WalletVaultStateOwner(std::shared_ptr<WalletService> wallet,uint64_t session,
    const VaultStateDomain& domain,const VaultIdentity& identity)
    :wallet_(std::move(wallet)),session_(session),domain_(domain),identity_(identity) {
    Check(bool(wallet_) && session_!=0 && Nonzero(identity_));
}
std::unique_ptr<VaultStateWrite> WalletVaultStateOwner::Begin() const {
    return std::make_unique<WalletStateWrite>(wallet_,session_,domain_,identity_);
}
BoundVaultService WalletVaultStateOwner::CreateNewService(
    std::shared_ptr<WalletService> wallet,uint64_t session,const VaultStateDomain& domain,
    const VaultServiceConfig& config,std::unique_ptr<SigningBackend> backend,
    VaultService::BlockHashAtHeightFn hash_at,VaultService::TxIncludedAtFn included,
    VaultTipSnapshotFn capture_tip,VaultWithdrawalDispatcherFactory dispatcher_factory) {
    auto use=WalletService::AcquireWalletUse(wallet);
    auto transaction=VaultStateTransaction::CreateNew(use->Wallet(),session,domain,config);
    const auto identity=transaction->Current().identity;
    auto owner=std::shared_ptr<WalletVaultStateOwner>(new WalletVaultStateOwner(wallet,session,domain,identity));
    BoundVaultService result{identity,VaultService::RestorePrepared(transaction->Current().state,
        std::move(owner),std::move(backend),std::move(hash_at),std::move(included),std::move(capture_tip),
        dispatcher_factory?dispatcher_factory(identity):nullptr)};
    transaction->Commit();
    return result;
}
BoundVaultService WalletVaultStateOwner::OpenExistingService(
    std::shared_ptr<WalletService> wallet,uint64_t session,const VaultStateDomain& domain,
    const VaultIdentity& identity,std::unique_ptr<SigningBackend> backend,
    VaultService::BlockHashAtHeightFn hash_at,VaultService::TxIncludedAtFn included,
    VaultTipSnapshotFn capture_tip,VaultWithdrawalDispatcherFactory dispatcher_factory) {
    auto use=WalletService::AcquireWalletUse(wallet);
    auto transaction=VaultStateTransaction::OpenExisting(use->Wallet(),session,domain,identity);
    auto owner=std::shared_ptr<WalletVaultStateOwner>(new WalletVaultStateOwner(wallet,session,domain,identity));
    BoundVaultService result{identity,VaultService::RestorePrepared(transaction->Current().state,
        std::move(owner),std::move(backend),std::move(hash_at),std::move(included),std::move(capture_tip),
        dispatcher_factory?dispatcher_factory(identity):nullptr)};
    transaction->Commit();
    return result;
}
} // namespace dinero::vault
