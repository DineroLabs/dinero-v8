#include "wallet/runtime_wallet_recovery.h"
#include "wallet/wallet_manager.h"
#include <algorithm>
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
} // namespace dinero
