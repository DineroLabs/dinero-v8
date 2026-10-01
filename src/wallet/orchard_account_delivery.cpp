#include "wallet/orchard_ownership_inventory.h"
#include "wallet/orchard_account_catalog.h"
#include "wallet/orchard_account_delivery.h"
#include "wallet/orchard_proof_jobs.h"
#include "wallet/runtime_account_replay.h"
#include "wallet/orchard_operation_archive.h"
#include <algorithm>
#include <map>
#include <set>
#include "wallet/wallet_manager.h"
#include <sqlite3.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <stdexcept>
#include <type_traits>
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
// Issuance cannot use EnsureDeliveryIdentity: absent historical ownership
// must refuse without enrollment, including under a caller-owned transaction.
orchard::WalletStorageIdentity ExistingIdentity(sqlite3* db,const OrchardAccountDelivery::Profile& p,bool require_retained=true){
    Check(db&&!sqlite3_get_autocommit(db));
    struct Statement { sqlite3_stmt* p=nullptr;~Statement(){sqlite3_finalize(p);} } q;
    Check(sqlite3_prepare_v2(db,"SELECT runtime_delivery_id FROM wallet_meta WHERE id=1",-1,&q.p,nullptr)==SQLITE_OK);
    Check(sqlite3_step(q.p)==SQLITE_ROW&&sqlite3_column_type(q.p,0)==SQLITE_BLOB&&sqlite3_column_bytes(q.p,0)==32);
    const auto* bytes=static_cast<const uint8_t*>(sqlite3_column_blob(q.p,0));Check(bytes);
    orchard::Hash id{};std::copy_n(bytes,32,id.begin());
    Check(std::any_of(id.begin(),id.end(),[](auto byte){return byte!=0;})&&sqlite3_step(q.p)==SQLITE_DONE);
    if(require_retained){
    Statement retained;
    Check(sqlite3_prepare_v2(db,"SELECT 1 FROM sqlite_master WHERE type='table' AND name='orchard_wallet_retained'",-1,&retained.p,nullptr)==SQLITE_OK);
    Check(sqlite3_step(retained.p)==SQLITE_ROW&&sqlite3_step(retained.p)==SQLITE_DONE);
    }
    return {static_cast<orchard::WalletNetwork>(p.domain.network_code),p.domain.genesis_wire,id,p.account};
}
// Digest is an exact request equality binding inside authenticated storage,
// not an authorization supplied by callers. No secret or randomized plan input
// is regenerated from it. Explicit widths/counts distinguish all field splits.
orchard::Hash SpendRequestCommitment(const orchard::WalletStorageIdentity& identity,
        const OrchardAccountDelivery::Profile& p,const orchard::Hash& id,
        std::span<const orchard::WalletPayment> payments,
        std::span<const orchard::TransparentOutput> outputs,uint64_t fee){
    Check(payments.size()<=DINERO_ORCHARD_V1_MAX_ACTIONS&&outputs.size()<=1024);
    std::unique_ptr<EVP_MD_CTX,decltype(&EVP_MD_CTX_free)> context(EVP_MD_CTX_new(),EVP_MD_CTX_free);
    Check(context&&EVP_DigestInit_ex(context.get(),EVP_sha256(),nullptr)==1);
    const auto raw=[&](std::span<const uint8_t> bytes){Check(EVP_DigestUpdate(context.get(),bytes.data(),bytes.size())==1);};
    const auto number=[&](uint64_t value,size_t width){std::array<uint8_t,8> bytes{};for(size_t i=0;i<width;++i)bytes[i]=value>>(8*i);raw(std::span<const uint8_t>(bytes).first(width));};
    constexpr std::array<uint8_t,8> domain{'D','N','O','R','S','Q','0','1'};raw(domain);
    number(static_cast<uint8_t>(identity.network),1);raw(identity.genesis);raw(identity.wallet_id);
    number(identity.account,4);number(p.domain.branch_id,4);number(p.activation,4);raw(id);
    number(0,4); // Fixed locktime for this pure Orchard-input host API.
    number(payments.size(),4);
    for(const auto& payment:payments){number(payment.amount_una,8);raw(payment.recipient.Raw());raw(payment.memo);}
    number(outputs.size(),4);
    for(const auto& output:outputs){
        Check(!output.script_pub_key.empty()&&output.script_pub_key.size()<=10000);
        number(output.amount_una,8);number(output.script_pub_key.size(),4);raw(output.script_pub_key);
    }
    number(fee,8);orchard::Hash result{};unsigned size=0;
    Check(EVP_DigestFinal_ex(context.get(),result.data(),&size)==1&&size==result.size()&&result!=orchard::Hash{});
    return result;
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
    std::optional<OrchardAccountCatalog::Snapshot> catalog;
    orchard::WalletStorageIdentity identity;
    orchard::WalletKeys keys;
    ViewingKey fvk;
    orchard::WalletSnapshotStore store;
    Owner(WalletManager& wallet,uint64_t session,const OrchardAccountDelivery::Profile& p,bool existing_only=false,bool creating=false)
        :lease(wallet.AcquireDatabaseLease()),seed(),identity(Bind(session,p,existing_only,creating)),
         keys(orchard::WalletKeys::FromSeed(seed->Bytes(),p.account)),fvk(keys),
         store(lease->Database(),identity,seed->Bytes()){}
    orchard::WalletStorageIdentity Bind(uint64_t session,const OrchardAccountDelivery::Profile& p,bool existing_only,bool creating){
        Check(lease->Session()==session&&lease->Database());
        Check(p.domain.network_code<=2&&p.account<0x80000000&&p.activation>0);
        // Refuse missing/locked keys before identity initialization can write.
        seed=lease->CopyRecoverySeed(session);
        if(creating){
            Check(!sqlite3_get_autocommit(lease->Database()));
            catalog=OrchardAccountCatalog::Read(lease->Database(),seed->Bytes());
            Check(catalog&&catalog->generated&&catalog->accounts.size()<OrchardAccountCatalog::kMaxAccounts);
            Check(std::none_of(catalog->accounts.begin(),catalog->accounts.end(),[&](const auto& e){return e.account==p.account;}));
            const auto identity=ExistingIdentity(lease->Database(),p,!catalog->accounts.empty());
            // Only the authenticated genuinely empty catalog may initialize
            // absent schema. A recorded owner never permits missing schema repair.
            if(catalog->accounts.empty()){
                struct Statement {sqlite3_stmt* p=nullptr;~Statement(){sqlite3_finalize(p);}} table,rows;
                Check(sqlite3_prepare_v2(lease->Database(),"SELECT 1 FROM sqlite_master WHERE type='table' AND name='orchard_wallet_retained'",-1,&table.p,nullptr)==SQLITE_OK);
                const int rc=sqlite3_step(table.p);
                if(rc==SQLITE_ROW){
                    Check(sqlite3_step(table.p)==SQLITE_DONE);
                    Check(sqlite3_prepare_v2(lease->Database(),"SELECT 1 FROM orchard_wallet_retained",-1,&rows.p,nullptr)==SQLITE_OK);
                    Check(sqlite3_step(rows.p)==SQLITE_DONE);
                }else Check(rc==SQLITE_DONE);
                orchard::WalletSnapshotStore::InitializeSchemaUnderTransaction(lease->Database());
            }
            return identity;
        }

        return existing_only?ExistingIdentity(lease->Database(),p):Identity(lease->EnsureDeliveryIdentity(),p);
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
        // A zero receipt is usable only if the FULL encrypted scan restores
        // at the checked activation origin. It is not an applied cursor.
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
    std::vector<OrchardAccountDelivery::Enrolled> ReadInventory(const RuntimeAccountReplay&,bool allow_empty);
    void ValidateRetainedInventory(const std::vector<OrchardAccountDelivery::Enrolled>&,const RuntimeAccountReplay&);
    std::vector<OrchardAccountDelivery::Enrolled> ValidateCatalogInventory(const OrchardAccountDelivery::Profile&,const RuntimeAccountReplay&);
    // Each temporary owner releases its seed pin before the next account is
    // opened. The caller's outer FULL transaction/lease spans the entire pass.
    static void ReconcileSpendCatalog(WalletManager&,uint64_t,const OrchardAccountDelivery::Profile&,
        const RuntimeAccountReplay&,const std::optional<orchard::Hash>& new_operation);
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
OrchardAccountDelivery::IssuedReceiver OrchardAccountDelivery::IssueReceiverForReplay(
        WalletManager& w,uint64_t session,const Profile& p,const RuntimeAccountReplay& view,orchard::WalletScope scope){
    Check(scope==orchard::WalletScope::External||scope==orchard::WalletScope::Internal);
    // Transaction begins before identity, ciphertext or derivation reads.
    // The outer lease keeps SQLite alive until rollback on every failure.
    auto lease=w.AcquireDatabaseLease();Check(lease->Session()==session);
    Transaction tx(lease->Database());Owner owner(w,session,p,true);
    auto current=owner.RestoreReplay(p,view);
    auto issued=current.account.IssueReceiver(scope);
    IssuedReceiver result{0,issued.second.EncodeAddress(owner.identity.network)};
    result.revision=owner.Replace(current.revision,std::move(issued.first)).revision;
    tx.Commit();return result;
}
std::vector<OrchardAccountDelivery::Enrolled> OrchardAccountDelivery::Owner::ReadInventory(
        const RuntimeAccountReplay& view,bool allow_empty){
    auto& owner=*this;const auto& context=view.Event(1).context;
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
        const auto cursor=archive.Begin(account); // Preserve exact current account encoding/owner guard.
        const auto captured=archive.CaptureCurrent(fvk.bytes,context.activation_height);
        Check(captured.revision==saved->revision && captured.metadata.Archive()==account.Archive() &&
            captured.metadata.Delivery()==account.Delivery() &&
            captured.metadata.ParentSnapshotRevision()==account.ParentSnapshotRevision());
        Check(captured.archive.size()==cursor.Remaining()&&captured.archive.size()<=inventory.size());
        for(const auto& archived:captured.archive){
            const auto& record=archived.record;
            const Locator record_locator{archived.identity.wallet_id,archived.identity.account};
            const auto found=inventory.find(record_locator);
            Check(found!=inventory.end()&&found->second==record.revision&&authenticated.insert(record_locator).second);
            archive_revisions.emplace_back(record_locator.first,record.revision);
        }
        result.push_back({identity.account,{saved->revision,std::move(account)},std::move(archive_revisions)});
    }
    if(result.empty()&&!allow_empty)throw std::runtime_error("Wallet recovery account baseline reconciliation required");
    Check(authenticated.size()==inventory.size());
    return result;
}
void OrchardAccountDelivery::Owner::ValidateRetainedInventory(const std::vector<OrchardAccountDelivery::Enrolled>& accounts,const RuntimeAccountReplay& view){
    struct Statement {sqlite3_stmt* p=nullptr;~Statement(){sqlite3_finalize(p);}} table,rows;
    auto* db=lease->Database();
    Check(sqlite3_prepare_v2(db,"SELECT 1 FROM sqlite_master WHERE type='table' AND name='orchard_wallet_retained'",-1,&table.p,nullptr)==SQLITE_OK);
    const int present=sqlite3_step(table.p);
    if(present==SQLITE_DONE){Check(catalog&&catalog->accounts.empty());return;}
    Check(present==SQLITE_ROW&&sqlite3_step(table.p)==SQLITE_DONE);
    Check(sqlite3_prepare_v2(db,"SELECT wallet_id,account,revision FROM orchard_wallet_retained ORDER BY wallet_id,account,revision",-1,&rows.p,nullptr)==SQLITE_OK);
    size_t count=0;int rc;
    while((rc=sqlite3_step(rows.p))==SQLITE_ROW){
        Check(++count<=65536&&sqlite3_column_type(rows.p,0)==SQLITE_BLOB&&sqlite3_column_bytes(rows.p,0)==32);
        Check(sqlite3_column_type(rows.p,1)==SQLITE_INTEGER&&sqlite3_column_type(rows.p,2)==SQLITE_INTEGER);
        const auto account=sqlite3_column_int64(rows.p,1),revision=sqlite3_column_int64(rows.p,2);
        Check(account>=0&&account<0x80000000LL&&revision>0);
        auto retained_identity=identity;retained_identity.account=static_cast<uint32_t>(account);
        const auto* bytes=static_cast<const uint8_t*>(sqlite3_column_blob(rows.p,0));Check(bytes);std::copy_n(bytes,32,retained_identity.wallet_id.begin());
        // Current inventory has already authenticated all normal and reached
        // archive owners. ReadRetained also requires a current authenticated
        // envelope and a strictly earlier revision; orphan history refuses.
        orchard::WalletSnapshotStore retained(db,retained_identity,seed->Bytes());
        (void)retained.ReadRetained(static_cast<uint64_t>(revision));
    }
    Check(rc==SQLITE_DONE);
    size_t predecessors=0;const auto& context=view.Event(1).context;
    for(const auto& enrolled:accounts){
        auto account_identity=identity;account_identity.account=enrolled.number;
        const auto keys=orchard::WalletKeys::FromSeed(seed->Bytes(),enrolled.number);ViewingKey viewing(keys);
        orchard::WalletSnapshotStore snapshots(db,account_identity,seed->Bytes());
        auto revision=enrolled.state.account.ParentSnapshotRevision();auto upper=enrolled.state.revision;
        while(revision){
            Check(++predecessors<=65536&&revision<upper);const auto parent=snapshots.ReadRetained(revision);
            const auto receipt=OrchardAccountState::ReadDeliveryMetadata(parent.state,context.domain,viewing.bytes,context.activation_height,view.Point({}).checkpoint.block_hash);
            const auto point=view.Point({receipt.sequence,receipt.digest});
            const auto restored=OrchardAccountState::Restore(parent.state,context.domain,viewing.bytes,context.activation_height,point.checkpoint,point.lookups);
            upper=revision;revision=restored.ParentSnapshotRevision();
        }
    }
}
std::vector<OrchardAccountDelivery::Enrolled> OrchardAccountDelivery::ReadEnrolledForReplay(
        WalletManager& w,uint64_t session,const RuntimeAccountReplay& view){
    const auto& context=view.Event(1).context;
    const Profile profile{context.domain,context.activation_height,0};
    Owner owner(w,session,profile);Transaction tx(owner.lease->Database());
    auto result=owner.ReadInventory(view,false);tx.Commit();return result;
}
OrchardAccountDelivery::CatalogEnrolled OrchardAccountDelivery::ReadCatalogForReplay(
        WalletManager& w,uint64_t session,const RuntimeAccountReplay& view){
    const auto& context=view.Event(1).context;
    const Profile profile{context.domain,context.activation_height,0};
    auto lease=w.AcquireDatabaseLease();Check(lease->Session()==session);
    Transaction tx(lease->Database());std::optional<OrchardAccountCatalog::Snapshot> catalog;
    {
        auto seed=lease->CopyRecoverySeed(session);
        catalog=OrchardAccountCatalog::Read(lease->Database(),seed->Bytes());Check(catalog&&catalog->generated);
        (void)ExistingIdentity(lease->Database(),profile,!catalog->accounts.empty());
    }
    if(catalog->accounts.empty()){
        // Genuinely empty catalogs may predate storage schema. Do not construct
        // a WalletSnapshotStore or run DDL just to report known emptiness.
        const auto exists=[&](const char* name){
            struct Statement {sqlite3_stmt* p=nullptr;~Statement(){sqlite3_finalize(p);}} q;
            Check(sqlite3_prepare_v2(lease->Database(),"SELECT 1 FROM sqlite_master WHERE type='table' AND name=?",-1,&q.p,nullptr)==SQLITE_OK);
            Check(sqlite3_bind_text(q.p,1,name,-1,SQLITE_STATIC)==SQLITE_OK);const int rc=sqlite3_step(q.p);
            if(rc==SQLITE_DONE)return false;
            Check(rc==SQLITE_ROW&&sqlite3_step(q.p)==SQLITE_DONE);return true;
        };
        const bool schema=exists("orchard_wallet_schema"),rows=exists("orchard_wallet_snapshots"),retained=exists("orchard_wallet_retained");
        Check((!schema&&!rows&&!retained)||(schema&&rows&&retained));
        if(schema){
            struct Statement {sqlite3_stmt* p=nullptr;~Statement(){sqlite3_finalize(p);}} version;
            Check(sqlite3_prepare_v2(lease->Database(),"SELECT version FROM orchard_wallet_schema WHERE id=1",-1,&version.p,nullptr)==SQLITE_OK);
            Check(sqlite3_step(version.p)==SQLITE_ROW&&sqlite3_column_type(version.p,0)==SQLITE_INTEGER&&sqlite3_column_int64(version.p,0)==1&&sqlite3_step(version.p)==SQLITE_DONE);
            for(const auto* sql:{"SELECT 1 FROM orchard_wallet_snapshots","SELECT 1 FROM orchard_wallet_retained"}){
                Statement q;Check(sqlite3_prepare_v2(lease->Database(),sql,-1,&q.p,nullptr)==SQLITE_OK);Check(sqlite3_step(q.p)==SQLITE_DONE);
            }
        }
        CatalogEnrolled result{std::move(*catalog),{}};tx.Commit();return result;
    }
    Owner owner(w,session,profile,true);auto accounts=owner.ValidateCatalogInventory(profile,view);
    Check(owner.catalog==catalog);CatalogEnrolled result{std::move(*catalog),std::move(accounts)};
    tx.Commit();return result;
}
std::vector<OrchardAccountDelivery::Enrolled> OrchardAccountDelivery::Owner::ValidateCatalogInventory(
        const OrchardAccountDelivery::Profile& p,const RuntimeAccountReplay& view){
    const auto& context=view.Event(1).context;
    Check(context.activation_height==p.activation&&context.domain.network_code==p.domain.network_code&&
        context.domain.genesis_wire==p.domain.genesis_wire&&context.domain.branch_id==p.domain.branch_id);
    catalog=OrchardAccountCatalog::Read(lease->Database(),seed->Bytes());Check(catalog&&catalog->generated);
    // First genuine creation is between schema initialization and its first
    // retained write here. Keep that existing empty-catalog initialization
    // owner; established catalogs must have the complete read-only schema.
    std::optional<OrchardOwnershipInventory::Snapshot> ownership;
    if(!catalog->accounts.empty()) {
        ownership=OrchardOwnershipInventory::Read(lease->Database(),seed->Bytes());
        Check(ownership->catalog==*catalog&&ownership->wallet_id==identity.wallet_id);
    }
    auto inventory=ReadInventory(view,true);Check(inventory.size()==catalog->accounts.size());
    if(ownership) {
        Check(ownership->accounts.size()==inventory.size());
        for(size_t i=0;i<inventory.size();++i)
            Check(ownership->accounts[i].entry.account==inventory[i].number&&
                  ownership->accounts[i].current.revision==inventory[i].state.revision);
    }
    for(size_t i=0;i<inventory.size();++i){
        const auto& entry=catalog->accounts[i];
        Check(entry.account==inventory[i].number&&entry.network==p.domain.network_code&&entry.genesis==p.domain.genesis_wire&&
            entry.branch==p.domain.branch_id&&entry.activation==p.activation);
    }
    ValidateRetainedInventory(inventory,view);return inventory;
}
OrchardAccountDelivery::IssuedReceiver OrchardAccountDelivery::IssueCatalogReceiverForReplay(
        WalletManager& w,uint64_t session,const Profile& p,const RuntimeAccountReplay& view,orchard::WalletScope scope){
    Check(scope==orchard::WalletScope::External||scope==orchard::WalletScope::Internal);
    auto lease=w.AcquireDatabaseLease();Check(lease->Session()==session);
    Transaction tx(lease->Database());Owner owner(w,session,p,true);
    auto inventory=owner.ValidateCatalogInventory(p,view);
    const auto found=std::find_if(inventory.begin(),inventory.end(),[&](const auto& entry){return entry.number==p.account;});
    Check(found!=inventory.end());
    auto issued=found->state.account.IssueReceiver(scope);
    IssuedReceiver result{0,issued.second.EncodeAddress(owner.identity.network)};
    result.revision=owner.Replace(found->state.revision,std::move(issued.first)).revision;
    tx.Commit();return result;
}
OrchardAccountDelivery::IssuedReceiver OrchardAccountDelivery::CreateAccountForReplay(
        WalletManager& w,uint64_t session,const Profile& p,const RuntimeAccountReplay& view){
    const auto& context=view.Event(1).context;
    Check(context.activation_height==p.activation&&context.domain.network_code==p.domain.network_code&&
        context.domain.genesis_wire==p.domain.genesis_wire&&context.domain.branch_id==p.domain.branch_id);
    auto lease=w.AcquireDatabaseLease();Check(lease->Session()==session);
    Transaction tx(lease->Database());Owner owner(w,session,p,true,true);
    (void)owner.ValidateCatalogInventory(p,view);
    const auto origin=view.Point({});
    const auto initial=OrchardAccountState::Begin(p.domain,owner.fvk.bytes,p.activation,origin.checkpoint.block_hash);
    // Restore against the actual source origin to validate its complete empty
    // scan state. No cursor is advanced and no event or baseline is fabricated.
    auto account=OrchardAccountState::Restore(initial.Encode(),p.domain,owner.fvk.bytes,p.activation,origin.checkpoint,origin.lookups);
    auto issued=account.IssueReceiver(orchard::WalletScope::External);
    IssuedReceiver result{0,issued.second.EncodeAddress(owner.identity.network)};
    result.revision=owner.Replace(0,std::move(issued.first)).revision;
    const OrchardAccountCatalog::Entry entry{p.account,static_cast<uint8_t>(p.domain.network_code),p.domain.genesis_wire,p.domain.branch_id,p.activation};
    (void)OrchardAccountCatalog::StageAppend(lease->Database(),owner.seed->Bytes(),owner.catalog->revision,entry);
    tx.Commit();return result;
}
void OrchardAccountDelivery::Owner::ReconcileSpendCatalog(WalletManager& w,uint64_t session,
        const Profile& p,const RuntimeAccountReplay& view,const std::optional<orchard::Hash>& new_operation){
    std::vector<Enrolled> inventory;
    {
        Owner owner(w,session,p,true);inventory=owner.ValidateCatalogInventory(p,view);
    }
    const auto head=view.Head();Check(head.sequence&&!head.digest.IsNull());
    for(const auto& enrolled:inventory){
        const auto receipt=enrolled.state.account.Delivery();
        Check(receipt.sequence==head.sequence&&receipt.digest==head.digest);
    }
    for(const auto& enrolled:inventory){
        const Profile account_profile{p.domain,p.activation,enrolled.number};
        Owner owner(w,session,account_profile,true);
        auto current=owner.RestoreReplay(account_profile,view);
        Check(current.revision==enrolled.state.revision);
        if(new_operation){
            Check(!current.account.Operations().Entries().contains(*new_operation));
            OrchardOperationArchive archive(owner.lease->Database(),owner.identity,p.domain,owner.seed->Bytes());
            Check(!archive.Contains(*new_operation));
        }
        (void)owner.Reconcile(std::move(current),account_profile,view);
    }
}
OrchardAccountDelivery::PreparedSpend OrchardAccountDelivery::ReserveCatalogSpendForReplay(
        WalletManager& w,uint64_t session,const Profile& p,uint64_t expected,const RuntimeAccountReplay& view,
        const orchard::Hash& id,std::span<const orchard::WalletPayment> payments,
        std::span<const orchard::TransparentOutput> outputs,uint64_t fee){
    auto result=ReserveSpendForReplay(w,session,p,expected,view,id,payments,outputs,fee,nullptr);
    return std::move(*result.direct);
}
std::unique_ptr<OrchardAccountDelivery::QueuedSpend> OrchardAccountDelivery::QueueCatalogSpendForReplay(
        WalletManager& w,uint64_t session,const Profile& p,uint64_t expected,const RuntimeAccountReplay& view,
        const orchard::Hash& id,std::span<const orchard::WalletPayment> payments,
        std::span<const orchard::TransparentOutput> outputs,uint64_t fee,OrchardProofJobs& jobs){
    return ReserveSpendForReplay(w,session,p,expected,view,id,payments,outputs,fee,&jobs).queued;
}
std::unique_ptr<OrchardAccountDelivery::QueuedSpend> OrchardAccountDelivery::QueueCatalogRequestForReplay(
        WalletManager& w,uint64_t session,const Profile& p,uint64_t expected,const RuntimeAccountReplay& view,
        const orchard::Hash& id,std::span<const orchard::WalletPayment> payments,
        std::span<const orchard::TransparentOutput> outputs,uint64_t fee,OrchardProofJobs& jobs){
    return ReserveSpendForReplay(w,session,p,expected,view,id,payments,outputs,fee,&jobs,true).queued;
}
OrchardAccountDelivery::SpendResult OrchardAccountDelivery::ReserveSpendForReplay(
        WalletManager& w,uint64_t session,const Profile& p,uint64_t expected,const RuntimeAccountReplay& view,
        const orchard::Hash& id,std::span<const orchard::WalletPayment> payments,
        std::span<const orchard::TransparentOutput> outputs,uint64_t fee,OrchardProofJobs* jobs,bool bind_request){
    Check(id!=orchard::Hash{}&&(!payments.empty()||!outputs.empty())&&payments.size()<=DINERO_ORCHARD_V1_MAX_ACTIONS);
    Check(fee<=orchard::kMaxMoneyUna);uint64_t required=fee;
    const auto add=[&](uint64_t amount){Check(amount>0&&amount<=orchard::kMaxMoneyUna-required);required+=amount;};
    for(const auto& payment:payments)add(payment.amount_una);
    for(const auto& output:outputs)add(output.amount_una);
    auto lease=w.AcquireDatabaseLease();Check(lease->Session()==session);Transaction tx(lease->Database());
    std::optional<orchard::Hash> request;
    {
        Owner owner(w,session,p,true);auto inventory=owner.ValidateCatalogInventory(p,view);
        const auto requested=std::find_if(inventory.begin(),inventory.end(),[&](const auto& e){return e.number==p.account;});
        Check(requested!=inventory.end());
        if(bind_request){
            Check(jobs);request=SpendRequestCommitment(owner.identity,p,id,payments,outputs,fee);
            std::unique_ptr<QueuedSpend> existing;
            for(const auto& enrolled:inventory){
                const auto& account=enrolled.state.account;
                const auto pending=account.Operations().Entries().find(id);
                auto identity=owner.identity;identity.account=enrolled.number;
                OrchardOperationArchive archive(lease->Database(),identity,p.domain,owner.seed->Bytes());
                const bool archived=archive.Contains(id);
                if(pending==account.Operations().Entries().end()&&!archived)continue;
                Check(enrolled.number==p.account&&!existing);
                std::optional<OrchardOperationArchive::Record> record;
                if(archived){record=archive.Read(id);Check(record->operation.Entries().at(id).request_commitment==request);}
                const bool only_archive=pending==account.Operations().Entries().end();
                const auto& entry=only_archive?record->operation.Entries().at(id):pending->second;
                Check(entry.request_commitment==request);
                existing=std::make_unique<QueuedSpend>(QueuedSpend{enrolled.state.revision,id,
                    std::vector<orchard::TransparentOutput>(outputs.begin(),outputs.end()),fee,false});
                existing->existing_request=true;existing->archived=only_archive;existing->durable=entry;
                if(only_archive)existing->observation=record->observation;
                else if(const auto observed=account.Observations().find(id);observed!=account.Observations().end())existing->observation=observed->second;
            }
            if(existing){
                // No reconciliation, random plan, executor lookup, re-enqueue,
                // SQL mutation or implicit cancellation on this retry path.
                SpendResult result;result.queued=std::move(existing);tx.Commit();return result;
            }
        }
        Check(requested->state.revision==expected);
    }
    Owner::ReconcileSpendCatalog(w,session,p,view,id);
    Owner owner(w,session,p,true);auto inventory=owner.ValidateCatalogInventory(p,view);
    const auto requested=std::find_if(inventory.begin(),inventory.end(),[&](const auto& e){return e.number==p.account;});
    Check(requested!=inventory.end());auto current=requested->state;
    std::set<orchard::Hash> reserved;std::set<orchard::Hash> operations;
    for(const auto& account:inventory)for(const auto& [operation,entry]:account.state.account.Operations().Entries()){
        Check(operations.insert(operation).second);
        for(const auto& nullifier:entry.nullifiers)Check(reserved.insert(nullifier).second);
    }
    struct Candidate{const ScannedOrchardNote* note;orchard::Hash nullifier;};std::vector<Candidate> candidates;
    for(const auto& note:current.account.Scan().Notes()){
        Check(note.note&&note.witness);orchard::Hash nullifier{};
        std::copy_n(note.note->Facts().nullifier,nullifier.size(),nullifier.begin());
        if(!reserved.contains(nullifier))candidates.push_back({&note,nullifier});
    }
    // Bounded action count: prefer larger existing notes, with a stable tie
    // break. An insufficient bounded selection refuses; no partial reservation.
    std::sort(candidates.begin(),candidates.end(),[](const auto& a,const auto& b){
        const auto av=a.note->note->Facts().amount,bv=b.note->note->Facts().amount;
        return av!=bv?av>bv:a.nullifier<b.nullifier;
    });
    uint64_t selected=0;std::vector<orchard::WalletSpendInput> inputs;
    for(const auto& candidate:candidates){
        if(selected>=required||inputs.size()==DINERO_ORCHARD_V1_MAX_ACTIONS)break;
        const auto amount=candidate.note->note->Facts().amount;
        Check(amount<=orchard::kMaxMoneyUna-selected);selected+=amount;
        inputs.push_back({*candidate.note->note,*candidate.note->witness});
    }
    Check(selected>=required&&!inputs.empty());
    std::vector<orchard::WalletPayment> recipients(payments.begin(),payments.end());
    auto account=current.account;
    if(selected>required){
        Check(recipients.size()<DINERO_ORCHARD_V1_MAX_ACTIONS);
        auto issued=account.IssueReceiver(orchard::WalletScope::Internal);account=std::move(issued.first);
        recipients.push_back({selected-required,std::move(issued.second)});
    }
    orchard::Hash anchor{};const auto& checkpoint=current.account.Scan().Checkpoint();
    Check(checkpoint.height>=p.activation);std::copy(checkpoint.anchor.begin(),checkpoint.anchor.end(),anchor.begin());
    std::vector<orchard::TransparentOutput> transparent(outputs.begin(),outputs.end());
    auto signing=orchard::SigningContext::Create(p.domain,0,{},transparent,fee);
    auto plan=orchard::WalletBundlePlan::PrepareSpend(owner.keys,inputs,anchor,recipients);
    const auto intent=plan.Intent(signing);Check(intent.Inputs().empty());
    for(const auto& nullifier:intent.Nullifiers())Check(!reserved.contains(nullifier));
    std::unique_ptr<OrchardProofJobs::Submission> submission;
    if(jobs)submission=jobs->PrepareOwned(id,
        {owner.identity,p.domain.branch_id,session,lease->InstanceToken()},std::move(plan),std::move(signing));
    auto staged=owner.Replace(current.revision,request?account.ReserveRequest(id,intent,*request):account.Reserve(id,intent));
    static_assert(std::is_nothrow_move_constructible_v<SpendResult>);
    SpendResult result;
    if(submission){
        submission->Bind(staged.account.Operations());
        result.queued=std::make_unique<QueuedSpend>(QueuedSpend{staged.revision,id,std::move(transparent),fee,false});
        if(request)result.queued->durable=staged.account.Operations().Entries().at(id);
    }else{
        result.direct=std::make_unique<PreparedSpend>(PreparedSpend{staged.revision,id,staged.account.Operations(),std::move(plan),std::move(signing),std::move(transparent),fee});
    }
    tx.Commit();
    if(submission)result.queued->enqueued=submission->Publish();
    return result;
}
OrchardAccountDelivery::OwnedProof OrchardAccountDelivery::ReadCatalogProofForReplay(
        WalletManager& w,uint64_t session,const Profile& p,const RuntimeAccountReplay& view,
        const orchard::Hash& id,OrchardProofJobs& jobs){
    Check(id!=orchard::Hash{});
    auto lease=w.AcquireDatabaseLease();Check(lease->Session()==session);Transaction tx(lease->Database());
    Owner owner(w,session,p,true);const auto inventory=owner.ValidateCatalogInventory(p,view);
    const auto requested=std::find_if(inventory.begin(),inventory.end(),[&](const auto& e){return e.number==p.account;});
    Check(requested!=inventory.end()&&requested->state.account.Operations().Entries().contains(id));
    auto captured=jobs.CaptureOwned(id,{owner.identity,p.domain.branch_id,session,lease->InstanceToken()},
        requested->state.account.Operations());
    OwnedProof result{requested->state.revision,captured.state,std::move(captured.proof)};
    static_assert(std::is_nothrow_move_constructible_v<OwnedProof>);
    tx.Commit();return result;
}
OrchardAccountDelivery::RequestProof OrchardAccountDelivery::ReadCatalogRequestProofForReplay(
        WalletManager& w,uint64_t session,const Profile& p,const RuntimeAccountReplay& view,
        const orchard::Hash& id,std::span<const orchard::WalletPayment> payments,
        std::span<const orchard::TransparentOutput> outputs,uint64_t fee,OrchardProofJobs& jobs){
    Check(id!=orchard::Hash{}&&(!payments.empty()||!outputs.empty()));
    auto lease=w.AcquireDatabaseLease();Check(lease->Session()==session);Transaction tx(lease->Database());
    Owner owner(w,session,p,true);const auto inventory=owner.ValidateCatalogInventory(p,view);
    const auto requested=std::find_if(inventory.begin(),inventory.end(),[&](const auto& e){return e.number==p.account;});
    Check(requested!=inventory.end());
    const auto& queue=requested->state.account.Operations();const auto found=queue.Entries().find(id);
    Check(found!=queue.Entries().end()&&found->second.inputs.empty());
    Check(found->second.request_commitment==SpendRequestCommitment(owner.identity,p,id,payments,outputs,fee));
    auto captured=jobs.CaptureOwned(id,{owner.identity,p.domain.branch_id,session,lease->InstanceToken()},queue);
    RequestProof result{requested->state.revision,found->second,captured.state,std::move(captured.proof)};
    static_assert(std::is_nothrow_move_constructible_v<RequestProof>);
    tx.Commit();return result;
}
OrchardAccountDelivery::Applied OrchardAccountDelivery::ReadyCatalogSpendForReplay(
        WalletManager& w,uint64_t session,const Profile& p,uint64_t expected,const RuntimeAccountReplay& view,
        const orchard::Hash& id,const consensus::VerifiedOrchardAuthorizations& authorization){
    Check(id!=orchard::Hash{}&&authorization.Transaction().Inputs().empty());
    auto lease=w.AcquireDatabaseLease();Check(lease->Session()==session);Transaction tx(lease->Database());
    Owner owner(w,session,p,true);const auto inventory=owner.ValidateCatalogInventory(p,view);
    const auto requested=std::find_if(inventory.begin(),inventory.end(),[&](const auto& e){return e.number==p.account;});
    Check(requested!=inventory.end()&&requested->state.revision==expected);
    const auto& current=requested->state;const auto found=current.account.Operations().Entries().find(id);
    Check(found!=current.account.Operations().Entries().end()&&found->second.inputs.empty());
    // SetReady binds exact message/nullifiers/body and refuses an observed
    // unfinished operation. A Ready retry must retain identical signed bytes.
    auto next=current.account.SetReady(id,authorization);
    if(found->second.phase==OrchardOperationQueue::Phase::Ready){tx.Commit();return current;}
    auto result=owner.Replace(current.revision,std::move(next));tx.Commit();return result;
}
OrchardAccountDelivery::FinalizedProof OrchardAccountDelivery::FinalizeCatalogProofForReplay(
        WalletManager& w,uint64_t session,const Profile& p,const RuntimeAccountReplay& view,
        const orchard::Hash& id,const consensus::VerifiedOrchardAuthorizations& authorization,
        OrchardProofJobs& jobs){
    Check(id!=orchard::Hash{}&&authorization.Transaction().Inputs().empty());
    auto lease=w.AcquireDatabaseLease();Check(lease->Session()==session);Transaction tx(lease->Database());
    Owner owner(w,session,p,true);const auto inventory=owner.ValidateCatalogInventory(p,view);
    const auto requested=std::find_if(inventory.begin(),inventory.end(),[&](const auto& e){return e.number==p.account;});
    Check(requested!=inventory.end());
    const auto& current=requested->state;const auto found=current.account.Operations().Entries().find(id);
    Check(found!=current.account.Operations().Entries().end()&&found->second.inputs.empty());
    auto captured=jobs.CaptureOwned(id,{owner.identity,p.domain.branch_id,session,lease->InstanceToken()},
        current.account.Operations());
    const bool already_ready=found->second.phase==OrchardOperationQueue::Phase::Ready;
    if(captured.state){
        Check(captured.state==OrchardProofJobs::State::Succeeded&&captured.proof);
        const auto& envelope=authorization.Transaction();
        const auto exact=orchard::TransactionEnvelope::Create(envelope.LockTime(),{},envelope.Outputs(),
            envelope.ExplicitFee(),captured.proof->Bytes());
        Check(exact.CanonicalBytes()==authorization.Orchard().CanonicalBytes());
    }else Check(already_ready); // Missing work cannot promote a reservation.
    auto next=current.account.SetReady(id,authorization);
    auto applied=already_ready?current:owner.Replace(current.revision,std::move(next));
    FinalizedProof result{std::move(applied),false};
    static_assert(std::is_nothrow_move_constructible_v<FinalizedProof>);
    tx.Commit();
    result.retired_job=jobs.RetireCaptured(id,captured.token);
    return result;
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
