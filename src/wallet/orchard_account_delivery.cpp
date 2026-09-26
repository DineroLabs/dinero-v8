#include "wallet/orchard_account_delivery.h"
#include "wallet/runtime_account_replay.h"
#include "wallet/orchard_operation_archive.h"
#include <algorithm>
#include <map>
#include <set>
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
} // namespace
struct OrchardAccountDelivery::Owner {
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
    OrchardAccountDelivery::Applied RestoreReplay(const OrchardAccountDelivery::Profile& p,const RuntimeAccountReplay& view){
        const auto& context=view.Event(1).context;
        Check(context.activation_height==p.activation&&context.domain.network_code==p.domain.network_code&&
            context.domain.genesis_wire==p.domain.genesis_wire&&context.domain.branch_id==p.domain.branch_id);
        const auto saved=store.Read();Check(saved.has_value());
        const auto receipt=OrchardAccountState::ReadDeliveryMetadata(saved->state,p.domain,fvk.bytes,p.activation,view.Point({}).checkpoint.block_hash);
        Check(receipt.sequence);
        auto result=Restore(p,view.Point({receipt.sequence,receipt.digest}));Check(result.revision==saved->revision);return result;
    }
    OrchardAccountDelivery::Applied Reconcile(OrchardAccountDelivery::Applied current,
            const OrchardAccountDelivery::Profile& p,const RuntimeAccountReplay& view){
        const auto receipt=current.account.Delivery();
        const auto selected=view.SelectedHashes({receipt.sequence,receipt.digest});
        OrchardOperationArchive archive(lease->Database(),identity,p.domain,seed->Bytes());
        auto cursor=archive.Begin(current.account);
        while(cursor.Remaining()){
            const auto page=archive.List(cursor,64);Check(!page.entries.empty());
            for(const auto& located:page.entries){
                const auto record=archive.Read(located.Id());
                // Fully restored pending entries already own their reservations
                // and may carry a newer observation. Never replace their bytes.
                if(current.account.Operations().Entries().contains(located.Id()))continue;
                if(record.observation.height<=current.account.Scan().Checkpoint().height){
                    const auto hash=selected(record.observation.height);
                    if(!hash.ok())throw consensus::OrchardStateLookupError(hash.status());
                    Check(!hash->IsNull());
                    if(*hash==record.observation.block_hash)continue;
                }
                // A legacy owner may already have scanned the replacement
                // branch without this operation present. Reacquire reservations
                // AND replay real bodies from the actual fork before commit.
                const RuntimeOutboxCursor position{receipt.sequence,receipt.digest};
                const auto fork=view.ForkHeight(position,record.observation.block_hash,record.observation.height);
                auto staged=archive.StageReactivate(current.revision,current.account,located,selected);
                current={staged.revision,std::move(staged.account)};
                auto observed=current.account.ObserveReactivatedOperation(located.Id(),fork,view.Point(position).lookups,selected);
                if(observed.Observations()!=current.account.Observations())
                    current=Replace(current.revision,std::move(observed));
            }
            cursor=page.next;
        }
        return current;
    }
    OrchardAccountDelivery::Applied Undo(const OrchardAccountDelivery::Profile& p,
            const OrchardAccountDelivery::Applied& current,const RuntimeOutboxEvent& event,
            const OrchardBlockCandidate& block,const OrchardAccountDelivery::RestorePoint& parent){
        const auto parent_revision=current.account.ParentSnapshotRevision();
        Check(parent_revision&&parent_revision<current.revision);
        auto retained=store.ReadRetained(parent_revision);
        auto prior=OrchardAccountState::Restore(retained.state,p.domain,fvk.bytes,p.activation,parent.checkpoint,parent.lookups);
        Check(prior.ParentSnapshotRevision()<parent_revision);
        return Replace(current.revision,current.account.RewindDelivery(event,block,prior)
            .WithParentSnapshotRevision(prior.ParentSnapshotRevision()));
    }
    OrchardAccountDelivery::Applied Replace(uint64_t expected,OrchardAccountState account){
        auto encoded=account.Encode();const auto revision=store.StageReplaceRetaining(expected,encoded);
        return {revision,std::move(account)};
    }
};
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
std::vector<OrchardAccountDelivery::Enrolled> OrchardAccountDelivery::ReadEnrolledForReplay(
        WalletManager& w,uint64_t session,const RuntimeAccountReplay& view){
    const auto& context=view.Event(1).context;
    const Profile profile{context.domain,context.activation_height,0};
    Owner owner(w,session,profile);Transaction tx(owner.lease->Database());
    struct Statement {
        sqlite3_stmt* p=nullptr;
        ~Statement(){sqlite3_finalize(p);}
    } rows;
    Check(sqlite3_prepare_v2(owner.lease->Database(),
        "SELECT wallet_id,account,revision FROM orchard_wallet_snapshots ORDER BY account",-1,&rows.p,nullptr)==SQLITE_OK);
    using Locator=std::pair<orchard::Hash,uint32_t>;
    std::map<Locator,uint64_t> inventory;
    int rc;
    while((rc=sqlite3_step(rows.p))==SQLITE_ROW){
        Check(sqlite3_column_type(rows.p,0)==SQLITE_BLOB&&sqlite3_column_bytes(rows.p,0)==32);
        Check(sqlite3_column_type(rows.p,1)==SQLITE_INTEGER&&sqlite3_column_type(rows.p,2)==SQLITE_INTEGER);
        const auto number=sqlite3_column_int64(rows.p,1),revision=sqlite3_column_int64(rows.p,2);
        Check(number>=0&&number<0x80000000LL&&revision>0);
        orchard::Hash id{};std::copy_n(static_cast<const uint8_t*>(sqlite3_column_blob(rows.p,0)),32,id.begin());
        // Operational refusal before effects, never a truncated inventory.
        if(inventory.size()>=65536)throw std::runtime_error("Orchard account recovery inventory capacity exceeded");
        Check(inventory.emplace(Locator{id,uint32_t(number)},uint64_t(revision)).second);
    }
    Check(rc==SQLITE_DONE);
    std::vector<Enrolled> result;std::set<Locator> authenticated;
    for(const auto& [locator,revision]:inventory){
        if(locator.first!=owner.identity.wallet_id)continue;
        if(result.size()>=1024)throw std::runtime_error("Orchard account recovery capacity exceeded");
        auto identity=owner.identity;identity.account=locator.second;
        const auto keys=orchard::WalletKeys::FromSeed(owner.seed->Bytes(),identity.account);
        Owner::ViewingKey fvk(keys);
        orchard::WalletSnapshotStore store(owner.lease->Database(),identity,owner.seed->Bytes());
        const auto saved=store.Read();Check(saved&&saved->revision==revision);
        const auto receipt=OrchardAccountState::ReadDeliveryMetadata(saved->state,context.domain,fvk.bytes,
            context.activation_height,view.Point({}).checkpoint.block_hash);
        if(!receipt.sequence)throw std::runtime_error("Wallet recovery account baseline reconciliation required");
        const auto point=view.Point({receipt.sequence,receipt.digest});
        auto account=OrchardAccountState::Restore(saved->state,context.domain,fvk.bytes,
            context.activation_height,point.checkpoint,point.lookups);
        Check(account.ParentSnapshotRevision()<saved->revision);
        Check(authenticated.insert(locator).second);
        // Archive records deliberately use derived wallet IDs in this SAME
        // snapshot table. Recognize only records reached from the restored
        // account's authenticated head; unrelated rows remain a hard refusal.
        std::vector<std::pair<orchard::Hash,uint64_t>> archive_revisions;
        OrchardOperationArchive archive(owner.lease->Database(),identity,context.domain,owner.seed->Bytes());
        auto cursor=archive.Begin(account);
        Check(cursor.Remaining()<=inventory.size());
        while(cursor.Remaining()){
            const auto page=archive.List(cursor,64);
            Check(!page.entries.empty());
            for(const auto& located:page.entries){
                const auto record=archive.Read(located.Id());
                const auto record_identity=archive.RecordIdentity(located.Id());
                const Locator record_locator{record_identity.wallet_id,record_identity.account};
                const auto found=inventory.find(record_locator);
                Check(found!=inventory.end()&&found->second==record.revision&&
                    record.sequence==located.Sequence()&&authenticated.insert(record_locator).second);
                archive_revisions.emplace_back(record_identity.wallet_id,record.revision);
            }
            cursor=page.next;
        }
        result.push_back({identity.account,{saved->revision,std::move(account)},std::move(archive_revisions)});
    }
    if(result.empty())throw std::runtime_error("Wallet recovery account baseline reconciliation required");
    Check(authenticated.size()==inventory.size());
    tx.Commit();return result;
}
OrchardAccountDelivery::Applied OrchardAccountDelivery::ApplyForReplay(WalletManager& w,uint64_t session,const Profile& p,
        uint64_t expected,const RuntimeAccountReplay& view,uint64_t sequence){
    const auto& event=view.Event(sequence);
    Owner owner(w,session,p);Transaction tx(owner.lease->Database());
    auto current=owner.RestoreReplay(p,view);Check(current.revision==expected);
    current=owner.Reconcile(std::move(current),p,view);
    auto result=[&]{
        if(!event.IsOrchardProfile())return owner.Replace(current.revision,current.account.ApplyHistoricalDelivery(event));
        if(event.direction==RuntimeBlockDirection::Connect)
            return owner.Replace(current.revision,current.account.AdvanceDelivery(event,view.Block(sequence),view.State(sequence),view.Authorizations(sequence))
                .WithParentSnapshotRevision(current.revision));
        return owner.Undo(p,current,event,view.Block(sequence),view.Point(event.cursor));
    }();
    result=owner.Reconcile(std::move(result),p,view);tx.Commit();return result;
}
OrchardAccountDelivery::Applied OrchardAccountDelivery::ReconcileForReplay(WalletManager& w,uint64_t session,const Profile& p,
        uint64_t expected,const RuntimeAccountReplay& view){
    Owner owner(w,session,p);Transaction tx(owner.lease->Database());
    auto current=owner.RestoreReplay(p,view);Check(current.revision==expected);
    auto result=owner.Reconcile(std::move(current),p,view);tx.Commit();return result;
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
    auto result=owner.Undo(p,current,event,block,parent);tx.Commit();return result;
}
OrchardAccountDelivery::Applied OrchardAccountDelivery::Historical(WalletManager& w,uint64_t s,const Profile& p,uint64_t expected,
        const RestorePoint& point,const RuntimeOutboxEvent& event){
    Owner owner(w,s,p);Transaction tx(owner.lease->Database());auto current=owner.Restore(p,point);Check(current.revision==expected);
    auto result=owner.Replace(expected,current.account.ApplyHistoricalDelivery(event));tx.Commit();return result;
}
} // namespace dinero::wallet
