#pragma once
#include "wallet/runtime_index_delivery.h"
#include "wallet/runtime_wallet_recovery.h"
#include "wallet/utxo_index.h"
#include "wallet/wallet_manager.h"
#include <future>
#include <chrono>
#include <thread>
namespace dinero {
struct RuntimeWalletRecoveryTestAccess {
 static auto ResumeEnrolled(const RuntimeAccountReplay& view,const std::function<RuntimeOutboxPage(RuntimeOutboxCursor,size_t)>& source,WalletManager& w,UTXOIndex& i,uint64_t session){return RuntimeWalletRecovery::ResumeAccounts(view,source,w,i,session,std::nullopt);}
 static auto Resume(const std::function<RuntimeOutboxPage(RuntimeOutboxCursor,size_t)>& source,WalletManager& w,UTXOIndex& i,uint64_t session){return RuntimeWalletRecovery::Resume(source,w,i,session);}
 static auto ResumeAccount(const RuntimeAccountReplay& view,const std::function<RuntimeOutboxPage(RuntimeOutboxCursor,size_t)>& source,WalletManager& w,UTXOIndex& i,uint64_t session){return RuntimeWalletRecovery::ResumeAccount(view,source,w,i,session,0);}
};
struct RuntimeIndexDeliveryTestAccess {
 static auto Read(dinero::UTXOIndex& i,const std::string& id){return RuntimeIndexDelivery::Read(i,id);}
 static auto Apply(dinero::UTXOIndex& i,const std::string& id,const RuntimeOutboxEvent& e){return RuntimeIndexDelivery::Apply(i,id,e);}
};
}

