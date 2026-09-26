#include "wallet/orchard_account_delivery.h"
#include "wallet/runtime_account_replay.h"
#include "wallet/wallet_manager.h"
#include <sqlite3.h>
#include <openssl/crypto.h>
#include <stdexcept>
namespace dinero::wallet {
namespace {
void Check(bool v) { if(!v) throw std::runtime_error("Orchard account delivery ownership or state mismatch"); }
void Exec(sqlite3* db,const char* sql) { Check(sqlite3_exec(db,sql,nullptr,nullptr,nullptr)==SQLITE_OK); }
struct Transaction {
    sqlite3* db;bool complete=false;
    explicit Transaction(sqlite3* p):db(p){Check(db&&sqlite3_get_autocommit(db));Exec(db,"BEGIN IMMEDIATE");}
    ~Transaction(){if(!complete&&sqlite3_exec(db,"ROLLBACK",nullptr,nullptr,nullptr)!=SQLITE_OK&&!sqlite3_get_autocommit(db))std::terminate();}
    void Commit(){Exec(db,"COMMIT");complete=true;}
};
orchard::WalletStorageIdentity Identity(const std::string& id,const OrchardAccountDelivery::Profile& p){
    Check(id.size()==71&&id.substr(0,7)=="DNWI01:"&&p.domain.network_code<=2&&p.account<0x80000000&&p.activation>0);
    orchard::Hash hash{};
    const auto digit=[](char c)->uint8_t {if(c>='0'&&c<='9')return c-'0';if(c>='a'&&c<='f')return c-'a'+10;throw std::runtime_error("Orchard account wallet identity malformed");};
    for(size_t i=0;i<hash.size();++i)hash[i]=(digit(id[7+2*i])<<4)|digit(id[8+2*i]);
    return {static_cast<orchard::WalletNetwork>(p.domain.network_code),p.domain.genesis_wire,hash,p.account};
}
struct Owner {
    struct ViewingKey {
        orchard::FullViewingKeyBytes bytes;
        explicit ViewingKey(const orchard::WalletKeys& keys):bytes(keys.ExportFullViewingKey()){}
        ~ViewingKey(){OPENSSL_cleanse(bytes.data(),bytes.size());}
    };
    std::unique_ptr<WalletManager::DatabaseLease> lease;
    std::unique_ptr<WalletManager::RecoverySeed> seed;
    orchard::WalletStorageIdentity identity;
    orchard::WalletKeys keys;
    ViewingKey fvk;
    orchard::WalletSnapshotStore store;
    Owner(WalletManager& wallet,uint64_t session,const OrchardAccountDelivery::Profile& p)
        :lease(wallet.AcquireDatabaseLease()),seed(),identity(Bind(session,p)),
         keys(orchard::WalletKeys::FromSeed(seed->Bytes(),p.account)),fvk(keys),
         store(lease->Database(),identity,seed->Bytes()){}
    orchard::WalletStorageIdentity Bind(uint64_t session,const OrchardAccountDelivery::Profile& p){
        Check(lease->Session()==session&&lease->Database());
        Check(p.domain.network_code<=2&&p.account<0x80000000&&p.activation>0);
        // Refuse missing/locked keys before identity initialization can write.
        seed=lease->CopyRecoverySeed(session);
        return Identity(lease->EnsureDeliveryIdentity(),p);
    }
    OrchardAccountDelivery::Applied Restore(const OrchardAccountDelivery::Profile& p,
            const OrchardAccountDelivery::RestorePoint& point){
        auto saved=store.Read();Check(saved.has_value());
        auto account=OrchardAccountState::Restore(saved->state,p.domain,fvk.bytes,p.activation,point.checkpoint,point.lookups);
        Check(account.ParentSnapshotRevision()<saved->revision);
        return {saved->revision,std::move(account)};
    }
    OrchardAccountDelivery::Applied Replace(uint64_t expected,OrchardAccountState account){
        auto encoded=account.Encode();const auto revision=store.StageReplaceRetaining(expected,encoded);
        return {revision,std::move(account)};
    }
};
}
OrchardAccountDelivery::Applied OrchardAccountDelivery::Read(WalletManager& w,uint64_t s,const Profile& p,const RestorePoint& point){
    Owner owner(w,s,p);Transaction tx(owner.lease->Database());auto result=owner.Restore(p,point);tx.Commit();return result;
}
OrchardAccountDelivery::Applied OrchardAccountDelivery::ReadForReplay(WalletManager& w,uint64_t s,const Profile& p,const RuntimeAccountReplay& view){
    const auto& context=view.Event(1).context;
    Check(context.activation_height==p.activation&&context.domain.network_code==p.domain.network_code&&
        context.domain.genesis_wire==p.domain.genesis_wire&&context.domain.branch_id==p.domain.branch_id);
    Owner owner(w,s,p);Transaction tx(owner.lease->Database());const auto saved=owner.store.Read();Check(saved.has_value());
    const auto receipt=OrchardAccountState::ReadDeliveryMetadata(saved->state,p.domain,owner.fvk.bytes,p.activation,view.Point({}).checkpoint.block_hash);
    const auto point=view.Point({receipt.sequence,receipt.digest});
    auto result=owner.Restore(p,point);Check(result.revision==saved->revision);tx.Commit();return result;
}
OrchardAccountDelivery::Applied OrchardAccountDelivery::Connect(WalletManager& w,uint64_t s,const Profile& p,uint64_t expected,
        const RestorePoint& point,const RuntimeOutboxEvent& event,const OrchardBlockCandidate& block,
        const consensus::PreparedOrchardState& state,std::span<const consensus::VerifiedOrchardAuthorizations> auths){
    Owner owner(w,s,p);Transaction tx(owner.lease->Database());auto current=owner.Restore(p,point);Check(current.revision==expected);
    auto result=owner.Replace(expected,current.account.AdvanceDelivery(event,block,state,auths).WithParentSnapshotRevision(expected));tx.Commit();return result;
}
OrchardAccountDelivery::Applied OrchardAccountDelivery::Disconnect(WalletManager& w,uint64_t s,const Profile& p,uint64_t expected,
        const RestorePoint& point,const RuntimeOutboxEvent& event,const OrchardBlockCandidate& block,const RestorePoint& parent){
    Owner owner(w,s,p);Transaction tx(owner.lease->Database());auto current=owner.Restore(p,point);Check(current.revision==expected);
    const auto parent_revision=current.account.ParentSnapshotRevision();
    Check(parent_revision && parent_revision<expected);
    auto retained=owner.store.ReadRetained(parent_revision);
    auto prior=OrchardAccountState::Restore(retained.state,p.domain,owner.fvk.bytes,p.activation,parent.checkpoint,parent.lookups);
    Check(prior.ParentSnapshotRevision()<parent_revision);
    auto result=owner.Replace(expected,current.account.RewindDelivery(event,block,prior)
        .WithParentSnapshotRevision(prior.ParentSnapshotRevision()));tx.Commit();return result;
}
OrchardAccountDelivery::Applied OrchardAccountDelivery::Historical(WalletManager& w,uint64_t s,const Profile& p,uint64_t expected,
        const RestorePoint& point,const RuntimeOutboxEvent& event){
    Owner owner(w,s,p);Transaction tx(owner.lease->Database());auto current=owner.Restore(p,point);Check(current.revision==expected);
    auto result=owner.Replace(expected,current.account.ApplyHistoricalDelivery(event));tx.Commit();return result;
}
} // namespace dinero::wallet
