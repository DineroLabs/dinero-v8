#include "wallet/runtime_wallet_recovery.h"
#include "wallet/runtime_recovery_source.h"
#include "wallet/runtime_index_delivery.h"
#include "wallet/runtime_origin_projection.h"
#include "wallet/wallet_manager.h"
#include <algorithm>
#include <atomic>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <sqlite3.h>

namespace dinero {
namespace {
bool Same(const RuntimeIndexProgress& a, const RuntimeIndexProgress& b) {
    return a.cursor==b.cursor && a.origin_hash==b.origin_hash &&
        a.origin_height==b.origin_height && a.tip_hash==b.tip_hash && a.tip_height==b.tip_height;
}
void Require(bool ok, const char* error) {
    if (!ok) throw std::runtime_error(error);
}
auto ReadStores(WalletManager& wallet, UTXOIndex& index, uint64_t session) {
    const auto lease=wallet.AcquireDatabaseLease();
    Require(lease->Session()==session,"Wallet recovery selection changed");
    const auto indexed=RuntimeIndexDelivery::ReadForWallet(wallet,index,session);
    const auto ordinary=RuntimeOrdinaryDelivery::ReadForWallet(wallet,session);
    Require(indexed.has_value() && ordinary.has_value(),"Wallet recovery baseline reconciliation required");
    return std::pair{*indexed,*ordinary};
}
void CheckPosition(const RuntimeIndexProgress& progress, const RuntimeOutboxPage& page) {
    Require(page.after_tip.has_value() && page.after_tip->first==progress.tip_hash &&
        page.after_tip->second==progress.tip_height,"Wallet recovery source position mismatch");
}
// What a complete Orchard catalog pass proved, and the wallet database as it
// stood inside that pass's last (read-only) recheck transaction. Own-connection
// writes move total_changes; another connection's commits move data_version.
struct IdleCatalogKey {
    const WalletManager* wallet=nullptr;
    std::shared_ptr<const void> instance;
    // The counters below compare only on the connection that produced them.
    const sqlite3* db=nullptr;
    uint64_t session=0,generation=0;
    RuntimeOutboxCursor head{};
    decltype(wallet::OrchardAccountDelivery::RestorePoint::checkpoint) origin{};
    sqlite3_int64 total_changes=0;int64_t data_version=0;
    RuntimeOrchardCatalogRecoveryResult result{};
    std::chrono::steady_clock::time_point full_at{};
};
struct IdleCatalogState {
    std::mutex mutex;
    std::optional<IdleCatalogKey> key;
    std::atomic<uint64_t> skipped{0},misses{0};
    std::atomic<int64_t> interval_ms{60000};
};
IdleCatalogState& IdleCatalog(){static IdleCatalogState value;return value;}
int64_t DataVersion(sqlite3* db){
    sqlite3_stmt* raw=nullptr;
    Require(sqlite3_prepare_v2(db,"PRAGMA data_version",-1,&raw,nullptr)==SQLITE_OK,"Wallet data version unavailable");
    std::unique_ptr<sqlite3_stmt,decltype(&sqlite3_finalize)> q(raw,sqlite3_finalize);
    Require(sqlite3_step(raw)==SQLITE_ROW,"Wallet data version unavailable");
    return sqlite3_column_int64(raw,0);
}
bool SameResult(const RuntimeOrchardCatalogRecoveryResult& a,const RuntimeOrchardCatalogRecoveryResult& b){
    return a.applied==b.applied&&a.account_revisions==b.account_revisions&&a.catalog_revision==b.catalog_revision;
}
}
uint64_t RuntimeWalletRecovery::IdleCatalogPassesSkipped() noexcept{return IdleCatalog().skipped.load();}
uint64_t RuntimeWalletRecovery::IdleCatalogSkipMisses() noexcept{return IdleCatalog().misses.load();}
void RuntimeWalletRecovery::SetIdleCatalogFullPassIntervalForTest(std::chrono::milliseconds value) noexcept{
    IdleCatalog().interval_ms.store(value.count());
}

RuntimeTransparentRecoveryResult RuntimeWalletRecovery::Resume(
        const Source& source, WalletManager& wallet, UTXOIndex& index, uint64_t session) {
    {
        const auto lease=wallet.AcquireDatabaseLease();
        Require(wallet.database_leases_==1,"Wallet recovery requires released caller lease");
    }
    // Snapshot both receipts under one wallet ownership interval, then release
    // ownership before acquiring the selected-chain source lock.
    auto [indexed,ordinary]=ReadStores(wallet,index,session);
    const auto origin=source({},1);
    Require(!origin.events.empty(),"Wallet recovery source origin unavailable");
    const auto& first=origin.events.front();
    const bool connect=first.direction==RuntimeBlockDirection::Connect;
    const auto origin_hash=connect?first.context.parent_hash:first.context.block_hash;
    const auto origin_height=connect?first.context.height-1:first.context.height;
    const auto target=origin.head;
    for (const auto* progress : {&indexed,&ordinary}) {
        Require(progress->cursor.sequence && progress->cursor.sequence<=target.sequence &&
            progress->origin_hash==origin_hash && progress->origin_height==origin_height,
            "Wallet recovery source origin mismatch");
        // Validate BOTH applied cursors, including the ahead store and EOF. A
        // matching height or agreement between stores is not source validation.
        CheckPosition(*progress,source(progress->cursor,1));
    }
    while (std::min(indexed.cursor.sequence,ordinary.cursor.sequence)<target.sequence) {
        const auto after=indexed.cursor.sequence<=ordinary.cursor.sequence?indexed.cursor:ordinary.cursor;
        const auto page=source(after,static_cast<size_t>(std::min<uint64_t>(128,target.sequence-after.sequence)));
        Require(!page.events.empty(),"Wallet recovery source ended before captured head");
        for (const auto& event : page.events) {
            Require(event.cursor.sequence<=target.sequence,"Wallet recovery page exceeds captured head");
            const auto lease=wallet.AcquireDatabaseLease();
            const auto current=ReadStores(wallet,index,session);
            Require(Same(current.first,indexed) && Same(current.second,ordinary),
                "Wallet recovery stores changed during source read");
            // Index first, ordinary second. If the second commit fails, restart
            // reads the two actual receipts and applies only the lagging store.
            if (indexed.cursor.sequence<event.cursor.sequence)
                indexed=RuntimeIndexDelivery::ApplyForWallet(wallet,index,session,event);
            if (ordinary.cursor.sequence<event.cursor.sequence)
                ordinary=RuntimeOrdinaryDelivery::ApplyForWallet(wallet,session,event);
        }
    }
    Require(indexed.cursor==target && ordinary.cursor==target && Same(indexed,ordinary),
        "Wallet recovery captured head mismatch");
    const auto final_page=source(target,1);
    CheckPosition(indexed,final_page);
    {
        const auto current=ReadStores(wallet,index,session);
        Require(Same(current.first,indexed) && Same(current.second,ordinary),
            "Wallet recovery stores changed during source read");
    }
    // The chain may already have advanced. Return the checked observation, not
    // a lasting readiness claim or a published process-wide wallet height.
    return {indexed,final_page.head};
}

RuntimeWalletRecoveryResult RuntimeWalletRecovery::ResumeAccount(
        const RuntimeAccountReplay& view,const Source& source,WalletManager& wallet,
        UTXOIndex& index,uint64_t session,uint32_t account_number) {
    const auto result=ResumeAccounts(view,source,wallet,index,session,account_number);
    return {result.applied,result.account_revisions.front().second,result.observed_head};
}

RuntimeEnrolledWalletRecoveryResult RuntimeWalletRecovery::ResumeAccounts(
        const RuntimeAccountReplay& view,const Source& source,WalletManager& wallet,
        UTXOIndex& index,uint64_t session,std::optional<uint32_t> selected_account,bool require_catalog) {
    if(require_catalog){
        Require(!selected_account,"Catalog recovery cannot select a partial account inventory");
        return ResumePreparedCatalog(view,source,wallet,index,session);
    }
    using Account=wallet::OrchardAccountDelivery;
    Require(!require_catalog||!selected_account,"Catalog recovery cannot select a partial account inventory");
    {const auto lease=wallet.AcquireDatabaseLease();Require(wallet.database_leases_==1,
        "Wallet recovery requires released caller lease");}
    const auto first_handle=view.Event(1);const auto& first=*first_handle;const auto target=view.Head();
    struct Snapshot {RuntimeIndexProgress indexed,ordinary;std::vector<Account::Enrolled> accounts;std::optional<wallet::OrchardAccountCatalog::Snapshot> catalog;};
    const auto read=[&] {
        const auto lease=wallet.AcquireDatabaseLease();const auto stores=ReadStores(wallet,index,session);
        std::vector<Account::Enrolled> accounts;std::optional<wallet::OrchardAccountCatalog::Snapshot> catalog;
        if(require_catalog){
            auto owned=Account::ReadCatalogForReplay(wallet,session,view);
            accounts=std::move(owned.accounts);catalog=std::move(owned.catalog);
        }else if(selected_account){
            const Account::Profile profile{first.context.domain,first.context.activation_height,*selected_account};
            auto account=Account::ReadForReplay(wallet,session,profile,view);
            accounts.push_back({*selected_account,std::move(account)});
        }else accounts=Account::ReadEnrolledForReplay(wallet,session,view);
        return Snapshot{stores.first,stores.second,std::move(accounts),std::move(catalog)};
    };
    auto current=read();
    const auto account_cursor=[](const Account::Enrolled& s) {
        const auto& d=s.state.account.Delivery();return RuntimeOutboxCursor{d.sequence,d.digest};
    };
    const auto unchanged=[&](const Snapshot& a,const Snapshot& b) {
        if(!Same(a.indexed,b.indexed)||!Same(a.ordinary,b.ordinary)||a.accounts.size()!=b.accounts.size()||a.catalog!=b.catalog)return false;
        for(size_t i=0;i<a.accounts.size();++i)
            if(a.accounts[i].number!=b.accounts[i].number||a.accounts[i].state.revision!=b.accounts[i].state.revision||
               account_cursor(a.accounts[i])!=account_cursor(b.accounts[i])||
               a.accounts[i].archive_revisions!=b.accounts[i].archive_revisions)return false;
        return true;
    };
    const auto origin=view.Point({}).checkpoint;
    for(const auto* p:{&current.indexed,&current.ordinary}) {
        Require(p->origin_hash==origin.block_hash&&p->origin_height==origin.height,
            "Wallet recovery source origin mismatch");
        const auto point=view.Point(p->cursor).checkpoint;
        Require(p->tip_hash==point.block_hash&&p->tip_height==point.height,"Wallet recovery source position mismatch");
        CheckPosition(*p,source(p->cursor,1));
    }
    auto sequence=std::min(current.indexed.cursor.sequence,current.ordinary.cursor.sequence);
    // Validate every account, including an ahead account, before any effects.
    for(const auto& entry:current.accounts){
        const auto cursor=account_cursor(entry);const auto position=source(cursor,1);
        const auto& scan=entry.state.account.Scan().Checkpoint();
        if(cursor.sequence) {
            Require(position.after_tip&&position.after_tip->first==scan.block_hash&&position.after_tip->second==scan.height,
                "Wallet recovery account source position mismatch");
        } else {
            // A late account or explicit rescan has no applied event. Its full
            // authenticated scanner must be empty at this exact source origin;
            // bind the live source's FIRST event to the immutable replay view.
            // No cursor is assigned here. ApplyForReplay scans event 1 and
            // commits its real effects, receipt and retained parent together.
            Require(cursor.digest.IsNull()&&scan==origin&&!position.events.empty()&&
                position.events.front().cursor==first.cursor,
                "Wallet recovery account source origin mismatch");
        }
        sequence=std::min(sequence,cursor.sequence);
    }
    for(++sequence;sequence<=target.sequence;++sequence) {
        const auto event_handle=view.Event(sequence);const auto& event=*event_handle;
        const auto lease=wallet.AcquireDatabaseLease();
        Require(unchanged(current,read()),"Wallet recovery stores changed during source read");
        if(current.indexed.cursor.sequence<sequence)
            current.indexed=RuntimeIndexDelivery::ApplyForWallet(wallet,index,session,event);
        if(current.ordinary.cursor.sequence<sequence)
            current.ordinary=RuntimeOrdinaryDelivery::ApplyForWallet(wallet,session,event);
        // Accounts commit in ascending account-number order. A failure retains
        // all earlier store/account prefixes for the next owned recovery pass.
        for(auto& entry:current.accounts)if(account_cursor(entry).sequence<sequence) {
            const Account::Profile profile{first.context.domain,first.context.activation_height,entry.number};
            auto& account=entry.state;
            account=Account::ApplyForReplay(wallet,session,profile,account.revision,view,sequence);
            Require(account_cursor(entry)==event.cursor&&account.account.Scan().Checkpoint()==view.Point(event.cursor).checkpoint,
                "Wallet recovery account applied position mismatch");
        }
        // Every account committed this event: bound retained history (v8.2.4).
        (void)Account::BoundRetainedHistory(wallet,session,view);
    }
    Require(current.indexed.cursor==target&&current.ordinary.cursor==target&&Same(current.indexed,current.ordinary),
        "Wallet recovery captured head mismatch");
    for(const auto& entry:current.accounts)Require(account_cursor(entry)==target,"Wallet recovery captured head mismatch");
    // Also reconcile an already-applied prefix from an older owner. This has
    // no invented source movement; archive reservations commit with the actual
    // account revision, under the same complete-inventory recheck as delivery.
    {
        const auto lease=wallet.AcquireDatabaseLease();
        Require(unchanged(current,read()),"Wallet recovery stores changed during source read");
        for(auto& entry:current.accounts){
            const Account::Profile profile{first.context.domain,first.context.activation_height,entry.number};
            entry.state=Account::ReconcileForReplay(wallet,session,profile,entry.state.revision,view);
        }
    }
    const auto final=source(target,1);CheckPosition(current.indexed,final);
    Require(unchanged(current,read()),"Wallet recovery stores changed during source read");
    std::vector<std::pair<uint32_t,uint64_t>> revisions;
    for(const auto& entry:current.accounts)revisions.emplace_back(entry.number,entry.state.revision);
    return {current.indexed,std::move(revisions),final.head,current.catalog?std::optional<uint64_t>(current.catalog->revision):std::nullopt};
}
RuntimeEnrolledWalletRecoveryResult RuntimeWalletRecovery::ResumePreparedCatalog(
        const RuntimeAccountReplay& view,const Source& source,WalletManager& wallet,UTXOIndex& index,uint64_t session){
    using Account=wallet::OrchardAccountDelivery;using Action=Account::CatalogRecoveryAction;
    {const auto lease=wallet.AcquireDatabaseLease();Require(wallet.database_leases_==1,"Wallet recovery requires released caller lease");}
    struct ReadTransaction {
        sqlite3* db;bool done=false;
        explicit ReadTransaction(sqlite3* value):db(value){
            Require(db&&sqlite3_get_autocommit(db),"Wallet recovery requires unborrowed transaction");
            Require(sqlite3_exec(db,"PRAGMA synchronous=FULL",nullptr,nullptr,nullptr)==SQLITE_OK,"Wallet recovery durability unavailable");
            Require(sqlite3_exec(db,"BEGIN IMMEDIATE",nullptr,nullptr,nullptr)==SQLITE_OK,"Wallet recovery capture transaction unavailable");
        }
        ~ReadTransaction(){if(!done&&sqlite3_exec(db,"ROLLBACK",nullptr,nullptr,nullptr)!=SQLITE_OK&&!sqlite3_get_autocommit(db))std::terminate();}
        void Commit(){Require(sqlite3_exec(db,"COMMIT",nullptr,nullptr,nullptr)==SQLITE_OK,"Wallet recovery capture commit refused");done=true;}
    };
    struct Snapshot {RuntimeIndexProgress indexed,ordinary;Account::CatalogEnrolled catalog;};
    const auto cursor=[](const Account::Enrolled& entry){const auto& d=entry.state.account.Delivery();return RuntimeOutboxCursor{d.sequence,d.digest};};
    const auto equal=[&](const Snapshot& a,const Snapshot& b){
        if(!Same(a.indexed,b.indexed)||!Same(a.ordinary,b.ordinary)||a.catalog.catalog!=b.catalog.catalog||a.catalog.accounts.size()!=b.catalog.accounts.size())return false;
        for(size_t i=0;i<a.catalog.accounts.size();++i){const auto& x=a.catalog.accounts[i];const auto& y=b.catalog.accounts[i];
            if(x.number!=y.number||x.state.revision!=y.state.revision||cursor(x)!=cursor(y)||x.archive_revisions!=y.archive_revisions)return false;}
        return true;
    };
    const auto recheck=[&](const Snapshot& expected,const wallet::OrchardCatalogRecoveryPlan& plan){
        const auto lease=wallet.AcquireDatabaseLease();Require(lease->Session()==session,"Wallet recovery selection changed");
        // Each transparent store reader owns its transaction. Keep the process
        // wallet lease across those reads and the following catalog snapshot,
        // without borrowing a transaction into either reader.
        const auto stores=ReadStores(wallet,index,session);
        Require(Same(expected.indexed,stores.first)&&Same(expected.ordinary,stores.second),"Wallet recovery stores changed during preparation");
        ReadTransaction tx(lease->Database());
        Account::RecheckCatalogRecoveryInTransaction(wallet,session,plan);tx.Commit();
    };
    const auto prepare=[&](Action action,uint64_t sequence){
        const auto stores=ReadStores(wallet,index,session);
        auto plan=Account::PrepareCatalogRecovery(wallet,session,view,action,sequence);
        Snapshot observed{stores.first,stores.second,Account::CatalogRecoveryBefore(*plan)};
        recheck(observed,*plan);return std::pair{std::move(observed),std::move(plan)};
    };
    auto initial=prepare(Action::Observe,0);auto current=std::move(initial.first);initial.second.reset();
    const auto first=view.Event(1);const auto target=view.Head();const auto origin=view.Point({}).checkpoint;
    for(const auto* progress:{&current.indexed,&current.ordinary}){
        Require(progress->origin_hash==origin.block_hash&&progress->origin_height==origin.height,"Wallet recovery source origin mismatch");
        const auto point=view.Point(progress->cursor).checkpoint;
        Require(progress->tip_hash==point.block_hash&&progress->tip_height==point.height,"Wallet recovery source position mismatch");
        CheckPosition(*progress,source(progress->cursor,1));
    }
    auto sequence=std::min(current.indexed.cursor.sequence,current.ordinary.cursor.sequence);
    for(const auto& entry:current.catalog.accounts){
        const auto position=cursor(entry);const auto page=source(position,1);const auto& scan=entry.state.account.Scan().Checkpoint();
        if(position.sequence)Require(page.after_tip&&page.after_tip->first==scan.block_hash&&page.after_tip->second==scan.height,"Wallet recovery account source position mismatch");
        else Require(position.digest.IsNull()&&scan==origin&&!page.events.empty()&&page.events.front().cursor==first->cursor,"Wallet recovery account source origin mismatch");
        sequence=std::min(sequence,position.sequence);
    }
    for(++sequence;sequence<=target.sequence;++sequence){
        const auto event=view.Event(sequence);auto prepared=prepare(Action::Apply,sequence);
        Require(equal(current,prepared.first),"Wallet recovery catalog changed during source read");
        const auto lease=wallet.AcquireDatabaseLease();recheck(current,*prepared.second);
        if(current.indexed.cursor.sequence<sequence)current.indexed=RuntimeIndexDelivery::ApplyForWallet(wallet,index,session,*event);
        if(current.ordinary.cursor.sequence<sequence)current.ordinary=RuntimeOrdinaryDelivery::ApplyForWallet(wallet,session,*event);
        // Preserve independent ascending commits. Complete-catalog byte checks
        // advance only by this plan's exact retained/non-retained snapshot steps.
        for(size_t i=0;i<current.catalog.accounts.size();++i)if(cursor(current.catalog.accounts[i]).sequence<sequence)
            current.catalog.accounts[i].state=Account::CommitCatalogRecoveryAccount(wallet,session,*prepared.second,i);
        // After this event's plan fully committed, never inside it (v8.2.4).
        (void)Account::BoundRetainedHistory(wallet,session,view);
    }
    Require(current.indexed.cursor==target&&current.ordinary.cursor==target&&Same(current.indexed,current.ordinary),"Wallet recovery captured head mismatch");
    for(const auto& entry:current.catalog.accounts)Require(cursor(entry)==target,"Wallet recovery captured head mismatch");
    auto reconciled=prepare(Action::Reconcile,0);Require(equal(current,reconciled.first),"Wallet recovery catalog changed before reconciliation");
    {
        const auto lease=wallet.AcquireDatabaseLease();recheck(current,*reconciled.second);
        for(size_t i=0;i<current.catalog.accounts.size();++i)
            current.catalog.accounts[i].state=Account::CommitCatalogRecoveryAccount(wallet,session,*reconciled.second,i);
    }
    const auto final=source(target,1);CheckPosition(current.indexed,final);
    auto checked=prepare(Action::Observe,0);Require(equal(current,checked.first),"Wallet recovery stores changed during source read");
    std::vector<std::pair<uint32_t,uint64_t>> revisions;revisions.reserve(current.catalog.accounts.size());
    for(const auto& entry:current.catalog.accounts)revisions.emplace_back(entry.number,entry.state.revision);
    return {current.indexed,std::move(revisions),final.head,current.catalog.catalog.revision};
}
std::optional<RuntimeOrchardCatalogRecoveryResult> RuntimeWalletRecovery::ResumePreparedOrchardCatalog(
        const RuntimeAccountReplay& view,const Source& source,WalletManager& wallet,uint64_t session,
        const OrchardSourceOwner& bind,size_t max_events){
    using Account=wallet::OrchardAccountDelivery;using Action=Account::CatalogRecoveryAction;
    Require(bool(bind)&&max_events,"Orchard recovery source owner or slice limit unavailable");
    {const auto lease=wallet.AcquireDatabaseLease();Require(wallet.database_leases_==1,
        "Orchard recovery requires released caller lease");}
    const auto target=view.Head();Require(target.sequence,"Orchard recovery requires genuine first delivery");
    const auto first=view.Event(1);const auto origin=view.Point({}).checkpoint;
    // An idle pass (the worker returns about every second) re-proved the same
    // catalog at the same head: about six complete captures, ~1 s each on a
    // mainnet wallet, holding the retained replay that wallet reads wait for.
    // When nothing was written to the wallet database since the last complete
    // pass proved every account at this head, return that pass's result. The
    // source position is still proven below on every pass.
    const auto generation=Account::CatalogRecoveryGeneration();
    auto& idle=IdleCatalog();
    std::optional<IdleCatalogKey> last;
    {std::lock_guard<std::mutex> lock(idle.mutex);last=idle.key;}
    bool unchanged=false;
    if(last){
        const auto lease=wallet.AcquireDatabaseLease();
        unchanged=last->wallet==&wallet&&last->session==session&&lease->Session()==session&&
            lease->InstanceToken()==last->instance&&lease->Database()&&lease->Database()==last->db&&
            last->generation==generation&&
            last->head==target&&last->origin==origin&&
            sqlite3_total_changes64(lease->Database())==last->total_changes&&
            DataVersion(lease->Database())==last->data_version;
    }
    const auto now=std::chrono::steady_clock::now();
    if(unchanged&&now-last->full_at<std::chrono::milliseconds(idle.interval_ms.load())){
        const auto final=source(target,1);const auto point=view.Point(target).checkpoint;
        Require(final.after_tip&&final.after_tip->first==point.block_hash&&final.after_tip->second==point.height,
            "Orchard recovery final source position mismatch");
        auto result=last->result;result.observed_head=final.head;
        if(const auto n=idle.skipped.fetch_add(1)+1;n==1||n%1000==0)
            std::cerr<<"[OrchardDelivery] idle catalog passes skipped: "<<n<<std::endl;
        return result;
    }
    std::optional<std::pair<sqlite3_int64,int64_t>> stamp;bool stamp_next=false;std::shared_ptr<const void> instance;
    const sqlite3* stamp_db=nullptr;
    const auto cursor=[](const Account::Enrolled& entry){const auto& d=entry.state.account.Delivery();
        return RuntimeOutboxCursor{d.sequence,d.digest};};
    const auto equal=[&](const Account::CatalogEnrolled& a,const Account::CatalogEnrolled& b){
        if(a.catalog!=b.catalog||a.accounts.size()!=b.accounts.size())return false;
        for(size_t i=0;i<a.accounts.size();++i){const auto& x=a.accounts[i];const auto& y=b.accounts[i];
            if(x.number!=y.number||x.state.revision!=y.state.revision||cursor(x)!=cursor(y)||
               x.archive_revisions!=y.archive_revisions)return false;}
        return true;
    };
    struct CaptureTransaction {
        sqlite3* db;bool done=false;
        explicit CaptureTransaction(sqlite3* value):db(value){
            Require(db&&sqlite3_get_autocommit(db),"Orchard recovery requires unborrowed transaction");
            Require(sqlite3_exec(db,"PRAGMA synchronous=FULL",nullptr,nullptr,nullptr)==SQLITE_OK,
                "Orchard recovery durability unavailable");
            Require(sqlite3_exec(db,"BEGIN IMMEDIATE",nullptr,nullptr,nullptr)==SQLITE_OK,
                "Orchard recovery capture transaction unavailable");
        }
        ~CaptureTransaction(){if(!done&&sqlite3_exec(db,"ROLLBACK",nullptr,nullptr,nullptr)!=SQLITE_OK&&
            !sqlite3_get_autocommit(db))std::terminate();}
        void Commit(){Require(sqlite3_exec(db,"COMMIT",nullptr,nullptr,nullptr)==SQLITE_OK,
            "Orchard recovery capture commit refused");done=true;}
    };
    const auto recheck=[&](const wallet::OrchardCatalogRecoveryPlan& plan,const std::function<void()>& fence){
        const auto lease=wallet.AcquireDatabaseLease();Require(lease->Session()==session,
            "Orchard recovery selection changed");
        CaptureTransaction tx(lease->Database());Account::RecheckCatalogRecoveryInTransaction(wallet,session,plan);
        fence();
        // The final read-only recheck holds the write lock: nothing can land
        // between what it proved and the database position recorded here.
        if(stamp_next){stamp.emplace(sqlite3_total_changes64(lease->Database()),DataVersion(lease->Database()));
            instance=lease->InstanceToken();stamp_db=lease->Database();}
        tx.Commit();
    };
    const auto prepare=[&](Action action,uint64_t sequence){
        // Complete current/retained/pending/archive restoration and real event
        // application happen OFF source, wallet, seed and SQLite ownership.
        auto plan=Account::PrepareCatalogRecovery(wallet,session,view,action,sequence);
        auto before=Account::CatalogRecoveryBefore(*plan);
        bind([&](const auto& fence){recheck(*plan,fence);});
        return std::pair{std::move(before),std::move(plan)};
    };
    auto initial=prepare(Action::Observe,0);auto current=std::move(initial.first);initial.second.reset();
    auto sequence=target.sequence;
    for(const auto& entry:current.accounts){
        const auto position=cursor(entry);const auto page=source(position,1);
        const auto& scan=entry.state.account.Scan().Checkpoint();
        if(position.sequence)Require(page.after_tip&&page.after_tip->first==scan.block_hash&&
            page.after_tip->second==scan.height,"Orchard recovery account source position mismatch");
        else Require(position.digest.IsNull()&&scan==origin&&!page.after_tip&&!page.events.empty()&&
            page.events.front().cursor==first->cursor,"Orchard recovery account source origin mismatch");
        Require(position.sequence<=target.sequence,"Orchard recovery account exceeds captured head");
        sequence=std::min(sequence,position.sequence);
    }
    size_t advanced=0;
    while(sequence<target.sequence && advanced<max_events){
        ++sequence;const auto event=view.Event(sequence);auto prepared=prepare(Action::Apply,sequence);
        Require(equal(current,prepared.first),"Orchard recovery catalog changed during preparation");
        bind([&](const auto& fence){
            // Keep the same wallet selection across independent ascending FULL
            // account commits. A refusal retains every already-earned prefix.
            const auto lease=wallet.AcquireDatabaseLease();recheck(*prepared.second,fence);
            for(size_t i=0;i<current.accounts.size();++i)if(cursor(current.accounts[i]).sequence<sequence){
                current.accounts[i].state=Account::CommitCatalogRecoveryAccount(wallet,session,*prepared.second,i,fence);
                Require(cursor(current.accounts[i])==event->cursor&&
                    current.accounts[i].state.account.Scan().Checkpoint()==view.Point(event->cursor).checkpoint,
                    "Orchard recovery applied position mismatch");
            }
        });
        // After this event's plan fully committed, under the same selected
        // fence, bound retained history (v8.2.4). Never inside the plan.
        bind([&](const auto& fence){(void)Account::BoundRetainedHistory(wallet,session,view);fence();});
        // Carry the committed catalog into the next event instead of restoring
        // the wallet's whole inventory again (~12 s per event on mainnet).
        // Remembered only if the stored bytes are exactly this catalog.
        try{Account::RememberCatalogRecovery(wallet,session,view,current);}
        catch(const std::exception&){Account::ForgetCatalogRecovery();}
        ++advanced;
    }
    // Each applied event is a real, independently fenced durable account
    // commit. Preserve its receipt for the next pass, but never reconcile or
    // report the captured head until every account actually reaches it.
    if(sequence<target.sequence)return std::nullopt;
    for(const auto& entry:current.accounts)Require(cursor(entry)==target,"Orchard recovery captured head mismatch");
    auto reconciled=prepare(Action::Reconcile,0);
    Require(equal(current,reconciled.first),"Orchard recovery catalog changed before reconciliation");
    bind([&](const auto& fence){
        const auto lease=wallet.AcquireDatabaseLease();recheck(*reconciled.second,fence);
        for(size_t i=0;i<current.accounts.size();++i)
            current.accounts[i].state=Account::CommitCatalogRecoveryAccount(wallet,session,*reconciled.second,i,fence);
    });
    const auto final=source(target,1);const auto point=view.Point(target).checkpoint;
    Require(final.after_tip&&final.after_tip->first==point.block_hash&&final.after_tip->second==point.height,
        "Orchard recovery final source position mismatch");
    stamp_next=true;auto checked=prepare(Action::Observe,0);stamp_next=false;
    Require(equal(current,checked.first),"Orchard recovery stores changed before return");
    std::vector<std::pair<uint32_t,uint64_t>> revisions;revisions.reserve(current.accounts.size());
    for(const auto& entry:current.accounts)revisions.emplace_back(entry.number,entry.state.revision);
    // No ReadStores/coverage/baseline adoption or transparent/index write occurs
    // on this distinct route. Even success is ONLY this captured Orchard prefix.
    RuntimeOrchardCatalogRecoveryResult result{target,std::move(revisions),final.head,
        current.catalog.revision};
    // The forced complete pass is also the skip's check: had the key matched,
    // this pass would have been skipped, so it must have found nothing new.
    if(unchanged&&!SameResult(result,last->result)){
        idle.misses.fetch_add(1);
        std::cerr<<"[OrchardDelivery] an idle catalog pass that would have been skipped found a change; "
                   "the wallet write that made it did not move the skip key"<<std::endl;
    }
    if(stamp){
        IdleCatalogKey key;key.wallet=&wallet;key.instance=instance;key.db=stamp_db;key.session=session;key.generation=generation;
        key.head=target;key.origin=origin;key.total_changes=stamp->first;key.data_version=stamp->second;
        key.result=result;key.full_at=now;
        std::lock_guard<std::mutex> lock(idle.mutex);idle.key=std::move(key);
    }
    return result;
}

// ---------------------------------------------------------------------------
// Public entry points over the R1 recovery source (Task 6.3, R2/R3).
// The lease precondition, the CoverageRequiredForWallet decision and the
// !ok() -> runtime_error mapping moved here from the node adapter with the
// same checks and the same exception messages.
// ---------------------------------------------------------------------------
RuntimeTransparentRecoveryResult RuntimeWalletRecovery::ResumeTransparentStores(
        const wallet::IRuntimeRecoverySource& chain, WalletManager& wallet,
        UTXOIndex& index, uint64_t session) {
    return Resume([&chain](RuntimeOutboxCursor cursor,size_t count) {
        const auto page=chain.GetDeliveryPage(cursor,count);
        if (!page.ok()) throw std::runtime_error("Wallet recovery checked source unavailable");
        return **page;
    },wallet,index,session);
}

RuntimeWalletRecoveryResult RuntimeWalletRecovery::ResumeWalletStores(
        const wallet::IRuntimeRecoverySource& chain,WalletManager& wallet,
        UTXOIndex& index,uint64_t session,uint32_t account) {
    {const auto lease=wallet.AcquireDatabaseLease();
     if(wallet.database_leases_!=1)throw std::runtime_error("Wallet recovery requires released caller lease");}
    const auto view=chain.GetAccountReplay();
    if(!view.ok())throw std::runtime_error("Wallet recovery checked account source unavailable");
    return ResumeAccount(**view,[&chain](RuntimeOutboxCursor cursor,size_t count){
        const auto page=chain.GetDeliveryPage(cursor,count);
        if(!page.ok())throw std::runtime_error("Wallet recovery checked source unavailable");
        return **page;
    },wallet,index,session,account);
}

RuntimeEnrolledWalletRecoveryResult RuntimeWalletRecovery::ResumeEnrolledWalletStores(
        const wallet::IRuntimeRecoverySource& chain,WalletManager& wallet,
        UTXOIndex& index,uint64_t session) {
    {const auto lease=wallet.AcquireDatabaseLease();
     if(wallet.database_leases_!=1)throw std::runtime_error("Wallet recovery requires released caller lease");}
    const auto view=chain.GetAccountReplay();
    if(!view.ok())throw std::runtime_error("Wallet recovery checked account source unavailable");
    return ResumeAccounts(**view,[&chain](RuntimeOutboxCursor cursor,size_t count){
        const auto page=chain.GetDeliveryPage(cursor,count);
        if(!page.ok())throw std::runtime_error("Wallet recovery checked source unavailable");
        return **page;
    },wallet,index,session,std::nullopt);
}

RuntimeEnrolledWalletRecoveryResult RuntimeWalletRecovery::ResumeCatalogWalletStores(
        const wallet::IRuntimeRecoverySource& chain,WalletManager& wallet,
        UTXOIndex& index,uint64_t session) {
    {const auto lease=wallet.AcquireDatabaseLease();
     if(wallet.database_leases_!=1)throw std::runtime_error("Wallet recovery requires released caller lease");}
    if(RuntimeOrdinaryDelivery::CoverageRequiredForWallet(wallet,index,session)) {
        const auto coverage=chain.GetWalletCoverage(wallet,session,index);
        // Keep the material limit typed: callers stop the session on it
        // instead of retrying an over-limit capture forever.
        if(coverage.status()==Status::ResourceLimit)throw RuntimeWalletCoverageMaterialLimit();
        if(!coverage.ok())throw std::runtime_error("Wallet recovery coverage source unavailable");
        if(chain.ReconcileWalletCoverage(wallet,index,**coverage)!=Status::Ok)
            throw std::runtime_error("Wallet recovery coverage reconciliation refused");
    }
    const auto view=chain.GetAccountReplay();
    if(!view.ok())throw std::runtime_error("Wallet recovery checked account source unavailable");
    return ResumeAccounts(**view,[&chain](RuntimeOutboxCursor cursor,size_t count){
        const auto page=chain.GetDeliveryPage(cursor,count);
        if(!page.ok())throw std::runtime_error("Wallet recovery checked source unavailable");
        return **page;
    },wallet,index,session,std::nullopt,true);
}

RuntimeOrchardCatalogRecoveryResult RuntimeWalletRecovery::ResumePreparedOrchardCatalogStores(
        const RuntimeAccountReplay& replay,const wallet::IRuntimeRecoverySource& chain,
        WalletManager& wallet,uint64_t session,const OrchardSourceOwner& owner) {
    auto result=AdvancePreparedOrchardCatalogStores(replay,chain,wallet,session,owner,
        std::numeric_limits<size_t>::max());
    Require(result.has_value(),"Orchard recovery did not reach captured head");
    return std::move(*result);
}

std::optional<RuntimeOrchardCatalogRecoveryResult>
RuntimeWalletRecovery::AdvancePreparedOrchardCatalogStores(
        const RuntimeAccountReplay& replay,const wallet::IRuntimeRecoverySource& chain,
        WalletManager& wallet,uint64_t session,const OrchardSourceOwner& owner,size_t max_events) {
    return ResumePreparedOrchardCatalog(replay,[&chain](RuntimeOutboxCursor cursor,size_t count){
        const auto page=chain.GetDeliveryPage(cursor,count);
        if(!page.ok())throw std::runtime_error("Imported Orchard delivery page unavailable");
        return **page;
    },wallet,session,owner,max_events);
}

} // namespace dinero