static void IndexDeliveryChecks(ChainDB& db,const OrchardBlockContext& c,
    const OrchardBlockCandidate& block,const UtreexoForest& forest,const std::filesystem::path& path) {
    TempDir wallet;const auto file=(wallet.path/"index.sqlite").string();
    dinero::UTXOIndex index(file);CHECK(index.Initialize());
    auto historical=HistoricalDeliveryParent();historical.header=RequiredValue(db.getHeader(c.parent_hash));
    CHECK(historical.GetHash()==c.parent_hash);
    const auto register_scripts=[&](dinero::UTXOIndex& target) {
        target.RegisterAddress(historical.vtx.front().vout.front().scriptPubKey,"m/86'/1448'/0'/0/0");
        for(const auto& tx:block.Transactions()) {
            if(tx.IsOrchard())for(const auto& output:tx.Orchard().Outputs())target.RegisterAddress(output.script_pub_key,"m/86'/1448'/0'/0/0");
            else for(const auto& output:tx.Historical().vout)target.RegisterAddress(output.scriptPubKey,"m/86'/1448'/0'/0/0");
        }
    };register_scripts(index);
    const auto undo=RequiredValue(db.getUndo(c.block_hash));
    size_t baseline=0;
    for(const auto& spent:undo.spent)if(spent.height<c.height) {
        CHECK(index.AddUTXO(WalletUTXO(TxId(spent.prev_txid),spent.prev_vout,AmountUna::Una(spent.value),spent.scriptPubKey,"m/86'/1448'/0'/0/0",spent.height,spent.is_coinbase)));++baseline;
    }
    const auto& old_output=historical.vtx.front().vout.front();
    CHECK(index.AddUTXO(WalletUTXO(historical.vtx.front().GetTxid(),0,old_output.value,old_output.scriptPubKey,"m/86'/1448'/0'/0/0",c.height-1,true)));++baseline;
    const auto failure=[&](const std::function<void()>& f,const char* text) {
        bool refused=false;try{f();}catch(const std::exception& e){refused=std::string(e.what()).find(text)!=std::string::npos;if(!refused)std::cerr<<"Expected "<<text<<", got "<<e.what()<<"\n";}if(!refused)std::cerr<<"Expected failure containing: "<<text<<"\n";CHECK(refused);
    };
    AnnotatedRecursiveMutex lock;std::lock_guard<AnnotatedRecursiveMutex> guard(lock);
    const auto event=ReadRuntimeOutboxUnderLock(db,c).events.front();CHECK(event.cursor.sequence==1);
    WalletManager ordinary(wallet.path/"ordinary");ordinary.create("ordinary");
    const std::array<uint8_t,64> account_seed{7};
    CHECK(ordinary.storeMasterSeed(std::vector<uint8_t>(account_seed.begin(),account_seed.end()),"",false));
    auto ordinary_session=ordinary.AcquireDatabaseLease()->Session();
    const auto wallet_identity=ordinary.AcquireDatabaseLease()->EnsureDeliveryIdentity();
    const auto ordinary_sql=[&](const std::string& text) {
        const auto lease=ordinary.AcquireDatabaseLease();
        CHECK(sqlite3_exec(lease->Database(),text.c_str(),nullptr,nullptr,nullptr)==SQLITE_OK);
    };
    const auto ordinary_count=[&](const char* text) {
        const auto lease=ordinary.AcquireDatabaseLease();sqlite3_stmt* stmt=nullptr;
        CHECK(sqlite3_prepare_v2(lease->Database(),text,-1,&stmt,nullptr)==SQLITE_OK);
        CHECK(sqlite3_step(stmt)==SQLITE_ROW);const auto value=sqlite3_column_int64(stmt,0);
        CHECK(sqlite3_step(stmt)==SQLITE_DONE);sqlite3_finalize(stmt);return value;
    };
    const auto script_hex=[](const auto& bytes){std::string out;constexpr char digits[]="0123456789abcdef";for(auto b:bytes){out+=digits[b>>4];out+=digits[b&15];}return out;};
    const auto watch=[&](const auto& bytes) {ordinary_sql("INSERT OR IGNORE INTO watch_scripts(script_pubkey,path) VALUES(X'"+script_hex(bytes)+"','delivery-fixture')");};
    watch(old_output.scriptPubKey);
    for(const auto& tx:block.Transactions()) {
        if(tx.IsOrchard())for(const auto& out:tx.Orchard().Outputs())watch(out.script_pub_key);
        else for(const auto& out:tx.Historical().vout)watch(out.scriptPubKey);
    }
    for(const auto& spent:undo.spent)if(spent.height<c.height)
        CHECK(ordinary.addUTXO(spent.prev_txid.GetHex(),spent.prev_vout,spent.value,"",script_hex(spent.scriptPubKey),spent.height,spent.is_coinbase));
    CHECK(ordinary.addUTXO(historical.vtx.front().GetTxid().AsUint256().GetHex(),0,old_output.value.GetInt64(),"",script_hex(old_output.scriptPubKey),c.height-1,true));
    CHECK(ordinary_count("SELECT count(*) FROM utxos")==int64_t(baseline));
    CHECK(!RuntimeOrdinaryDelivery::ReadForWallet(ordinary,ordinary_session));
    size_t recovery_source_reads=0;
    const auto source=[&](RuntimeOutboxCursor cursor,size_t maximum) {
        CHECK(maximum>0 && maximum<=128);++recovery_source_reads;
        return ReadRuntimeOutboxUnderLock(db,c,cursor,maximum);
    };
    const auto recover=[&] {return RuntimeWalletRecoveryTestAccess::Resume(source,ordinary,index,ordinary_session);};
    failure([&]{(void)recover();},"baseline reconciliation required");
    CHECK(recovery_source_reads==0);
    // Failing effects roll back schema preparation, rows and source progress.
    ordinary_sql("CREATE TRIGGER ordinary_reject_output BEFORE INSERT ON utxos BEGIN SELECT RAISE(ABORT,'ordinary output'); END");
    failure([&]{RuntimeOrdinaryDelivery::ApplyForWallet(ordinary,ordinary_session,event);},"statement failed");
    CHECK(!RuntimeOrdinaryDelivery::ReadForWallet(ordinary,ordinary_session));
    CHECK(ordinary_count("SELECT count(*) FROM utxos")==int64_t(baseline));
    CHECK(ordinary_count("SELECT count(*) FROM utxos WHERE is_spent=1")==0);
    CHECK(ordinary_count("SELECT count(*) FROM pragma_table_info('wallet_meta') WHERE name='runtime_ordinary_receipt'")==0);
    ordinary_sql("DROP TRIGGER ordinary_reject_output");
    ordinary_sql("ALTER TABLE wallet_meta ADD COLUMN runtime_ordinary_receipt BLOB; ALTER TABLE wallet_meta ADD COLUMN runtime_ordinary_invalid INTEGER NOT NULL DEFAULT 0");
    ordinary_sql("CREATE TRIGGER ordinary_reject_receipt BEFORE UPDATE OF runtime_ordinary_receipt ON wallet_meta WHEN NEW.runtime_ordinary_receipt IS NOT NULL BEGIN SELECT RAISE(ABORT,'ordinary receipt'); END");
    failure([&]{RuntimeOrdinaryDelivery::ApplyForWallet(ordinary,ordinary_session,event);},"statement failed");
    CHECK(!RuntimeOrdinaryDelivery::ReadForWallet(ordinary,ordinary_session));
    CHECK(ordinary_count("SELECT count(*) FROM utxos")==int64_t(baseline));
    ordinary_sql("DROP TRIGGER ordinary_reject_receipt");
    ordinary_sql("PRAGMA foreign_keys=ON; CREATE TABLE ordinary_parent(id INTEGER PRIMARY KEY); CREATE TABLE ordinary_child(id INTEGER REFERENCES ordinary_parent(id) DEFERRABLE INITIALLY DEFERRED); CREATE TRIGGER ordinary_reject_commit AFTER UPDATE OF runtime_ordinary_receipt ON wallet_meta WHEN NEW.runtime_ordinary_receipt IS NOT NULL BEGIN INSERT INTO ordinary_child VALUES(9); END");
    failure([&]{RuntimeOrdinaryDelivery::ApplyForWallet(ordinary,ordinary_session,event);},"SQL failed");
    CHECK(!RuntimeOrdinaryDelivery::ReadForWallet(ordinary,ordinary_session));
    CHECK(ordinary_count("SELECT count(*) FROM utxos")==int64_t(baseline));
    CHECK(ordinary_count("SELECT count(*) FROM transactions")==0);
    CHECK(ordinary_count("SELECT count(*) FROM utxos WHERE is_spent=1")==0);
    ordinary_sql("DROP TRIGGER ordinary_reject_commit");
    { const auto lease=ordinary.AcquireDatabaseLease();
      CHECK(sqlite3_exec(lease->Database(),"BEGIN IMMEDIATE",nullptr,nullptr,nullptr)==SQLITE_OK);
      failure([&]{RuntimeOrdinaryDelivery::ApplyForWallet(ordinary,ordinary_session,event);},"identity ownership unavailable");
      CHECK(!sqlite3_get_autocommit(lease->Database()));
      CHECK(sqlite3_exec(lease->Database(),"ROLLBACK",nullptr,nullptr,nullptr)==SQLITE_OK); }

    CHECK(!RuntimeIndexDeliveryTestAccess::Read(index,wallet_identity));
    // The production entry points derive identity from a real pinned wallet.
    { WalletManager owner(wallet.path/"manager");owner.create("delivery");owner.open("delivery");
      dinero::UTXOIndex bound((wallet.path/"bound.sqlite").string());CHECK(bound.Initialize());
      auto session=owner.AcquireDatabaseLease()->Session();
      CHECK(!RuntimeIndexDelivery::ReadForWallet(owner,bound,session));
      CHECK(RuntimeIndexDelivery::ApplyForWallet(owner,bound,session,event).cursor==event.cursor);
      owner.open("delivery");
      failure([&]{(void)RuntimeIndexDelivery::ReadForWallet(owner,bound,session);},"selection changed");
      failure([&]{RuntimeIndexDelivery::ApplyForWallet(owner,bound,session,event);},"selection changed");
      session=owner.AcquireDatabaseLease()->Session();
      CHECK(RuntimeIndexDelivery::ReadForWallet(owner,bound,session)->cursor==event.cursor);
      CHECK(RuntimeIndexDelivery::ApplyForWallet(owner,bound,session,event).cursor==event.cursor);
      { WalletManager namesake(wallet.path/"namesake");namesake.create("delivery");
        const auto other_session=namesake.AcquireDatabaseLease()->Session();
        failure([&]{(void)RuntimeIndexDelivery::ReadForWallet(namesake,bound,other_session);},"ownership changed");
        failure([&]{RuntimeIndexDelivery::ApplyForWallet(namesake,bound,other_session,event);},"ownership changed"); }
      owner.create("other");owner.open("delivery");session=owner.AcquireDatabaseLease()->Session();
      // Hold the real index owner while the production reader waits for it.
      // Its wallet lease must remain pinned for that entire wait.
      std::future<std::optional<RuntimeIndexProgress>> reader;
      std::future<void> switcher;
      std::promise<void> switch_started, switch_acquired;
      auto started=switch_started.get_future();auto acquired=switch_acquired.get_future();
      bool pinned=false, switch_blocked=false;
      auto* sqlite_mutex=sqlite3_db_mutex(owner.getCurrentDatabase());
      bound.ApplyAtomically([&] {
          reader=std::async(std::launch::async,[&]{return RuntimeIndexDelivery::ReadForWallet(owner,bound,session);});
          const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(3);
          do {
              const int rc=sqlite3_mutex_try(sqlite_mutex);
              if(rc==SQLITE_BUSY){pinned=true;break;}
              CHECK(rc==SQLITE_OK);sqlite3_mutex_leave(sqlite_mutex);
              std::this_thread::yield();
          } while(std::chrono::steady_clock::now()<deadline);
          switcher=std::async(std::launch::async,[&]{
              switch_started.set_value();
              { auto competing_lease=owner.AcquireDatabaseLease();switch_acquired.set_value(); }
              owner.open("other");
          });
          started.wait();
          switch_blocked=acquired.wait_for(std::chrono::milliseconds(100))==std::future_status::timeout;
      });
      CHECK(reader.get()->cursor==event.cursor);switcher.get();
      CHECK(pinned && switch_blocked);
      failure([&]{RuntimeIndexDelivery::ApplyForWallet(owner,bound,session,event);},"selection changed");
      session=owner.AcquireDatabaseLease()->Session();
      failure([&]{(void)RuntimeIndexDelivery::ReadForWallet(owner,bound,session);},"ownership changed");
      failure([&]{RuntimeIndexDelivery::ApplyForWallet(owner,bound,session,event);},"ownership changed"); }

    sqlite3* raw=nullptr;CHECK(sqlite3_open(file.c_str(),&raw)==SQLITE_OK);
    struct Close {sqlite3* db;~Close(){sqlite3_close(db);}} close{raw};
    const auto sql=[&](const char* text){CHECK(sqlite3_exec(raw,text,nullptr,nullptr,nullptr)==SQLITE_OK);};
    // A receipt write failure must roll back every input/output effect.
    sql("CREATE TRIGGER reject_receipt BEFORE INSERT ON utxo_metadata WHEN NEW.key='runtime_delivery:v1:receipt' BEGIN SELECT RAISE(ABORT,'receipt failure'); END");
    failure([&]{RuntimeIndexDeliveryTestAccess::Apply(index,wallet_identity,event);},"Index delivery statement failed");
    CHECK(!RuntimeIndexDeliveryTestAccess::Read(index,wallet_identity) && index.GetUTXOCount().value()==baseline);
    for(const auto& spent:undo.spent)if(spent.height<c.height)CHECK(!index.GetUTXO(TxId(spent.prev_txid),spent.prev_vout)->spend_height);
    CHECK(!index.GetUTXO(TxId(H(250)),UINT32_MAX)); // Finish the reusable SELECT before external schema DDL.
    sql("DROP TRIGGER reject_receipt");
    // Required row errors and deferred COMMIT rejection cannot advance progress.
    sql("CREATE TRIGGER reject_output BEFORE INSERT ON wallet_utxos BEGIN SELECT RAISE(ABORT,'output failure'); END");
    failure([&]{RuntimeIndexDeliveryTestAccess::Apply(index,wallet_identity,event);},"Index delivery output failed");
    CHECK(!RuntimeIndexDeliveryTestAccess::Read(index,wallet_identity) && index.GetUTXOCount().value()==baseline);
    sql("DROP TRIGGER reject_output");
    sql("CREATE TABLE delivery_parent(id INTEGER PRIMARY KEY); CREATE TABLE delivery_child(id INTEGER REFERENCES delivery_parent(id) DEFERRABLE INITIALLY DEFERRED); CREATE TRIGGER reject_commit AFTER INSERT ON utxo_metadata WHEN NEW.key='runtime_delivery:v1:receipt' BEGIN INSERT INTO delivery_child VALUES(99); END");
    failure([&]{RuntimeIndexDeliveryTestAccess::Apply(index,wallet_identity,event);},"Wallet UTXO write commit failed");
    CHECK(!RuntimeIndexDeliveryTestAccess::Read(index,wallet_identity) && index.GetUTXOCount().value()==baseline);
    for(const auto& spent:undo.spent)if(spent.height<c.height)CHECK(!index.GetUTXO(TxId(spent.prev_txid),spent.prev_vout)->spend_height);
    CHECK(!index.GetUTXO(TxId(H(250)),UINT32_MAX));
    sql("DROP TRIGGER reject_commit");
    CHECK(index.BeginTransaction() && index.SetMetadata("borrowed","pending"));
    failure([&]{RuntimeIndexDeliveryTestAccess::Apply(index,wallet_identity,event);},"write ownership unavailable");
    CHECK(index.GetMetadata("borrowed")==std::optional<std::string>("pending"));
    CHECK(index.RollbackTransaction() && !index.GetMetadata("borrowed"));
    const auto first=RuntimeIndexDeliveryTestAccess::Apply(index,wallet_identity,event);
    CHECK(first.cursor==event.cursor && first.tip_hash==c.block_hash && first.origin_hash==c.parent_hash);
    CHECK(RuntimeIndexDeliveryTestAccess::Apply(index,wallet_identity,event).cursor==first.cursor);
    const auto count=index.GetUTXOCount().value();CHECK(count>baseline);
    // Index may commit first; ordinary progress remains absent until its own
    // effects commit. Retry from the same checked event closes that prefix.
    CHECK(!RuntimeOrdinaryDelivery::ReadForWallet(ordinary,ordinary_session));
    CHECK(RuntimeOrdinaryDelivery::ApplyForWallet(ordinary,ordinary_session,event).cursor==first.cursor);
    using Account=dinero::wallet::OrchardAccountDelivery;
    const Account::Profile account_profile{c.domain,c.activation_height,0};
    const auto account_view=RuntimeAccountReplayTestAccess::Capture(db,c);
    auto next_receiver=([&]{
        const auto lease=ordinary.AcquireDatabaseLease();const auto secret=lease->CopyRecoverySeed(ordinary_session);
        const auto keys=orchard::WalletKeys::FromSeed(secret->Bytes(),0);
        const auto initial=dinero::wallet::OrchardAccountState::Begin(c.domain,keys.ExportFullViewingKey(),c.activation_height,c.parent_hash);
        auto issued=initial.IssueReceiver(orchard::WalletScope::External).first;
        orchard::Hash identity{};for(size_t i=0;i<32;++i)identity[i]=uint8_t(std::stoul(wallet_identity.substr(7+2*i,2),nullptr,16));
        CHECK(sqlite3_exec(lease->Database(),"BEGIN IMMEDIATE",nullptr,nullptr,nullptr)==SQLITE_OK);
        orchard::WalletSnapshotStore::InitializeSchemaUnderTransaction(lease->Database());
        orchard::WalletSnapshotStore store(lease->Database(),{orchard::WalletNetwork::Regtest,c.domain.genesis_wire,identity,0},secret->Bytes());
        CHECK(store.StageReplaceRetaining(0,issued.Encode())==1);
        CHECK(sqlite3_exec(lease->Database(),"COMMIT",nullptr,nullptr,nullptr)==SQLITE_OK);
        return issued.IssueReceiver(orchard::WalletScope::External).second.Raw();
    })();
    // Explicit fixture enrollment above is not a production baseline proof.
    auto account_first=Account::Connect(ordinary,ordinary_session,account_profile,1,account_view->Point({}),event,
        account_view->Block(1),account_view->State(1),account_view->Authorizations(1));
    CHECK(account_first.account.Delivery().sequence==1&&account_first.account.ParentSnapshotRevision()==1);
    CHECK(account_first.account.Scan().BalanceUna()==5000);
    CHECK(Account::ReadForReplay(ordinary,ordinary_session,account_profile,*account_view).revision==account_first.revision);
    const auto ordinary_first=RuntimeOrdinaryDelivery::ReadForWallet(ordinary,ordinary_session);
    CHECK(ordinary_first.has_value());CHECK(ordinary_first->cursor==first.cursor);
    CHECK(ordinary_count("SELECT height FROM tip WHERE rowid=1")==int64_t(c.height));
    CHECK(ordinary_count("SELECT count(*) FROM transactions WHERE is_coinbase=1")==1);
    CHECK(ordinary_count("SELECT time FROM transactions WHERE is_coinbase=1")==int64_t(block.Header().timestamp));
    // Net chain undo omits the one same-block output consumed by its child.
    CHECK(ordinary_count("SELECT count(*) FROM utxos WHERE is_spent=0")==int64_t(count));
    CHECK(ordinary_count("SELECT count(*) FROM utxos WHERE is_spent=1")==int64_t(undo.spent.size()+1));
    CHECK(RuntimeOrdinaryDelivery::ApplyForWallet(ordinary,ordinary_session,event).cursor==first.cursor);
    { const auto lease=ordinary.AcquireDatabaseLease();
      const auto reads=recovery_source_reads;
      failure([&]{(void)recover();},"released caller lease");
      CHECK(recovery_source_reads==reads); }
    bool source_unleased=false;
    CHECK(RuntimeWalletRecoveryTestAccess::Resume([&](RuntimeOutboxCursor cursor,size_t limit) {
        if(!source_unleased) {
            auto competing=std::async(std::launch::async,[&]{return ordinary.AcquireDatabaseLease()->Session();});
            CHECK(competing.wait_for(std::chrono::seconds(2))==std::future_status::ready);
            CHECK(competing.get()==ordinary_session);source_unleased=true;
        }
        return source(cursor,limit);
    },ordinary,index,ordinary_session).applied.cursor==first.cursor);
    CHECK(source_unleased);
    const auto recovered_first=recover();CHECK(recovered_first.applied.cursor==first.cursor);
    CHECK(recovered_first.observed_head==first.cursor);
    failure([&]{(void)RuntimeWalletRecoveryTestAccess::Resume([&](RuntimeOutboxCursor cursor,size_t limit) {
        auto page=source(cursor,limit);
        if(cursor==first.cursor)page.after_tip->first=H(249);
        return page;
    },ordinary,index,ordinary_session);},"source position mismatch");
    ordinary.open("ordinary");
    failure([&]{(void)RuntimeOrdinaryDelivery::ReadForWallet(ordinary,ordinary_session);},"selection changed");
    ordinary_session=ordinary.AcquireDatabaseLease()->Session();
    CHECK(RuntimeOrdinaryDelivery::ReadForWallet(ordinary,ordinary_session)->cursor==first.cursor);
    auto ordinary_skip=event;ordinary_skip.cursor.sequence+=2;ordinary_skip.previous_digest=first.cursor.digest;
    failure([&]{RuntimeOrdinaryDelivery::ApplyForWallet(ordinary,ordinary_session,ordinary_skip);},"source discontinuity");

    for(const auto& spent:undo.spent)CHECK(index.GetUTXO(TxId(spent.prev_txid),spent.prev_vout)->spend_height==int(c.height));
    failure([&]{(void)RuntimeIndexDeliveryTestAccess::Read(index,"other-wallet");},"ownership changed");
    CHECK(!index.SetMetadata("runtime_delivery:v1:receipt","forged") && !index.DeleteMetadata("runtime_delivery:v1:receipt"));
    CHECK(index.Initialize());register_scripts(index);
    CHECK(RuntimeIndexDeliveryTestAccess::Read(index,wallet_identity)->cursor==first.cursor);
    auto skipped=event;skipped.cursor.sequence+=2;skipped.previous_digest=first.cursor.digest;
    failure([&]{RuntimeIndexDeliveryTestAccess::Apply(index,wallet_identity,skipped);},"source discontinuity");
    index.RegisterAddress({0x51,0x51},"m/86'/1448'/0'/0/1");
    failure([&]{(void)RuntimeIndexDeliveryTestAccess::Read(index,wallet_identity);},"ownership changed");
    index.ClearRegisteredAddresses();register_scripts(index);
    // Actual indexed rollback gives the next checked source event.
    BlockStorage files;CHECK(files.init(path)==Status::Ok);
    auto disk_index=DiskIndex(db,block.Header(),c.height);const auto parent=RequiredValue(db.getHeader(c.parent_hash));
    ConsensusUTXOSet live;live.ReplaceForestGuarded(forest);live.SetBestBlock(c.block_hash,c.height);
    CHECK(db.forEachUTXO([&](const uint256& hash,uint32_t n,const Coin& coin){CHECK(live.AddCoin(OutPoint(TxId(hash),n),MemoryCoin(coin)));return true;})==Status::Ok);
    auto rollback=PreparedOrchardChainstateWrite::DisconnectIndexed(lock,db,token,files,disk_index,live,c,block,parent,forest,true);
    rollback->Commit();rollback.reset();
    const auto down=ReadRuntimeOutboxUnderLock(db,c,first.cursor).events.front();
    CHECK(down.direction==RuntimeBlockDirection::Disconnect);
    ordinary_sql("CREATE TRIGGER ordinary_reject_undo BEFORE DELETE ON utxos BEGIN SELECT RAISE(ABORT,'ordinary undo'); END");
    failure([&]{(void)recover();},"statement failed");
    CHECK(RuntimeOrdinaryDelivery::ReadForWallet(ordinary,ordinary_session)->cursor==first.cursor);
    const auto second=*RuntimeIndexDelivery::ReadForWallet(ordinary,index,ordinary_session);
    CHECK(second.cursor==down.cursor);
    CHECK(ordinary_count("SELECT count(*) FROM utxos WHERE is_spent=0")==int64_t(count));
    // Validate the ahead store against source before allowing lagging effects.
    failure([&]{(void)RuntimeWalletRecoveryTestAccess::Resume([&](RuntimeOutboxCursor cursor,size_t limit) {
        if(cursor==second.cursor)throw std::runtime_error("ahead source unavailable");
        return source(cursor,limit);
    },ordinary,index,ordinary_session);},"ahead source unavailable");
    CHECK(RuntimeOrdinaryDelivery::ReadForWallet(ordinary,ordinary_session)->cursor==first.cursor);
    ordinary_sql("DROP TRIGGER ordinary_reject_undo");
    ordinary_sql("CREATE TRIGGER ordinary_reject_restore BEFORE UPDATE OF is_spent ON utxos BEGIN SELECT RAISE(ABORT,'ordinary restore'); END");
    failure([&]{RuntimeOrdinaryDelivery::ApplyForWallet(ordinary,ordinary_session,down);},"statement failed");
    CHECK(RuntimeOrdinaryDelivery::ReadForWallet(ordinary,ordinary_session)->cursor==first.cursor);
    CHECK(ordinary_count("SELECT count(*) FROM utxos WHERE is_spent=0")==int64_t(count));
    ordinary_sql("DROP TRIGGER ordinary_reject_restore");

    ordinary.open("ordinary");ordinary_session=ordinary.AcquireDatabaseLease()->Session();
    CHECK(recover().applied.cursor==second.cursor);
    CHECK(recover().applied.cursor==second.cursor);
    CHECK(ordinary_count("SELECT count(*) FROM utxos")==int64_t(baseline));
    CHECK(ordinary_count("SELECT count(*) FROM utxos WHERE is_spent=1")==0);

    CHECK(second.cursor==down.cursor && second.tip_hash==c.parent_hash && index.GetUTXOCount().value()==baseline);
    for(const auto& spent:undo.spent)if(spent.height<c.height)CHECK(!index.GetUTXO(TxId(spent.prev_txid),spent.prev_vout)->spend_height);
    // Cross below activation through the existing historical source log. These
    // are real historical body/effect identities, not historical consensus replay.
    auto next_cursor=second.cursor;
    CHECK(!index.GetUTXO(TxId(H(250)),UINT32_MAX));
    for(auto direction:{RuntimeBlockDirection::Disconnect,RuntimeBlockDirection::Connect}) {
        auto prepared=PreparedHistoricalRuntimeOutbox::PrepareUnderLock(db,c,historical,c.height-1,direction);CHECK(prepared);
        rocksdb::WriteBatch canonical;
        Tip(db,direction==RuntimeBlockDirection::Connect?historical.GetHash():historical.header.prev_block_hash,
            direction==RuntimeBlockDirection::Connect?c.height-1:c.height-2,canonical);
        prepared->StageOrTerminateUnderLock(db,canonical);Commit(db,canonical);
        const auto old_event=ReadRuntimeOutboxUnderLock(db,c,next_cursor,1).events.front();CHECK(!old_event.IsOrchardProfile());
        if(direction==RuntimeBlockDirection::Disconnect) {
            bool concurrent_prefix=false;
            failure([&]{(void)RuntimeWalletRecoveryTestAccess::Resume([&](RuntimeOutboxCursor cursor,size_t limit) {
                auto page=source(cursor,limit);
                if(!concurrent_prefix) {
                    concurrent_prefix=true;
                    CHECK(RuntimeIndexDelivery::ApplyForWallet(ordinary,index,ordinary_session,old_event).cursor==old_event.cursor);
                }
                return page;
            },ordinary,index,ordinary_session);},"stores changed during source read");
            CHECK(concurrent_prefix);
            CHECK(RuntimeOrdinaryDelivery::ReadForWallet(ordinary,ordinary_session)->cursor==next_cursor);
        }
        next_cursor=RuntimeIndexDeliveryTestAccess::Apply(index,wallet_identity,old_event).cursor;
        CHECK(RuntimeOrdinaryDelivery::ApplyForWallet(ordinary,ordinary_session,old_event).cursor==next_cursor);
        CHECK(ordinary_count("SELECT count(*) FROM utxos")==int64_t(baseline-(direction==RuntimeBlockDirection::Disconnect?1:0)));

        CHECK(index.GetUTXO(historical.vtx.front().GetTxid(),0).has_value()==(direction==RuntimeBlockDirection::Connect));
        CHECK(index.GetUTXOCount().value()==baseline-(direction==RuntimeBlockDirection::Disconnect?1:0));
    }
    // More than one maximum page of retained historical transitions must be
    // replayed in source order, despite every pair returning to the same tip.
    const auto append_historical=[&](RuntimeBlockDirection direction) {
        auto prepared=PreparedHistoricalRuntimeOutbox::PrepareUnderLock(db,c,historical,c.height-1,direction);CHECK(prepared);
        rocksdb::WriteBatch canonical;
        Tip(db,direction==RuntimeBlockDirection::Connect?historical.GetHash():historical.header.prev_block_hash,
            direction==RuntimeBlockDirection::Connect?c.height-1:c.height-2,canonical);
        prepared->StageOrTerminateUnderLock(db,canonical);Commit(db,canonical);
    };
    for(size_t i=0;i<65;++i) {
        append_historical(RuntimeBlockDirection::Disconnect);
        append_historical(RuntimeBlockDirection::Connect);
    }
    const auto captured=ReadRuntimeOutboxUnderLock(db,c,next_cursor,1).head;
    size_t replay_pages=0;bool extended=false;
    const auto catchup=RuntimeWalletRecoveryTestAccess::Resume([&](RuntimeOutboxCursor cursor,size_t limit) {
        auto page=source(cursor,limit);
        if(limit==128)++replay_pages;
        if(!extended && cursor==captured) {
            extended=true;append_historical(RuntimeBlockDirection::Disconnect);
            append_historical(RuntimeBlockDirection::Connect);
            page=source(cursor,limit);
        }
        return page;
    },ordinary,index,ordinary_session);
    CHECK(replay_pages==1 && extended);
    CHECK(catchup.applied.cursor==captured);
    CHECK(catchup.observed_head.sequence==captured.sequence+2);
    next_cursor=recover().applied.cursor;CHECK(next_cursor==catchup.observed_head);
    CHECK(ordinary_count("SELECT count(*) FROM utxos")==int64_t(baseline));
    // A selection change while obtaining source must refuse before any effect.
    const auto before_switch=next_cursor;
    bool switched=false;
    failure([&]{(void)RuntimeWalletRecoveryTestAccess::Resume([&](RuntimeOutboxCursor cursor,size_t limit) {
        auto page=source(cursor,limit);
        if(!switched){ordinary.open("ordinary");switched=true;}
        return page;
    },ordinary,index,ordinary_session);},"selection changed");
    ordinary_session=ordinary.AcquireDatabaseLease()->Session();
    CHECK(RuntimeOrdinaryDelivery::ReadForWallet(ordinary,ordinary_session)->cursor==before_switch);
    CHECK(recover().applied.cursor==before_switch);
    UtreexoForest parent_forest;std::string error;CHECK(storage::RestoreHistoricalForest(db,c.height-1,parent_forest,error)==Status::Ok);
    auto reconnect=PreparedOrchardChainstateWrite::ConnectIndexed(lock,db,token,files,disk_index,live,c,block,parent,parent_forest,{},true,false,FixtureRetirement(c));
    reconnect->Commit();reconnect.reset();
    auto again=ReadRuntimeOutboxUnderLock(db,c,next_cursor).events.front();
    CHECK(RuntimeIndexDeliveryTestAccess::Apply(index,wallet_identity,again).cursor==again.cursor && index.GetUTXOCount().value()==count);
    CHECK(RuntimeOrdinaryDelivery::ApplyForWallet(ordinary,ordinary_session,again).cursor==again.cursor);
    CHECK(ordinary_count("SELECT count(*) FROM utxos WHERE is_spent=0")==int64_t(count));
    // Account still at event 1 while transparent stores have crossed activation
    // and more than 128 source transitions. Recover only the lagging account.
    const auto all_view=RuntimeAccountReplayTestAccess::Capture(db,c);
    const auto all_recover=[&](const RuntimeAccountReplay& view){
        return RuntimeWalletRecoveryTestAccess::ResumeAccount(view,source,ordinary,index,ordinary_session);
    };
    {const auto lease=ordinary.AcquireDatabaseLease();failure([&]{(void)all_recover(*all_view);},"released caller lease");}
    failure([&]{(void)RuntimeWalletRecoveryTestAccess::ResumeAccount(*all_view,[&](RuntimeOutboxCursor cursor,size_t n){
        auto page=source(cursor,n);if(cursor==first.cursor)page.after_tip->first=H(249);return page;
    },ordinary,index,ordinary_session);},"account source position mismatch");
    CHECK(Account::ReadForReplay(ordinary,ordinary_session,account_profile,*all_view).revision==account_first.revision);
    bool account_changed=false;
    failure([&]{(void)RuntimeWalletRecoveryTestAccess::ResumeAccount(*all_view,[&](RuntimeOutboxCursor cursor,size_t n){
        auto page=source(cursor,n);
        if(!account_changed){
            account_changed=true;const auto lease=ordinary.AcquireDatabaseLease();
            auto before=Account::ReadForReplay(ordinary,ordinary_session,account_profile,*all_view);
            auto issued=before.account.IssueReceiver(orchard::WalletScope::External).first;
            next_receiver=issued.IssueReceiver(orchard::WalletScope::External).second.Raw();
            const auto seed=lease->CopyRecoverySeed(ordinary_session);orchard::Hash identity{};
            for(size_t i=0;i<32;++i)identity[i]=uint8_t(std::stoul(wallet_identity.substr(7+2*i,2),nullptr,16));
            CHECK(sqlite3_exec(lease->Database(),"BEGIN IMMEDIATE",nullptr,nullptr,nullptr)==SQLITE_OK);
            orchard::WalletSnapshotStore store(lease->Database(),{orchard::WalletNetwork::Regtest,c.domain.genesis_wire,identity,0},seed->Bytes());
            CHECK(store.StageReplaceRetaining(before.revision,issued.Encode())==before.revision+1);
            CHECK(sqlite3_exec(lease->Database(),"COMMIT",nullptr,nullptr,nullptr)==SQLITE_OK);
        }
        return page;
    },ordinary,index,ordinary_session);},"stores changed during source read");
    CHECK(account_changed);
    CHECK(all_recover(*all_view).applied.cursor==again.cursor);
    const auto caught=Account::ReadForReplay(ordinary,ordinary_session,account_profile,*all_view);
    CHECK(caught.account.Delivery().sequence==again.cursor.sequence&&caught.account.ParentSnapshotRevision()>1);
    CHECK(caught.account.Scan().BalanceUna()==5000);
    CHECK(caught.account.IssueReceiver(orchard::WalletScope::External).second.Raw()==next_receiver);
    CHECK(all_recover(*all_view).account_revision==caught.revision);
    auto final_down=PreparedOrchardChainstateWrite::DisconnectIndexed(lock,db,token,files,disk_index,live,c,block,parent,forest,true);
    final_down->Commit();final_down.reset();
    const auto down_view=RuntimeAccountReplayTestAccess::Capture(db,c);
    ordinary_sql("CREATE TRIGGER reject_account_recovery BEFORE UPDATE ON orchard_wallet_snapshots BEGIN SELECT RAISE(ABORT,'account recovery');END");
    failure([&]{(void)all_recover(*down_view);},"Orchard wallet storage integrity, transaction or I/O failure");
    CHECK(RuntimeIndexDelivery::ReadForWallet(ordinary,index,ordinary_session)->cursor==down_view->Head());
    CHECK(RuntimeOrdinaryDelivery::ReadForWallet(ordinary,ordinary_session)->cursor==down_view->Head());
    CHECK(Account::ReadForReplay(ordinary,ordinary_session,account_profile,*down_view).revision==caught.revision);
    ordinary_sql("DROP TRIGGER reject_account_recovery");
    const auto old_session=ordinary_session;ordinary.open("ordinary");ordinary_session=ordinary.AcquireDatabaseLease()->Session();
    failure([&]{(void)RuntimeWalletRecoveryTestAccess::ResumeAccount(*down_view,source,ordinary,index,old_session);},"selection changed");
    CHECK(all_recover(*down_view).applied.cursor==down_view->Head());
    const auto undone=Account::ReadForReplay(ordinary,ordinary_session,account_profile,*down_view);
    CHECK(undone.account.Scan().Checkpoint().block_hash==c.parent_hash&&undone.account.ParentSnapshotRevision()==0);
    CHECK(undone.account.Scan().BalanceUna()==0);
    CHECK(undone.account.IssueReceiver(orchard::WalletScope::External).second.Raw()==next_receiver);
    auto final_up=PreparedOrchardChainstateWrite::ConnectIndexed(lock,db,token,files,disk_index,live,c,block,parent,parent_forest,{},true,false,FixtureRetirement(c));
    final_up->Commit();final_up.reset();
    const auto up_view=RuntimeAccountReplayTestAccess::Capture(db,c);
    CHECK(all_recover(*up_view).applied.cursor==up_view->Head());
    again=up_view->Event(up_view->Head().sequence);
    // Discover nonconsecutive account numbers from actual snapshot rows. Every
    // row must authenticate/restore before advancing any lagging store.
    const auto enroll=[&](uint32_t number){
        const auto lease=ordinary.AcquireDatabaseLease();const auto seed=lease->CopyRecoverySeed(ordinary_session);
        const auto keys=orchard::WalletKeys::FromSeed(seed->Bytes(),number);
        auto initial=dinero::wallet::OrchardAccountState::Begin(c.domain,keys.ExportFullViewingKey(),c.activation_height,c.parent_hash)
            .IssueReceiver(orchard::WalletScope::External).first;
        orchard::Hash identity{};for(size_t i=0;i<32;++i)identity[i]=uint8_t(std::stoul(wallet_identity.substr(7+2*i,2),nullptr,16));
        CHECK(sqlite3_exec(lease->Database(),"BEGIN IMMEDIATE",nullptr,nullptr,nullptr)==SQLITE_OK);
        orchard::WalletSnapshotStore store(lease->Database(),{orchard::WalletNetwork::Regtest,c.domain.genesis_wire,identity,number},seed->Bytes());
        CHECK(store.StageReplaceRetaining(0,initial.Encode())==1);
        CHECK(sqlite3_exec(lease->Database(),"COMMIT",nullptr,nullptr,nullptr)==SQLITE_OK);
    };
    const Account::Profile other_profile{c.domain,c.activation_height,7};
    const auto enrolled_recover=[&](const RuntimeAccountReplay& view){
        return RuntimeWalletRecoveryTestAccess::ResumeEnrolled(view,source,ordinary,index,ordinary_session);
    };
    const auto primary=Account::ReadForReplay(ordinary,ordinary_session,account_profile,*up_view);
    enroll(7);
    failure([&]{(void)enrolled_recover(*up_view);},"account baseline reconciliation required");
    CHECK(Account::ReadForReplay(ordinary,ordinary_session,account_profile,*up_view).revision==primary.revision);
    auto other=Account::Connect(ordinary,ordinary_session,other_profile,1,account_view->Point({}),event,
        account_view->Block(1),account_view->State(1),account_view->Authorizations(1));
    CHECK(other.account.Scan().BalanceUna()==0); // Account 0's note is not account 7's.
    ordinary_sql("CREATE TEMP TABLE saved_account AS SELECT * FROM orchard_wallet_snapshots WHERE account=7");
    ordinary_sql("UPDATE orchard_wallet_snapshots SET sealed=zeroblob(length(sealed)) WHERE account=7");
    failure([&]{(void)enrolled_recover(*up_view);},"Orchard wallet storage integrity, transaction or I/O failure");
    CHECK(RuntimeOrdinaryDelivery::ReadForWallet(ordinary,ordinary_session)->cursor==up_view->Head());
    ordinary_sql("UPDATE orchard_wallet_snapshots SET sealed=(SELECT sealed FROM saved_account) WHERE account=7");
    // A different persistent wallet identity cannot be filtered out silently.
    ordinary_sql("UPDATE orchard_wallet_snapshots SET wallet_id=zeroblob(32) WHERE account=7");
    failure([&]{(void)enrolled_recover(*up_view);},"ownership or state mismatch");
    ordinary_sql("UPDATE orchard_wallet_snapshots SET wallet_id=(SELECT wallet_id FROM saved_account) WHERE account=7");
    const auto enrolled=enrolled_recover(*up_view);
    CHECK(enrolled.applied.cursor==up_view->Head()&&enrolled.account_revisions.size()==2);
    CHECK(enrolled.account_revisions[0].first==0&&enrolled.account_revisions[0].second==primary.revision);
    CHECK(enrolled.account_revisions[1].first==7);
    other=Account::ReadForReplay(ordinary,ordinary_session,other_profile,*up_view);
    CHECK(other.account.Delivery().sequence==up_view->Head().sequence&&other.account.Scan().BalanceUna()==0);
    const auto other_receiver=other.account.IssueReceiver(orchard::WalletScope::External).second.Raw();
    // Failure in the second account retains the first account's committed undo.
    auto multi_down=PreparedOrchardChainstateWrite::DisconnectIndexed(lock,db,token,files,disk_index,live,c,block,parent,forest,true);
    multi_down->Commit();multi_down.reset();
    const auto multi_view=RuntimeAccountReplayTestAccess::Capture(db,c);
    ordinary_sql("CREATE TRIGGER reject_second_account BEFORE UPDATE ON orchard_wallet_snapshots WHEN NEW.account=7 BEGIN SELECT RAISE(ABORT,'second account');END");
    failure([&]{(void)enrolled_recover(*multi_view);},"Orchard wallet storage integrity, transaction or I/O failure");
    const auto primary_undone=Account::ReadForReplay(ordinary,ordinary_session,account_profile,*multi_view);
    CHECK(primary_undone.account.Delivery().sequence==multi_view->Head().sequence);
    CHECK(primary_undone.account.Scan().BalanceUna()==0);
    CHECK(Account::ReadForReplay(ordinary,ordinary_session,other_profile,*multi_view).revision==other.revision);
    CHECK(RuntimeIndexDelivery::ReadForWallet(ordinary,index,ordinary_session)->cursor==multi_view->Head());
    CHECK(RuntimeOrdinaryDelivery::ReadForWallet(ordinary,ordinary_session)->cursor==multi_view->Head());
    ordinary_sql("DROP TRIGGER reject_second_account");
    ordinary.open("ordinary");ordinary_session=ordinary.AcquireDatabaseLease()->Session();
    const auto retried=enrolled_recover(*multi_view);
    CHECK(retried.account_revisions.size()==2&&retried.account_revisions[0].second==primary_undone.revision);
    CHECK(Account::ReadForReplay(ordinary,ordinary_session,other_profile,*multi_view).account.IssueReceiver(orchard::WalletScope::External).second.Raw()==other_receiver);
    // A newly enrolled row during source acquisition invalidates the captured
    // account roster, even when existing stores were already at the head.
    bool roster_changed=false;
    failure([&]{(void)RuntimeWalletRecoveryTestAccess::ResumeEnrolled(*multi_view,[&](RuntimeOutboxCursor cursor,size_t n){
        const auto page=source(cursor,n);
        if(!roster_changed){
            roster_changed=true;enroll(19);
            const Account::Profile added{c.domain,c.activation_height,19};
            (void)Account::Connect(ordinary,ordinary_session,added,1,account_view->Point({}),event,
                account_view->Block(1),account_view->State(1),account_view->Authorizations(1));
        }
        return page;
    },ordinary,index,ordinary_session);},"stores changed during source read");
    CHECK(roster_changed);
    const auto complete=enrolled_recover(*multi_view);
    CHECK(complete.account_revisions.size()==3&&complete.account_revisions[2].first==19);
    CHECK(complete.applied.cursor==multi_view->Head());
    ordinary_sql("CREATE TEMP TABLE deleted_account AS SELECT * FROM orchard_wallet_snapshots WHERE account=7");
    bool roster_deleted=false;
    failure([&]{(void)RuntimeWalletRecoveryTestAccess::ResumeEnrolled(*multi_view,[&](RuntimeOutboxCursor cursor,size_t n){
        const auto page=source(cursor,n);
        if(!roster_deleted){roster_deleted=true;ordinary_sql("DELETE FROM orchard_wallet_snapshots WHERE account=7");}
        return page;
    },ordinary,index,ordinary_session);},"stores changed during source read");
    CHECK(roster_deleted);
    ordinary_sql("INSERT INTO orchard_wallet_snapshots SELECT * FROM deleted_account");
    CHECK(enrolled_recover(*multi_view).account_revisions.size()==3);
    auto multi_up=PreparedOrchardChainstateWrite::ConnectIndexed(lock,db,token,files,disk_index,live,c,block,parent,parent_forest,{},true,false,FixtureRetirement(c));
    multi_up->Commit();multi_up.reset();
    const auto multi_up_view=RuntimeAccountReplayTestAccess::Capture(db,c);
    CHECK(enrolled_recover(*multi_up_view).account_revisions.size()==3);
    CHECK(Account::ReadForReplay(ordinary,ordinary_session,account_profile,*multi_up_view).account.Scan().BalanceUna()==5000);
    again=multi_up_view->Event(multi_up_view->Head().sequence);
    ordinary_sql("DROP TRIGGER runtime_ordinary_utxos_UPDATE");
    failure([&]{(void)RuntimeOrdinaryDelivery::ReadForWallet(ordinary,ordinary_session);},"guard unavailable");
    ordinary_sql("CREATE TRIGGER runtime_ordinary_utxos_UPDATE AFTER UPDATE ON utxos BEGIN UPDATE wallet_meta SET runtime_ordinary_invalid=1,runtime_ordinary_receipt=NULL WHERE id=1 AND runtime_ordinary_receipt IS NOT NULL; END");
    // Existing ordinary writer changes atomically revoke source completion.
    ordinary_sql("UPDATE utxos SET is_spent=0");
    failure([&]{(void)RuntimeOrdinaryDelivery::ReadForWallet(ordinary,ordinary_session);},"invalidated");
    failure([&]{RuntimeOrdinaryDelivery::ApplyForWallet(ordinary,ordinary_session,again);},"invalidated");

    CHECK(index.Initialize());register_scripts(index);
    CHECK(RuntimeIndexDeliveryTestAccess::Read(index,wallet_identity)->cursor==again.cursor);
    // Missing invalidation guards refuse even exact replay rather than being repaired silently.
    std::string trigger;
    { sqlite3_stmt* stmt=nullptr;CHECK(sqlite3_prepare_v2(raw,"SELECT sql FROM sqlite_master WHERE name='runtime_delivery_UPDATE'",-1,&stmt,nullptr)==SQLITE_OK);
      CHECK(sqlite3_step(stmt)==SQLITE_ROW);trigger=reinterpret_cast<const char*>(sqlite3_column_text(stmt,0));
      CHECK(sqlite3_step(stmt)==SQLITE_DONE);sqlite3_finalize(stmt); }
    sql("DROP TRIGGER runtime_delivery_UPDATE");
    failure([&]{RuntimeIndexDeliveryTestAccess::Apply(index,wallet_identity,again);},"guard unavailable");
    sql(trigger.c_str());
    CHECK(RuntimeIndexDeliveryTestAccess::Read(index,wallet_identity)->cursor==again.cursor);
    // A zero-owned-output index must invalidate its receipt on reset, too.
    { dinero::UTXOIndex empty((wallet.path/"empty.sqlite").string());CHECK(empty.Initialize());
      CHECK(RuntimeIndexDeliveryTestAccess::Apply(empty,"empty-wallet",event).cursor==event.cursor);
      CHECK(empty.GetUTXOCount().value()==0 && empty.ClearAll());
      failure([&]{(void)RuntimeIndexDeliveryTestAccess::Read(empty,"empty-wallet");},"invalidated"); }
    // Existing production writers cannot leave a stale successful receipt.
    const auto& spent=undo.spent.front();CHECK(index.SpendUTXO(TxId(spent.prev_txid),spent.prev_vout,c.height));
    failure([&]{(void)RuntimeIndexDeliveryTestAccess::Read(index,wallet_identity);},"invalidated");
    CHECK(index.ClearAll());
    failure([&]{RuntimeIndexDeliveryTestAccess::Apply(index,wallet_identity,event);},"invalidated");
}
